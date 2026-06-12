#pragma once
#include <cmath>
#include <cstdint>
#include "daisysp.h"
#include "constants.h"
#include "tube.h"
#include "diffuser.h"

using daisysp::DelayLine;
using daisysp::PitchShifter;

// ── The magnetic drum (one circular medium, 1 record + 4 playback heads) ──────
//
// Topology (DESIGN.md §"Core topology"):
//   read 4 taps @ ¼/½/¾/1 of the current drum length (1:2:3:4)  → wetRaw
//   wet = playback EQ( wetRaw )         : gap-loss LPF → de-emphasis → head bump
//   fb  = swell · darken(wet)           : per-pass narrowing (+ Shoegaze diffusion)
//   write( record-saturate( pre-emphasis(in + fb) ) + wire-noise )
//   Swell recirculates the SELECTED program → patterns multiply & darken.
//
// Head voicing (SW1): Octave injects a +12 shift into the feedback (rising
// shimmer); Sub blends a −12 shift into the output. Shoegaze (SW3) runs the
// feedback through an allpass diffuser. Age (K5) adds hiss, dropouts and grit.
//
// The large drum buffer + the PitchShifter live in SDRAM at file scope in
// echorec.cpp (DSY_SDRAM_BSS can't apply to struct members); this struct holds
// the taps / EQ / feedback / diffuser / dropout state and uses the supplied
// pointers. Taps use ReadHermite (cubic) since the heads move with speed/warble.

// 12 authentic T7E programs as 4-bit head masks (bit h ⇒ playback head h+1).
// Program 9 filled as 1+4 (the source matrix duplicated #6) — see DESIGN.md.
static constexpr uint8_t kEchoPrograms[12] = {
    0b0001, 0b0010, 0b0100, 0b1000,  //  1: H1   2: H2   3: H3   4: H4
    0b0011, 0b1010, 0b1100, 0b0101,  //  5: 1+2  6: 2+4  7: 3+4  8: 1+3
    0b1001, 0b0111, 0b1110, 0b1111,  //  9: 1+4 10:1+2+3 11:2+3+4 12: all
};

// Per-head playback level (head 4 = primary). TODO: tune on hardware.
static constexpr float kEchoHeadGain[4] = {0.5f, 0.6f, 0.8f, 1.0f};

enum class Voicing : uint8_t { Normal, Octave, Sub };

struct EchorecDrum {
  DelayLine<float, kDrumMaxSmp>* drum_  = nullptr;
  PitchShifter*                  pitch_ = nullptr;   // SDRAM, owned by glue

  uint8_t program_ = 11;        // active program index (0..11) — boot = all heads
  float   len_smp_ = 28800.f;   // current head-4 (full-rotation) period in samples
  float   swell_   = 0.f;       // 0..0.95

  // ── Pre/de-emphasis pair (stages 2 & 4) — matched α·HPF shelves ─────────────
  static constexpr float kEmphAlpha = 1.5f;
  float emph_c_     = 0.f;
  float pre_emph_z_ = 0.f;
  float de_emph_z_  = 0.f;

  // ── Gap-loss / bias HF-loss (stages 2/3) ───────────────────────────────────
  float gap_c_ = 0.f, gap_z_ = 0.f;

  // ── Head-bump resonant EQ (stage 4) ────────────────────────────────────────
  float bump_hi_c_ = 0.f, bump_lo_c_ = 0.f;
  float bump_hi_z_ = 0.f, bump_lo_z_ = 0.f;
  static constexpr float kBumpGain = 0.6f;

  // ── Per-pass darkening + Shoegaze diffusion (stage 5 / SW3) ─────────────────
  float    trail_c_    = 0.f, trail_lp_z_ = 0.f;
  Diffuser diffuser_;
  bool     shoegaze_   = false;

  // ── Wire noise floor (stage 3) ─────────────────────────────────────────────
  float    noise_amt_  = 0.f;
  uint32_t noise_seed_ = 0x1B0CA7EDu;

  // ── Age lofi: dropouts + grit (stage 3) ────────────────────────────────────
  float    drop_env_  = 1.f;    // amplitude gate (1 = open)
  float    drop_gate_c_ = 0.f;  // ~2 ms gate smoothing
  float    drop_prob_ = 0.f;    // per-sample trip probability (Age²)
  int      drop_len_  = 0;      // dropout length in samples
  int      drop_count_ = 0;
  bool     dropping_  = false;
  uint32_t drop_seed_ = 0xBEEF1234u;
  float    grit_gain_ = 1.f;    // extra record drive with Age

  // ── Head voicing (SW1) ─────────────────────────────────────────────────────
  Voicing voicing_ = Voicing::Normal;
  static constexpr float kOctBlend = 0.35f;  // octave into feedback (shimmer)
  static constexpr float kSubBlend = 0.5f;   // sub into output (thicken)

  TubeStage record_;            // magnetic-medium saturation (write path, in-loop)

  void Init(float sr, DelayLine<float, kDrumMaxSmp>* drum, PitchShifter* pitch) {
    drum_  = drum;
    pitch_ = pitch;
    record_.Init(sr);
    record_.SetSkew(0.06f);
    emph_c_      = OnePoleCoeff(2000.f, sr);
    bump_hi_c_   = OnePoleCoeff(160.f,  sr);
    bump_lo_c_   = OnePoleCoeff(45.f,   sr);
    gap_c_       = OnePoleCoeff(14000.f, sr);
    trail_c_     = OnePoleCoeff(6000.f, sr);
    drop_gate_c_ = OnePoleCoeff(80.f,   sr);   // ~2 ms gate slew
    diffuser_.Init();
    Reset();
  }
  void Reset() {
    pre_emph_z_ = de_emph_z_ = gap_z_ = 0.f;
    bump_hi_z_  = bump_lo_z_ = trail_lp_z_ = 0.f;
    drop_env_   = 1.f; dropping_ = false; drop_count_ = 0;
    diffuser_.Reset();
    record_.Reset();
  }

  void SetProgram(uint8_t idx)   { program_ = (idx < 12) ? idx : 11; }
  void SetLengthSmp(float n)     { len_smp_ = n; }
  void SetSwell(float s)         { swell_ = (s > 0.95f) ? 0.95f : (s < 0.f ? 0.f : s); }
  void SetTrailCutoff(float c)   { trail_c_ = c; }
  void SetPlaybackCutoff(float c){ gap_c_ = c; }
  void SetNoise(float amt)       { noise_amt_ = amt; }
  void SetShoegaze(bool on)      { shoegaze_ = on; }
  void SetVoicing(Voicing v)     { voicing_ = v; }

  // Age (0..1) → dropout rate/length + record grit. Age² so low Age is clean.
  void SetAgeLofi(float age, float sr) {
    drop_prob_ = age * age * 0.00004f;                 // ~3.8 trips/s at age=1
    drop_len_  = (int)(0.030f * sr * (0.5f + age));     // ~22–45 ms
    record_.SetSkew(0.06f + age * 0.15f);              // harder magnetic skew
    grit_gain_ = 1.f + age * 0.4f;                     // extra in-loop record drive
  }

  inline float Noise() {
    noise_seed_ = noise_seed_ * 1664525u + 1013904223u;
    return ((float)(int32_t)noise_seed_ * (1.f / 2147483648.f)) * noise_amt_;
  }

  // Random amplitude dropouts (stage 3 lofi). Returns the smoothed gate 0..1.
  inline float Dropout() {
    if (drop_prob_ > 0.f) {
      drop_seed_ = drop_seed_ * 1664525u + 1013904223u;
      const float r = (float)(drop_seed_ >> 8) * (1.f / 16777216.f);  // 0..1
      if (!dropping_ && r < drop_prob_) { dropping_ = true; drop_count_ = drop_len_; }
      if (dropping_ && --drop_count_ <= 0) dropping_ = false;
    }
    const float target = dropping_ ? 0.f : 1.f;
    drop_env_ += drop_gate_c_ * (target - drop_env_);
    return drop_env_;
  }

  // One sample. `in` already has the fixed Hot drive (×G) applied upstream;
  // `len_smp_` already includes the warble multiplier from the glue.
  inline float Process(float in) {
    const uint8_t mask = kEchoPrograms[program_];

    // ── Read the four fixed heads (cubic), sum the selected program ──────────
    float wet = 0.f;
    for (int h = 0; h < 4; ++h) {
      if (mask & (1u << h)) {
        const float d = len_smp_ * (float)(h + 1) * 0.25f;     // ¼/½/¾/1
        wet += drum_->ReadHermite(d) * kEchoHeadGain[h];
      }
    }

    // ── Playback EQ (stage 4): gap-loss LPF → de-emphasis → head bump ────────
    gap_z_ += gap_c_ * (wet - gap_z_);
    wet = gap_z_;
    de_emph_z_ += emph_c_ * (wet - de_emph_z_);
    wet = (wet + kEmphAlpha * de_emph_z_) * (1.f / (1.f + kEmphAlpha));
    bump_hi_z_ += bump_hi_c_ * (wet - bump_hi_z_);
    bump_lo_z_ += bump_lo_c_ * (wet - bump_lo_z_);
    wet += kBumpGain * (bump_hi_z_ - bump_lo_z_);

    // ── Head voicing (SW1): one ±12 pitch tap, routed per mode ───────────────
    float shifted = 0.f;
    if (voicing_ != Voicing::Normal && pitch_) {
      float x = wet;                       // PitchShifter::Process takes a ref
      shifted = pitch_->Process(x);
    }
    float out_wet = (voicing_ == Voicing::Sub) ? wet + shifted * kSubBlend : wet;

    // ── Swell loop: darken (+ Shoegaze diffuse), recirculate selected program ─
    trail_lp_z_ += trail_c_ * (wet - trail_lp_z_);
    float fb_src = shoegaze_ ? diffuser_.Process(trail_lp_z_) : trail_lp_z_;
    float fb = fb_src * swell_;
    if (voicing_ == Voicing::Octave) fb += shifted * kOctBlend;   // rising shimmer

    // ── Record path: pre-emphasis → grit drive → saturation (self-limits) ────
    pre_emph_z_ += emph_c_ * ((in + fb) - pre_emph_z_);
    const float pre = (in + fb) + kEmphAlpha * ((in + fb) - pre_emph_z_);
    const float rec = record_.Process(pre * grit_gain_) + Noise();
    drum_->Write(rec);

    return out_wet * Dropout();            // lofi amplitude dropouts on output
  }
};
