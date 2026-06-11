#pragma once
#include <cmath>
#include <cstdint>
#include "daisysp.h"
#include "constants.h"
#include "tube.h"

using daisysp::DelayLine;

// ── The magnetic drum (one circular medium, 1 record + 4 playback heads) ──────
//
// Topology (DESIGN.md §"Core topology"):
//   read 4 taps @ ¼/½/¾/1 of the current drum length (1:2:3:4)  → wetRaw
//   wet = playback EQ( wetRaw )         : gap-loss LPF → de-emphasis → head bump
//   fb  = darken(wet) · swell           : per-pass bandwidth narrowing (stage 5)
//   write( record-saturate( pre-emphasis(in + fb) ) + wire-noise )
//   Swell recirculates the SELECTED program → patterns multiply & darken.
//
// The large drum buffer lives in SDRAM at file scope in echorec.cpp
// (DSY_SDRAM_BSS can't apply to struct members), so this struct holds only the
// taps / EQ / feedback state and reads/writes through the supplied pointer.
//
// Taps use the lib's ReadHermite (4-point cubic), not Read (linear): the heads
// move with drum speed + warble, and linear interpolation would dull them
// unevenly with fractional delay. All coefficients are derived from `sr` in
// Init() — no 48 kHz-baked literals.

// 12 authentic T7E programs as 4-bit head masks (bit h ⇒ playback head h+1).
// Program 9 filled as 1+4 (the source matrix duplicated #6) — see DESIGN.md.
static constexpr uint8_t kEchoPrograms[12] = {
    0b0001, 0b0010, 0b0100, 0b1000,  //  1: H1   2: H2   3: H3   4: H4
    0b0011, 0b1010, 0b1100, 0b0101,  //  5: 1+2  6: 2+4  7: 3+4  8: 1+3
    0b1001, 0b0111, 0b1110, 0b1111,  //  9: 1+4 10:1+2+3 11:2+3+4 12: all
};

// Per-head playback level (head 4 = primary). TODO: tune on hardware.
static constexpr float kEchoHeadGain[4] = {0.5f, 0.6f, 0.8f, 1.0f};

struct EchorecDrum {
  DelayLine<float, kDrumMaxSmp>* drum_ = nullptr;

  uint8_t program_ = 11;        // active program index (0..11) — boot = all heads
  float   len_smp_ = 28800.f;   // current head-4 (full-rotation) period in samples
  float   swell_   = 0.f;       // 0..0.95

  // ── Pre/de-emphasis pair (stages 2 & 4) — matched α·HPF shelves ─────────────
  // pre : y = x + α·(x − L(x))      (HF gain 1+α, lifts signal over noise/medium)
  // de  : y = (x + α·L(x)) / (1+α)  (HF gain 1/(1+α), drops the accumulated hiss)
  // Net response flat; the medium's losses + noise live between them.
  static constexpr float kEmphAlpha = 1.5f;
  float emph_c_     = 0.f;      // shelf hinge coeff (≈2 kHz), from sr
  float pre_emph_z_ = 0.f;      // record pre-emphasis LPF state (write path)
  float de_emph_z_  = 0.f;      // playback de-emphasis LPF state (read path)

  // ── Gap-loss / bias HF-loss (stages 2/3) ───────────────────────────────────
  // The bias compromise: hotter record level + more Age ⇒ lower playback cutoff
  // (the "too hot = mushy/dark" behaviour). Cutoff is set per block by the glue
  // from drive + Age; the one-pole runs on the read sum.
  float gap_c_   = 0.f;        // playback HF cutoff coeff (set per block)
  float gap_z_   = 0.f;

  // ── Head-bump resonant EQ (stage 4) ────────────────────────────────────────
  // Two-LPF difference ≈ band-pass at ~100 Hz; a scaled copy is added back to
  // lift the playback-head low-mid response.
  float bump_hi_c_ = 0.f, bump_lo_c_ = 0.f;   // ~160 Hz / ~45 Hz, from sr
  float bump_hi_z_ = 0.f, bump_lo_z_ = 0.f;
  static constexpr float kBumpGain = 0.6f;

  // ── Per-pass darkening in the swell loop (stage 5) ──────────────────────────
  float trail_c_    = 0.f;     // cutoff set by SW3 trail character
  float trail_lp_z_ = 0.f;

  // ── Wire noise floor (stage 3) ─────────────────────────────────────────────
  float    noise_amt_  = 0.f;  // level (Age-driven)
  uint32_t noise_seed_ = 0x1B0CA7EDu;

  TubeStage record_;           // magnetic-medium saturation (write path, in-loop)

  void Init(float sr, DelayLine<float, kDrumMaxSmp>* drum) {
    drum_ = drum;
    record_.Init(sr);
    record_.SetSkew(0.06f);                  // even-harmonic magnetic skew
    emph_c_    = OnePoleCoeff(2000.f, sr);   // pre/de-emphasis hinge
    bump_hi_c_ = OnePoleCoeff(160.f,  sr);   // head-bump band edges
    bump_lo_c_ = OnePoleCoeff(45.f,   sr);
    gap_c_     = OnePoleCoeff(14000.f, sr);  // default near-transparent playback LPF
    trail_c_   = OnePoleCoeff(6000.f, sr);   // default Vintage darkening (SW3)
    Reset();
  }
  void Reset() {
    pre_emph_z_ = de_emph_z_ = gap_z_ = 0.f;
    bump_hi_z_  = bump_lo_z_ = trail_lp_z_ = 0.f;
    record_.Reset();
  }

  void SetProgram(uint8_t idx)   { program_ = (idx < 12) ? idx : 11; }
  void SetLengthSmp(float n)     { len_smp_ = n; }
  void SetSwell(float s)         { swell_ = (s > 0.95f) ? 0.95f : (s < 0.f ? 0.f : s); }
  void SetTrailCutoff(float c)   { trail_c_ = c; }              // SW3 Clean/Vintage/Dub
  void SetPlaybackCutoff(float c){ gap_c_ = c; }                // bias/gap loss (drive+Age)
  void SetNoise(float amt)       { noise_amt_ = amt; }          // K5 Age

  // Cheap white-ish noise for the wire floor (stage 3).
  inline float Noise() {
    noise_seed_ = noise_seed_ * 1664525u + 1013904223u;
    return ((float)(int32_t)noise_seed_ * (1.f / 2147483648.f)) * noise_amt_;
  }

  // One sample. `in` already has the SW1 drive gain (×G) applied upstream;
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
    gap_z_ += gap_c_ * (wet - gap_z_);                          // bias/gap HF loss
    wet = gap_z_;
    de_emph_z_ += emph_c_ * (wet - de_emph_z_);
    wet = (wet + kEmphAlpha * de_emph_z_) * (1.f / (1.f + kEmphAlpha));
    bump_hi_z_ += bump_hi_c_ * (wet - bump_hi_z_);
    bump_lo_z_ += bump_lo_c_ * (wet - bump_lo_z_);
    wet += kBumpGain * (bump_hi_z_ - bump_lo_z_);               // ~100 Hz lift

    // ── Swell loop: recirculate the SELECTED program, darkening each pass ─────
    trail_lp_z_ += trail_c_ * (wet - trail_lp_z_);             // stage 5
    const float fb = trail_lp_z_ * swell_;

    // ── Record path: pre-emphasis → magnetic saturation (self-limits swell) ──
    pre_emph_z_ += emph_c_ * ((in + fb) - pre_emph_z_);
    const float pre = (in + fb) + kEmphAlpha * ((in + fb) - pre_emph_z_);
    const float rec = record_.Process(pre) + Noise();           // tanh in-loop, ADR-0003
    drum_->Write(rec);

    return wet;
  }
};
