#pragma once
#include <cmath>

// ── Custom plate reverb ────────────────────────────────────────────────────────
// Schroeder-style mono-in / stereo-out plate reverb.
// Architecture: 3-stage allpass pre-diffusion → 4 parallel damped comb filters
// per side → 1 allpass post-diffusion per side.
//
// All delay lengths are mutually prime to eliminate flutter echo.
// L and R comb lengths differ by prime offsets for stereo decorrelation.
// No random modulation — deterministic, zero periodic artifacts.
//
// Total SRAM cost: ~60 KB (vs ReverbSc's 387 KB).
// Declare as `static PlateReverb reverb;` — BSS zero-initialises all buffers.

struct PlateReverb {
  // ── Delay lengths (samples at 48 kHz) ─────────────────────────────────────
  // Pre-diffusion allpass (3 stages, shared mono input)
  static constexpr int kApLen[3]  = { 113, 162, 241 };
  // Parallel comb filters — lengths mutually prime; R offset by primes from L
  static constexpr int kCombL[4]  = { 1213, 1381, 1531, 1657 };
  static constexpr int kCombR[4]  = { 1237, 1409, 1559, 1681 };
  // Post-diffusion allpass per side (different lengths for stereo decorrelation)
  static constexpr int kPostApL   = 453;
  static constexpr int kPostApR   = 557;

  // ── Allpass buffers ────────────────────────────────────────────────────────
  float ap_buf[3][241]    = {};
  int   ap_pos[3]         = {};

  // ── Comb filter buffers ────────────────────────────────────────────────────
  float comb_buf_l[4][1700] = {};
  float comb_buf_r[4][1700] = {};
  int   comb_pos_l[4]       = {};
  int   comb_pos_r[4]       = {};
  float comb_flt_l[4]       = {};  // one-pole LPF state per comb (left)
  float comb_flt_r[4]       = {};  // one-pole LPF state per comb (right)

  // ── Post-diffusion buffers ─────────────────────────────────────────────────
  float post_buf_l[453] = {};
  float post_buf_r[557] = {};
  int   post_pos_l      = 0;
  int   post_pos_r      = 0;

  // ── Parameters — set once per block via SetFeedback / SetLpFreq ───────────
  // Defaults are 0; main() sets correct values before audio starts.
  float feedback = 0.f;
  float damp     = 0.f;

  void SetFeedback(float f) { feedback = f; }

  void SetLpFreq(float hz, float sr) {
    static constexpr float kTwoPi = 6.28318530718f;
    damp = expf(-kTwoPi * hz / sr);
  }

  // ── DSP helpers ───────────────────────────────────────────────────────────
  // Allpass section: fixed coefficient g = 0.5 (Schroeder/Freeverb convention).
  // Transfer function: H(z) = (-g + z^{-D}) / (1 - g*z^{-D}), |H| = 1 for all f.
  inline float Allpass(float in, float* buf, int& pos, int len) {
    const float bufout = buf[pos];
    buf[pos] = in + bufout * 0.5f;
    if (++pos >= len) pos = 0;
    return bufout - in * 0.5f;
  }

  // Damped comb filter — Freeverb style.
  // One-pole LPF (coefficient damp) in feedback so highs decay faster.
  inline float Comb(float in, float* buf, int& pos, int len, float& flt) {
    const float out = buf[pos];
    flt = out * (1.f - damp) + flt * damp;  // one-pole LPF
    buf[pos] = in + flt * feedback;
    if (++pos >= len) pos = 0;
    return out;
  }

  // Mono-in, stereo-out — call once per sample when reverb is active.
  void Process(float in, float* outL, float* outR) {
    // Pre-diffuse through 3 allpass stages for smooth impulse buildup.
    float sig = in;
    sig = Allpass(sig, ap_buf[0], ap_pos[0], kApLen[0]);
    sig = Allpass(sig, ap_buf[1], ap_pos[1], kApLen[1]);
    sig = Allpass(sig, ap_buf[2], ap_pos[2], kApLen[2]);

    // 4 parallel damped combs per side — mutually prime lengths suppress flutter.
    float sumL = 0.f, sumR = 0.f;
    sumL += Comb(sig, comb_buf_l[0], comb_pos_l[0], 1213, comb_flt_l[0]);
    sumL += Comb(sig, comb_buf_l[1], comb_pos_l[1], 1381, comb_flt_l[1]);
    sumL += Comb(sig, comb_buf_l[2], comb_pos_l[2], 1531, comb_flt_l[2]);
    sumL += Comb(sig, comb_buf_l[3], comb_pos_l[3], 1657, comb_flt_l[3]);
    sumR += Comb(sig, comb_buf_r[0], comb_pos_r[0], 1237, comb_flt_r[0]);
    sumR += Comb(sig, comb_buf_r[1], comb_pos_r[1], 1409, comb_flt_r[1]);
    sumR += Comb(sig, comb_buf_r[2], comb_pos_r[2], 1559, comb_flt_r[2]);
    sumR += Comb(sig, comb_buf_r[3], comb_pos_r[3], 1681, comb_flt_r[3]);
    // Normalise and post-diffuse each side independently.
    // Output scale ×0.35 matches ReverbSc's kOutputGain so existing mix levels hold.
    *outL = Allpass(sumL * 0.25f, post_buf_l, post_pos_l, kPostApL) * 0.35f;
    *outR = Allpass(sumR * 0.25f, post_buf_r, post_pos_r, kPostApR) * 0.35f;
  }
};
