#pragma once
#include <cmath>
#include "constants.h"

// ── DMM signal chain state and DSP ───────────────────────────────────────────
//
// Encapsulates the SA571 compander and the anti-alias / anti-image Butterworth
// filters, plus the BBD bandwidth LPF and feedback path LPF that are part of
// the Deluxe Memory Man signal path.
//
// Call Init(sr) once at startup. Call Reset() when entering bypass so the
// chain starts clean on re-engage. Per-sample methods are inline for
// zero overhead in the audio callback.

struct DmmChain {
  // ── SA571 compressor constants ─────────────────────────────────────────────
  static constexpr float kCompTarget  = 0.25f;    // ~–12 dBFS RMS target
  static constexpr float kCompAttack  = 0.004158f; // ~5 ms  [1-exp(-1/(0.005*48000))]
  static constexpr float kCompRelease = 0.000347f; // ~60 ms [1-exp(-1/(0.060*48000))]
  static constexpr float kCompMaxGain = 2.0f;      // caps gain so hum isn't amplified
  static constexpr float kCompMinGain = 0.1f;
  // Sidechain HPF: 1-pole at ~164 Hz = 1-exp(-2π×164/48000)
  // Rejects 60 Hz mains hum (and its 120 Hz harmonic) from the envelope detector.
  // Guitar tone is unaffected — only what drives the gain decision is high-passed.
  static constexpr float kCompHpfC    = 0.02124f;

  // ── Anti-alias / anti-image biquad state and coefficients ─────────────────
  // 2-pole Butterworth LPF, cutoff 8 kHz. Shared coefficients, separate state.
  // aa = pre-BBD (anti-alias), ai = post-BBD (anti-image reconstruction).
  float aa_w1 = 0.f, aa_w2 = 0.f;
  float ai_w1 = 0.f, ai_w2 = 0.f;
  float aa_b0, aa_b1, aa_b2, aa_a1, aa_a2;  // computed in Init()

  // ── BBD bandwidth LPF ──────────────────────────────────────────────────────
  // One-pole on the delay write path. Cutoff narrows with longer delay time,
  // matching real bucket-brigade (MN3005) bandwidth characteristics.
  // Coefficient bbd_c is computed per-block in AudioCallback (not stored here).
  float bbd_lpf_z = 0.f;

  // ── SA571 compander state ─────────────────────────────────────────────────
  float comp_env_sq = 0.f;  // squared RMS envelope
  float comp_hpf_z  = 0.f;  // sidechain HPF one-pole LP state

  // ── Init / Reset ──────────────────────────────────────────────────────────

  // Compute filter coefficients from the audio sample rate.
  // Must be called once before audio starts.
  void Init(float sr) {
    // 2-pole Butterworth LPF at 8 kHz — bilinear transform, Q = 1/√2.
    const float wc   = kTwoPi * 8000.f / sr;
    const float q    = 0.7071f;
    const float k    = tanf(wc * 0.5f);
    const float k2   = k * k;
    const float norm = 1.f / (k2 + k / q + 1.f);
    aa_b0 =  k2 * norm;
    aa_b1 =  2.f * k2 * norm;
    aa_b2 =  aa_b0;
    aa_a1 =  2.f * (k2 - 1.f) * norm;
    aa_a2 =  (k2 - k / q + 1.f) * norm;
    // One-pole feedback LPF at 5 kHz
    fb_lpf_c = OnePoleCoeff(5000.f, sr);
    Reset();
  }

  // Zero all filter state — call when entering bypass so re-engage starts clean.
  void Reset() {
    aa_w1 = aa_w2 = ai_w1 = ai_w2 = 0.f;
    bbd_lpf_z = fb_lpf_z = 0.f;
    comp_env_sq = comp_hpf_z = 0.f;
  }

  // ── Per-sample filters ────────────────────────────────────────────────────

  // 2-pole Butterworth anti-alias LPF (pre-BBD write path).
  inline float AaFilter(float x) { return BiquadLP(x, aa_w1, aa_w2); }

  // 2-pole Butterworth anti-image LPF (post-BBD read path).
  inline float AiFilter(float x) { return BiquadLP(x, ai_w1, ai_w2); }

  // One-pole LPF on the delay feedback path (warms successive repeats).
  inline float FbFilter(float x) {
    fb_lpf_z += fb_lpf_c * (x - fb_lpf_z);
    return fb_lpf_z;
  }

  // One-pole LPF on the BBD write path (bandwidth narrows with delay time).
  // c is the per-block coefficient: 1 - exp(-2π × cutoff_hz / sr).
  inline float BbdFilter(float x, float c) {
    bbd_lpf_z += c * (x - bbd_lpf_z);
    return bbd_lpf_z;
  }

  // ── SA571 compressor ──────────────────────────────────────────────────────
  // RMS-based 2:1 gain reduction with sidechain HPF.
  // The HPF removes 60 Hz mains hum from the envelope detector so single-coil
  // pickup hum cannot pump the compressor gain. Full-band signal goes to output.
  inline float Compress(float x) {
    comp_hpf_z += kCompHpfC * (x - comp_hpf_z);
    const float x_sc = x - comp_hpf_z;    // sidechain: signal above ~164 Hz
    const float x2   = x_sc * x_sc;
    if (x2 > comp_env_sq)
      comp_env_sq += kCompAttack  * (x2 - comp_env_sq);
    else
      comp_env_sq += kCompRelease * (x2 - comp_env_sq);
    const float rms  = sqrtf(comp_env_sq + 1e-12f);
    const float gain = fmaxf(kCompMinGain, fminf(kCompMaxGain, kCompTarget / rms));
    return tanhf(x * gain);  // tanhf applied to full signal, not sidechain
  }

private:
  // Feedback path LPF state — darkens each successive delay repeat.
  // One-pole at ~5 kHz; coefficient computed in Init().
  float fb_lpf_z = 0.f;
  float fb_lpf_c = 0.f;

  // Direct-form II transposed biquad using this struct's shared coefficients.
  inline float BiquadLP(float x, float& w1, float& w2) {
    const float y = aa_b0 * x + w1;
    w1 = aa_b1 * x - aa_a1 * y + w2;
    w2 = aa_b2 * x - aa_a2 * y;
    return y;
  }
};
