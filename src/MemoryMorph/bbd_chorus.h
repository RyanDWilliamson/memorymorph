#pragma once
#include <cmath>
#include <cstddef>
#include "daisysp.h"
#include "constants.h"

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
//   1. Sine LFO
//      Earlier revisions used a trapezoid (rise/hold/fall/hold), but the slope
//      discontinuities at the rise→hold and hold→fall corners were audible as
//      a square / sawtooth edge each cycle. The real CE-1 and SDD-320 use a
//      digital LFO whose output is integrated by the BBD clock divider — the
//      net waveform driving the delay tap is approximately sinusoidal. Sine
//      gives smooth, classic chorus motion with no audible corner artefact.
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
// re-configures transposition: +0.07 st / fun=0 for Eventide H910 micro-pitch,
// +12 st / fun=0.3 for DMM shimmer. Only one mode runs at a time so there's no
// contention.
//
// Reference targets (canonical hardware settings):
//   CE-1     : Rate ≈ 0.5 Hz, Intensity ~12:00–1:00, Level unity, no clipping
//              — Boss CE-1 service notes
//   H910     : +7 cents (single-shifter approximation of the H910 ±7c dual),
//              pre-delay ~20 ms, mix ~30%, feedback 0
//              — Eventide H910 hardware manual
//   SDD-320  : "Buttons 1+4" — widest preset, fixed internal LFO ~0.3 Hz,
//              full cross-channel polarity-inverted HPF spread, 100% wet
//              — Roland SDD-320 owner's manual
//
// Memory: ~9.7 KB SRAM. No new SDRAM (PitchShifter is already allocated).

struct BbdChorus {
  // ── Constants ──────────────────────────────────────────────────────────────
  // 2400 samples at 48 kHz = 50 ms — comfortable headroom for the CE-1 BBD
  // (≈7.5 ms center, ±4 ms swing) and the H910 pre-delay tap (≈20 ms).
  static constexpr size_t kDelaySize    = 2400;
  static constexpr float  kCenterMs     =  7.5f;  // CE-1 / MN3002 BBD center
  static constexpr float  kSwingMs      =  4.0f;  // CE-1 "intensity 12:00–1:00"
  static constexpr float  kEmphHz       = 3000.f;
  static constexpr float  kEventidePreMs = 20.f;  // H910 micro-pitch pre-delay
  // H910 wet mix raised to 50% — single-shifter approximation of the ±7c dual
  // needs more level than the canonical 30% to register as audible widening
  // (the original 30% with a +0.07 st shift was inaudible on guitar input).
  static constexpr float  kEventideMix   = 0.50f;

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
  float eventide_pre_smp = 0.f;  // H910 pre-delay tap in samples (20 ms @ sr)

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
    pitch_           = p;
    center_smp       = kCenterMs      * 0.001f * sr;
    swing_smp        = kSwingMs       * 0.001f * sr;
    eventide_pre_smp = kEventidePreMs * 0.001f * sr;
    emph_c           = OnePoleCoeff(kEmphHz, sr);
    xfeed_c          = OnePoleCoeff(800.f,    sr);
    // 0.5 Hz is CE-1 canonical; SDD-320 internal is ~0.3 Hz — overridden
    // per-algorithm by the caller via SetRate() before each block.
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

    // 4. Modulated delay times (sine LFO — smooth, no slope discontinuities).
    const float dl = center_smp + LfoShape(lfo_phase) * swing_smp * depth;
    const float dr = center_smp + LfoShape(phase_r)   * swing_smp * depth;

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

  // ── Eventide H910 "Micropitch" — pre-delay + shared pitch shifter ─────────
  // Canonical hardware settings: ±7 cents dual-shifter, 15–25 ms pre-delay,
  // 25–35% wet, 0–10% feedback. We have one shared SDRAM PitchShifter, so the
  // dual-shifter ±7c image is approximated by a single +7c shifter (set at the
  // mode-switch site) summed with a 20 ms pre-delayed dry tap on the opposite
  // channel — the delay + pitch decorrelation reads as the H910 micro-pitch
  // thickening without true bidirectional detune. No LFO — the static pitch
  // shift IS the motion.
  inline void ProcessEventide(float in, float& outL, float& outR) {
    pre_z += emph_c * (in - pre_z);
    const float pre_out = in + (in - pre_z);
    buf[write_idx] = pre_out;

    // 20 ms pre-delay tap (H910 micro-pitch front-end).
    const float pre_tap = ReadInterp(eventide_pre_smp);
    de_r_z += emph_c * (pre_tap - de_r_z);
    const float pre_eq = 0.5f * (pre_tap + de_r_z);

    // Shared mono PitchShifter — operating on the pre-delayed signal so the
    // pitched and dry channels share the same delay reference. The DaisySP
    // PitchShifter::Process takes a non-const ref, so feed a mutable copy.
    float ps_in = pre_eq;
    const float pitched = pitch_ ? pitch_->Process(ps_in) : pre_eq;

    // 30% wet mix per the H910 manual reference. L carries the pitched +7c
    // image; R carries the 20 ms delayed dry — the two together read as the
    // classic Eventide micro-pitch widening.
    outL = (1.f - kEventideMix) * in + kEventideMix * pitched;
    outR = (1.f - kEventideMix) * in + kEventideMix * pre_eq;

    if (++write_idx >= kDelaySize) write_idx = 0;
  }

  // ── Dimension D — modulated delay + cross-channel HPF with polarity invert ─
  // Same trapezoidal LFO as BBD with full swing. "Buttons 1+4" on the SDD-320
  // is the widest preset — the routing makes it sound spacious without seasick
  // modulation:
  //   outL = wetL − HPF(wetR)
  //   outR = wetR − HPF(wetL)
  // The minus sign + HPF cancels some low-frequency cross-talk while
  // reinforcing the highs — "wider than stereo" with a glassy shimmer.
  inline void ProcessDimensionD(float in, float& outL, float& outR) {
    pre_z += emph_c * (in - pre_z);
    const float pre_out = in + (in - pre_z);
    buf[write_idx] = pre_out;

    lfo_phase += lfo_inc;
    if (lfo_phase >= 1.f) lfo_phase -= 1.f;
    float phase_r = lfo_phase + 0.5f;
    if (phase_r >= 1.f) phase_r -= 1.f;

    // Full swing — "Buttons 1+4" is the widest SDD-320 preset, not subtle.
    const float dl = center_smp + LfoShape(lfo_phase) * swing_smp * depth;
    const float dr = center_smp + LfoShape(phase_r)   * swing_smp * depth;
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
  // Smooth sine LFO — replaces the earlier trapezoid whose slope discontinuities
  // were audible as a square / sawtooth edge at the corner transitions.
  inline float LfoShape(float p) const {
    return sinf(p * kTwoPi);
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
