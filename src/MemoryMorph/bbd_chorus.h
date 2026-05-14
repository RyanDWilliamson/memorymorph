#pragma once
#include <cmath>
#include <cstddef>
#include "daisysp.h"

// ── BBD chorus — Roland SRE-555 / SDD-320 multi-algorithm ────────────────────
//
// Mono-in / stereo-out chorus with three switchable algorithms (SW2 in the
// SDD-555 control map):
//
//   ProcessBbd          — SW2 UP — Roland CE-1 / SRE-555 BBD chorus
//   ProcessEventide     — SW2 MID — Eventide H910 "Micropitch" detune chorus
//   ProcessDimensionD   — SW2 DOWN — Roland SDD-320 Dimension D cross-channel
//
// Three structural elements are shared across algorithms:
//
//   1. Trapezoidal LFO (rise 20% → hold 30% → fall 20% → hold 30%)
//      The flat top and bottom mean the delay is briefly constant (no pitch
//      shift), then ramps linearly during rise/fall (controlled pitch excursion).
//      Audibly this is "shimmer + settle" rather than the continuous warble of
//      a sine-modulated chorus.
//
//   2. Pre/De-emphasis (+6 dB / −6 dB shelf at 3 kHz)
//      Classic BBD noise-reduction trick: boost HF before the delay (where BBD
//      noise is fixed), cut HF after (which restores flat response and attenuates
//      accumulated BBD clock noise). All three algorithms use the same pair.
//
//   3. Mono delay line (2400 samples = 50 ms headroom)
//      Single write side serves all algorithms. Each algorithm reads it
//      differently — BBD uses two LFO-modulated taps, Dimension D adds
//      cross-channel HPF subtraction, Eventide adds a short pre-delay before
//      the shared SDRAM PitchShifter.
//
// PitchShifter is shared with DMM's shimmer voice (SDRAM). The mode-switch
// re-configures transposition: +0.20 st / fun=0 for Eventide, +12 st /
// fun=0.3 for DMM shimmer. Only one mode runs at a time so there's no contention.
//
// Memory: ~9.7 KB SRAM. No new SDRAM (PitchShifter is already allocated).

struct BbdChorus {
  // ── Constants ──────────────────────────────────────────────────────────────
  // 2400 samples at 48 kHz = 50 ms — comfortable headroom for ~5–25 ms chorus
  // and Dimension D's shorter ~5–15 ms range in Phase 4.
  static constexpr size_t kDelaySize = 2400;
  static constexpr float  kCenterMs  = 12.f;
  static constexpr float  kSwingMs   =  8.f;
  static constexpr float  kEmphHz    = 3000.f;
  static constexpr float  kTwoPi     = 6.28318530718f;

  // Trapezoidal segments — rise/fall ratio is the SDD-320 character control.
  // 20/30/20/30 produces ~40% of the period in pitch-stable holds.
  static constexpr float kRise   = 0.20f;
  static constexpr float kHoldHi = 0.30f;
  static constexpr float kFall   = 0.20f;

  // ── State ──────────────────────────────────────────────────────────────────
  // No `= {0}` on buf — that initialiser would force ~9.6 KB of zeros into
  // FLASH `.data`. BSS clears it at startup since the BbdChorus instance has
  // static storage duration.
  float  buf[kDelaySize];
  size_t write_idx = 0;
  float  lfo_phase  = 0.f;
  float  lfo_inc    = 0.f;
  float  depth      = 1.f;
  float  center_smp = 0.f;
  float  swing_smp  = 0.f;

  float pre_z  = 0.f;   // pre-emphasis LPF state
  float de_l_z = 0.f;   // de-emphasis LPF state (L tap)
  float de_r_z = 0.f;   // de-emphasis LPF state (R tap)
  float emph_c = 0.f;   // shared shelf coefficient = 1 - exp(-2π·f/sr)

  // Dimension D cross-channel HPF state. HPF(x) = x - LPF(x); the LPF
  // state lives here. xfeed_c is a 1-pole coefficient at ~800 Hz, the
  // cutoff that gives Dimension D's "wider than stereo" psychoacoustic feel
  // without summing badly to mono.
  float xfeed_lpf_l_z = 0.f;
  float xfeed_lpf_r_z = 0.f;
  float xfeed_c       = 0.f;

  // Pointer to the shared SDRAM PitchShifter (used by ProcessEventide).
  // Set in Init(); may be null if Eventide mode is unused.
  daisysp::PitchShifter* pitch_ = nullptr;

  // ── Init / Reset ──────────────────────────────────────────────────────────

  // p is the shared SDRAM PitchShifter. The caller is responsible for
  // configuring its transposition / fun on mode switch — BbdChorus does
  // not own those settings because they swap between SDD-555 Eventide
  // (+0.20 st, fun=0) and DMM shimmer (+12 st, fun=0.3).
  void Init(float sr, daisysp::PitchShifter* p = nullptr) {
    pitch_     = p;
    center_smp = kCenterMs * 0.001f * sr;
    swing_smp  = kSwingMs  * 0.001f * sr;
    emph_c     = 1.f - expf(-kTwoPi * kEmphHz   / sr);
    xfeed_c    = 1.f - expf(-kTwoPi * 800.f      / sr);
    SetRate(0.5f, sr);
    Reset();
  }

  void Reset() {
    for (size_t i = 0; i < kDelaySize; ++i) buf[i] = 0.f;
    write_idx = 0;
    lfo_phase = 0.f;
    pre_z = de_l_z = de_r_z = 0.f;
    xfeed_lpf_l_z = xfeed_lpf_r_z = 0.f;
  }

  void SetRate(float hz, float sr) { lfo_inc = hz / sr; }
  void SetDepth(float d)           { depth   = d; }

  // ── BBD chorus per-sample: mono in, stereo out ────────────────────────────
  inline void ProcessBbd(float in, float& outL, float& outR) {
    // 1. Pre-emphasis: x + HPF(x) = +6 dB shelf above 3 kHz.
    pre_z += emph_c * (in - pre_z);
    const float pre_out = in + (in - pre_z);

    // 2. Mono write side.
    buf[write_idx] = pre_out;

    // 3. Trapezoidal LFO — L = phase, R = phase + 180°.
    lfo_phase += lfo_inc;
    if (lfo_phase >= 1.f) lfo_phase -= 1.f;
    float phase_r = lfo_phase + 0.5f;
    if (phase_r >= 1.f) phase_r -= 1.f;

    // 4. Modulated delay times.
    const float dl = center_smp + Trapezoid(lfo_phase) * swing_smp * depth;
    const float dr = center_smp + Trapezoid(phase_r)   * swing_smp * depth;

    // 5. Linear-interpolated reads. Plenty for chorus-scale modulation rates;
    //    higher-order interpolation would only matter for fast vibrato.
    const float read_l = ReadInterp(dl);
    const float read_r = ReadInterp(dr);

    // 6. De-emphasis: 0.5·(x + LPF) = −6 dB shelf above 3 kHz. Net response
    //    around the BBD is flat, but BBD clock noise is attenuated.
    de_l_z += emph_c * (read_l - de_l_z);
    de_r_z += emph_c * (read_r - de_r_z);
    outL = 0.5f * (read_l + de_l_z);
    outR = 0.5f * (read_r + de_r_z);

    // 7. Advance write head.
    if (++write_idx >= kDelaySize) write_idx = 0;
  }

  // ── Eventide H910 "Micropitch" — short pre-delay + shared pitch shifter ───
  // No LFO modulation in this algorithm — the pitch shift IS the chorus motion.
  // Two short reads at different delays decorrelate L/R; the same mono pitched
  // signal is mixed into both for the characteristic Eventide chorus feel.
  // PitchShifter transposition is configured at mode switch (+0.20 st by default).
  inline void ProcessEventide(float in, float& outL, float& outR) {
    pre_z += emph_c * (in - pre_z);
    const float pre_out = in + (in - pre_z);
    buf[write_idx] = pre_out;

    // Static short reads — ~6 ms L, ~10 ms R for stereo decorrelation.
    const float dl = center_smp * 0.5f;
    const float dr = center_smp * 0.5f + swing_smp * 0.5f;
    const float read_l = ReadInterp(dl);
    const float read_r = ReadInterp(dr);

    de_l_z += emph_c * (read_l - de_l_z);
    de_r_z += emph_c * (read_r - de_r_z);
    const float dry_l = 0.5f * (read_l + de_l_z);
    const float dry_r = 0.5f * (read_r + de_r_z);

    // Shared mono PitchShifter — read once, mix into both channels.
    float ps_in = dry_l;
    const float pitched = pitch_ ? pitch_->Process(ps_in) : dry_l;

    outL = 0.5f * (dry_l + pitched);
    outR = 0.5f * (dry_r + pitched);

    if (++write_idx >= kDelaySize) write_idx = 0;
  }

  // ── Dimension D — modulated delay + cross-channel HPF with polarity invert ─
  // Same trapezoidal LFO as BBD but with half the swing (subtler), then the
  // characteristic Roland routing:
  //   outL = wetL − HPF(wetR)
  //   outR = wetR − HPF(wetL)
  // The minus sign + HPF cancels some low-frequency cross-talk while
  // reinforcing the highs — "wider than stereo" without audible modulation.
  inline void ProcessDimensionD(float in, float& outL, float& outR) {
    pre_z += emph_c * (in - pre_z);
    const float pre_out = in + (in - pre_z);
    buf[write_idx] = pre_out;

    lfo_phase += lfo_inc;
    if (lfo_phase >= 1.f) lfo_phase -= 1.f;
    float phase_r = lfo_phase + 0.5f;
    if (phase_r >= 1.f) phase_r -= 1.f;

    // Half-swing — Dimension D is much subtler than CE-1 chorus.
    const float dl = center_smp + Trapezoid(lfo_phase) * swing_smp * depth * 0.5f;
    const float dr = center_smp + Trapezoid(phase_r)   * swing_smp * depth * 0.5f;
    const float read_l = ReadInterp(dl);
    const float read_r = ReadInterp(dr);

    de_l_z += emph_c * (read_l - de_l_z);
    de_r_z += emph_c * (read_r - de_r_z);
    const float wetL = 0.5f * (read_l + de_l_z);
    const float wetR = 0.5f * (read_r + de_r_z);

    // Cross-channel HPF: HPF(x) = x - LPF(x). Track LPF state per direction.
    xfeed_lpf_l_z += xfeed_c * (wetR - xfeed_lpf_l_z);
    xfeed_lpf_r_z += xfeed_c * (wetL - xfeed_lpf_r_z);
    const float hpf_r_into_l = wetR - xfeed_lpf_l_z;
    const float hpf_l_into_r = wetL - xfeed_lpf_r_z;

    outL = wetL - hpf_r_into_l;
    outR = wetR - hpf_l_into_r;

    if (++write_idx >= kDelaySize) write_idx = 0;
  }

private:
  // Piecewise-linear trapezoid: rise (-1→+1), hold high, fall (+1→-1), hold low.
  inline float Trapezoid(float p) const {
    if (p < kRise)               return p * (2.f / kRise) - 1.f;
    const float p1 = kRise + kHoldHi;
    if (p < p1)                  return 1.f;
    const float p2 = p1 + kFall;
    if (p < p2)                  return 1.f - (p - p1) * (2.f / kFall);
    return -1.f;
  }

  // Linear interpolated read, delay in samples, looking back from write_idx.
  inline float ReadInterp(float delay) const {
    const int kDS    = (int)kDelaySize;
    const int d_int  = (int)delay;
    const float frac = delay - (float)d_int;
    int r0 = ((int)write_idx - d_int + kDS) % kDS;
    int r1 = (r0 - 1 + kDS) % kDS;
    return buf[r0] + frac * (buf[r1] - buf[r0]);
  }
};
