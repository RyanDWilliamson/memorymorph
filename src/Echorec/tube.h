#pragma once
#include <cmath>
#include "constants.h"

// ── Valve stage (12AX7 / 12AU7) ──────────────────────────────────────────────
//
// Asymmetric soft-clip for the even-harmonic warmth of a triode: y = tanh(x +
// a·x²), DC-blocked (the x² term adds DC). Shared by:
//   • stage 1 — 12AX7 input preamp (the SW1 drive stage; G applied outside)
//   • stage 7 — output valve (the gain-staging ceiling)
//   • the in-loop record-medium saturation inside EchorecDrum
//
// The bias-dependent record HF-loss (stage 2) is a separate concern folded in
// by EchorecDrum / the Age control — kept out of this primitive on purpose.
struct TubeStage {
  float a_    = 0.05f;   // 2nd-harmonic skew
  float dc_z_ = 0.f;     // ~20 Hz DC blocker state
  float dc_c_ = 0.f;     // DC blocker coeff (from sr)

  void Init(float sr) { dc_c_ = OnePoleCoeff(20.f, sr); Reset(); }
  void Reset()        { dc_z_ = 0.f; }
  void SetSkew(float a) { a_ = a; }

  inline float Process(float x) {
    float y = tanhf(x + a_ * x * x);
    dc_z_ += dc_c_ * (y - dc_z_);   // remove the DC the x² term introduced
    return y - dc_z_;
  }
};
