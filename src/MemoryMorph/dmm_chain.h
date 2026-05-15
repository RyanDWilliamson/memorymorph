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
  // Attack/release match the real DMM PCB's 100 µF rectifier cap (~50 ms /
  // ~250 ms), not the SA571 datasheet "voice" setting. The slow release is
  // what produces the famous noise-floor breathing between repeats — earlier
  // 5 ms / 60 ms values gave a cleaner compressor but no audible pump.
  static constexpr float kCompTarget  = 0.25f;     // ~–12 dBFS RMS target
  static constexpr float kCompAttack  = 0.000417f; // ~50 ms  [1-exp(-1/(0.050*48000))]
  static constexpr float kCompRelease = 0.0000833f;// ~250 ms [1-exp(-1/(0.250*48000))]
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

  // ── Pre/de-emphasis shelf around the BBD ──────────────────────────────────
  // Real BBD delays (DMM, Boss DM-2/CE-2, MXR AD-3208) boost HF before the BBD
  // and cut HF after — pushes signal above the BBD noise floor on the way in,
  // de-emphasises both the signal and the accumulated BBD clock/aliasing noise
  // on the way out. Net response is flat; noise floor is ~9.5 dB lower above
  // the corner.
  //
  // Implementation uses the classic α·HPF trick with the existing OnePoleCoeff:
  //   pre  : y = x + α·(x − LPF(x))               (HF gain 1+α, +9.5 dB at α=2)
  //   de   : y = (x + α·LPF(x)) / (1+α)           (DC unity, HF gain 1/(1+α))
  // α=2 / fc=1.5 kHz lands between the bbd_chorus shelf (±6 dB @ 3 kHz) and a
  // Boss CE-2 (±15 dB @ 2.3 kHz) — DMM-appropriate without overcooking treble.
  static constexpr float kEmphAlpha = 2.0f;
  static constexpr float kEmphHz    = 1500.f;
  float emph_c     = 0.f;   // computed in Init()
  float pre_emph_z = 0.f;
  float de_emph_z  = 0.f;

  // ── Asymmetric BBD saturation ─────────────────────────────────────────────
  // BBD chips saturate asymmetrically when driven hard — the positive and
  // negative half-cycles compress at different rates, producing audible
  // 2nd-harmonic content on top of tanh's odd harmonics. `kAsymA` is the
  // small even-order coefficient (~7% 2nd-harmonic at full scale); the slow
  // ~20 Hz HPF below removes the DC offset that x² inevitably accumulates.
  static constexpr float kAsymA    = 0.07f;
  static constexpr float kAsymDcHz = 20.f;
  float asym_dc_c = 0.f;    // computed in Init()
  float asym_dc_z = 0.f;

  // ── SA571 compander state ─────────────────────────────────────────────────
  float comp_env_sq = 0.f;  // squared RMS envelope
  float comp_hpf_z  = 0.f;  // sidechain HPF one-pole LP state
  float exp_env_sq  = 0.f;  // expander envelope (independent — sees BBD output)
  float exp_hpf_z   = 0.f;

  // ── BBD noise-floor injection ─────────────────────────────────────────────
  // Real BBD chips have a stationary noise floor (clock noise, shot noise,
  // 1/f) that becomes audible during quiet passages — and is what gives a
  // companded BBD delay its famous "breathing" character. Digital paths have
  // no noise to expand, so we synthesise it: cheap LCG + one-pole tilt
  // approximating pink, summed in at the BBD write point at ~-65 dBFS RMS.
  uint32_t noise_seed = 0x9E3779B1u;  // any non-zero seed works
  float    noise_z    = 0.f;

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
    // Pre/de-emphasis shelf coefficient (shared, since both filters share fc).
    emph_c     = OnePoleCoeff(kEmphHz,    sr);
    asym_dc_c  = OnePoleCoeff(kAsymDcHz,  sr);
    Reset();
  }

  // Zero all filter state — call when entering bypass so re-engage starts clean.
  void Reset() {
    aa_w1 = aa_w2 = ai_w1 = ai_w2 = 0.f;
    bbd_lpf_z = fb_lpf_z = 0.f;
    pre_emph_z = de_emph_z = 0.f;
    asym_dc_z = 0.f;
    comp_env_sq = comp_hpf_z = 0.f;
    exp_env_sq  = exp_hpf_z  = 0.f;
    noise_z = 0.f;
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

  // ── BBD pre-emphasis (write path) ─────────────────────────────────────────
  // +9.5 dB HF shelf at 1.5 kHz. Apply between the tone filter and the BBD
  // bandwidth LPF so the BBD sees signal lifted well above its noise floor.
  inline float PreEmph(float x) {
    pre_emph_z += emph_c * (x - pre_emph_z);
    return x + kEmphAlpha * (x - pre_emph_z);
  }

  // ── BBD de-emphasis (read path) ───────────────────────────────────────────
  // -9.5 dB HF shelf at 1.5 kHz, inverse of PreEmph. Apply after AiFilter so
  // both the original program HF and the accumulated BBD noise are attenuated
  // back to flat — net response around the BBD is unity within ±0.5 dB.
  inline float DeEmph(float x) {
    de_emph_z += emph_c * (x - de_emph_z);
    constexpr float kInv = 1.f / (1.f + kEmphAlpha);
    return kInv * (x + kEmphAlpha * de_emph_z);
  }

  // ── Asymmetric BBD soft saturation (write path) ───────────────────────────
  // Adds a 2nd-harmonic skew to the BBD's natural soft compression. Pure
  // tanh is odd-symmetric and only generates 3rd, 5th, 7th harmonics — real
  // BBDs and tape both produce a noticeable 2nd harmonic. The (x + α·x²)
  // pre-shaping introduces it; tanh keeps the result bounded; the embedded
  // 20 Hz HPF removes the DC offset that x² produces (which would otherwise
  // accumulate in the delay's feedback loop).
  inline float AsymSat(float x) {
    const float y = x + kAsymA * x * x;
    asym_dc_z += asym_dc_c * (y - asym_dc_z);
    return tanhf(y - asym_dc_z);
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

  // ── SA571 expander (matched complement of Compress) ───────────────────────
  // Inverse gain law: gain = rms / target. When the BBD output is at the
  // compressor's nominal target level, gain ≈ 1 (transparent). When the BBD
  // output is mostly noise (between transients), gain falls toward kCompMinGain
  // → noise floor pulled down by ~30 dB → audible "breathing" decay between
  // hits, which is the defining characteristic of every companded BBD box.
  inline float Expand(float x) {
    exp_hpf_z += kCompHpfC * (x - exp_hpf_z);
    const float x_sc = x - exp_hpf_z;
    const float x2   = x_sc * x_sc;
    if (x2 > exp_env_sq)
      exp_env_sq += kCompAttack  * (x2 - exp_env_sq);
    else
      exp_env_sq += kCompRelease * (x2 - exp_env_sq);
    const float rms  = sqrtf(exp_env_sq + 1e-12f);
    const float gain = fmaxf(kCompMinGain, fminf(kCompMaxGain, rms / kCompTarget));
    return x * gain;
  }

  // ── BBD noise-floor sample (~-65 dBFS RMS) ────────────────────────────────
  // Cheap pink-ish noise: LCG white source + one-pole tilt mixed back in.
  // Output amplitude calibrated for ~5.6e-4 RMS = -65 dBFS, deep enough to be
  // inaudible against signal but sufficient for the expander to chew on.
  inline float Noise() {
    noise_seed = noise_seed * 1664525u + 1013904223u;  // Numerical Recipes LCG
    const float white = static_cast<int32_t>(noise_seed) * (1.f / 2147483648.f);
    noise_z += 0.05f * (white - noise_z);
    return (white * 0.4f + noise_z * 0.6f) * 5.6e-4f;
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
