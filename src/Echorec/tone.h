#pragma once
#include "constants.h"

// ── Bass/Treble tone network (stage 6, K4) ───────────────────────────────────
//
// The real T7E has separate Bass and Treble valve tone controls; here they are
// collapsed into a single tilt knob (see DESIGN.md control map). One-pole hinge
// at ~700 Hz: split into low/high bands and crossfade their balance.
//   knob 0.5 → flat   |   <0.5 → darker (lift lows)   |   >0.5 → brighter.
struct ToneTilt {
  float lp_z_ = 0.f;
  float c_    = 0.f;     // hinge coeff
  float tilt_ = 0.f;     // -1 .. +1

  void Init(float sr) { c_ = OnePoleCoeff(700.f, sr); Reset(); }
  void Reset()        { lp_z_ = 0.f; }
  void SetTilt(float knob01) { tilt_ = 2.f * knob01 - 1.f; }

  inline float Process(float x) {
    lp_z_ += c_ * (x - lp_z_);
    const float low  = lp_z_;
    const float high = x - lp_z_;
    // TODO: voice the band gains (a flat Baxandall mid, gentle shelves) once on
    // hardware — current form is a linear tilt placeholder.
    return low * (1.f - tilt_) + high * (1.f + tilt_);
  }
};
