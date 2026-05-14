#pragma once
#include <cmath>

// ── NE570 compander model — SRE-555 / SDD-320 "dirt source" ─────────────────
//
// Models the Signetics NE570 log-domain VCA used in Roland's SRE-555 Chorus
// Echo and SDD-320 Dimension D. The NE570 (and its SA571 sibling in the DMM)
// achieves compression by routing the signal through a VCA whose control
// voltage is set by an RMS detector. The VCA itself is a translinear circuit
// with a polynomial transfer function — that polynomial is the source of the
// program-dependent even-order harmonic coloration that gives these boxes
// their "alive" character vs. clean digital processors.
//
// Model: x_vca = x + k2*x² + k3*x³ with k2 dominant (asymmetric, even-order).
// The dirt rides on the signal level — quiet passages stay clean, loud
// passages get the 2nd-harmonic warmth. Compress applies the polynomial;
// Expand stays clean so the dirt from compression survives to the output.
//
// Constants are deliberately looser than the SA571 in DmmChain — more
// "seasoning than compression" per the plan. Pair with PreEmphasis/DeEmphasis
// around the BBD chorus to keep HF noise down.
//
// Per-sample methods are inline for zero overhead in the audio callback.
// Reset() zeros state on bypass entry / mode switch.

struct Ne570 {
  // ── Compander envelope constants (48 kHz) ──────────────────────────────────
  // Attack  ~10 ms:  1 - exp(-1 / (0.010 * 48000)) ≈ 0.002083
  // Release ~120 ms: 1 - exp(-1 / (0.120 * 48000)) ≈ 0.000174
  // Both are roughly 2× slower than the SA571 — the SRE-555 compander is a
  // gentler "smoothing" element rather than the DMM's aggressive squash.
  static constexpr float kTarget  = 0.30f;
  static constexpr float kAttack  = 0.002083f;
  static constexpr float kRelease = 0.000174f;
  static constexpr float kMaxGain = 1.8f;
  static constexpr float kMinGain = 0.15f;

  // Sidechain HPF at ~164 Hz — same as DmmChain. Rejects mains hum from the
  // envelope detector so single-coil pickup hum cannot pump compressor gain.
  static constexpr float kHpfC = 0.02124f;

  // ── VCA polynomial coefficients (the "dirt") ───────────────────────────────
  // k2 dominant = 2nd-harmonic, tube-like asymmetric warmth.
  // k3 small = trace of 3rd harmonic for body without sounding overdriven.
  // Values picked to land at audible-but-musical levels for guitar signals
  // (peak around 0.3–0.7 after compression).
  static constexpr float kK2 = 0.08f;
  static constexpr float kK3 = 0.02f;

  // ── State ──────────────────────────────────────────────────────────────────
  // Separate envelopes for compressor and expander: in the real chain they
  // see different signals (compressor sees clean input; expander sees the
  // BBD-degraded output), so they must track independently.
  float comp_env_sq = 0.f;
  float comp_hpf_z  = 0.f;
  float exp_env_sq  = 0.f;
  float exp_hpf_z   = 0.f;

  void Reset() {
    comp_env_sq = comp_hpf_z = 0.f;
    exp_env_sq  = exp_hpf_z  = 0.f;
  }

  // ── Compressor with VCA polynomial dirt ────────────────────────────────────
  // This is where the SDD-555 "alive" character lives. The polynomial generates
  // even-order harmonics at signal-dependent levels, asymmetric so the 2nd
  // harmonic dominates. Final tanhf prevents runaway at extreme inputs; at
  // typical levels (|y| < 0.7) the polynomial dominates and tanh is near-linear.
  inline float Compress(float x) {
    comp_hpf_z += kHpfC * (x - comp_hpf_z);
    const float x_sc = x - comp_hpf_z;
    const float x2   = x_sc * x_sc;
    if (x2 > comp_env_sq)
      comp_env_sq += kAttack  * (x2 - comp_env_sq);
    else
      comp_env_sq += kRelease * (x2 - comp_env_sq);
    const float rms  = sqrtf(comp_env_sq + 1e-12f);
    const float gain = fmaxf(kMinGain, fminf(kMaxGain, kTarget / rms));
    const float y    = x * gain;
    return tanhf(y + kK2 * y * y + kK3 * y * y * y);
  }

  // ── Matched expander (clean — no polynomial) ───────────────────────────────
  // Same envelope detector, inverse gain. Kept clean so the dirt from
  // Compress survives to the output instead of being partially cancelled.
  // This is what gives real companders their "program-dependent distortion":
  // the residual is exactly the polynomial coloration applied during compression.
  inline float Expand(float x) {
    exp_hpf_z += kHpfC * (x - exp_hpf_z);
    const float x_sc = x - exp_hpf_z;
    const float x2   = x_sc * x_sc;
    if (x2 > exp_env_sq)
      exp_env_sq += kAttack  * (x2 - exp_env_sq);
    else
      exp_env_sq += kRelease * (x2 - exp_env_sq);
    const float rms  = sqrtf(exp_env_sq + 1e-12f);
    const float gain = fmaxf(kMinGain, fminf(kMaxGain, rms / kTarget));
    return x * gain;
  }
};
