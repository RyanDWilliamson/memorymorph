#pragma once
#include <cmath>

// ── Memory Morph shared constants & math helpers ─────────────────────────────
//
// `static constexpr` at namespace scope → each translation unit gets its own
// internal-linkage copy. Safe to include from multiple headers without ODR
// violations. `static inline` for the helper does the same for the function.
//
// Add new shared math here rather than redeclaring in each DSP header.

static constexpr float kTwoPi       = 6.28318530718f;
static constexpr float kSampleRateF = 48000.f;

// Coefficient for a 1-pole filter tracking state with `y += c * (x - y)`:
//   c = 1 - exp(-2π · hz / sr)
// Use for the LPF half of a shelf, the LPF state of an HPF (x - LPF(x)),
// or any 1-pole tracker. Caller supplies sr to keep the helper portable
// across sample rates (the firmware is fixed at 48 kHz but DSP headers
// shouldn't bake that assumption in).
static inline float OnePoleCoeff(float hz, float sr) {
  return 1.f - expf(-kTwoPi * hz / sr);
}
