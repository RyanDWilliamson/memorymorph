#pragma once
#include <cmath>
#include "daisysp.h"

// ── Shimmer voice: HPF → PitchShifter(+12 st) → envelope-following duck ─────
//
// The PitchShifter must live in SDRAM (DSY_SDRAM_BSS) and cannot be a struct
// member. ShimmerVoice stores a pointer set at Init time.
//
// Feedback topology:
//   shimmer.buf  →  reverb input  →  reverb output  →  Process()  →  shimmer.buf
//
// buf is public so AudioCallback can add it to the reverb input before calling
// Process() with the reverb output.

struct ShimmerVoice {
  float buf = 0.f;  // last output — fed back into reverb input next sample

  void Init(float sr, daisysp::PitchShifter* p) {
    pitch_ = p;
    pitch_->Init(sr);
    pitch_->SetTransposition(12.f);
    pitch_->SetFun(0.3f);  // grain-position jitter — tape-flutter sparkle
  }

  void Reset() { buf = env_ = hpf_z_ = 0.f; }

  // Process one sample. shimmer_amt > 0.001 activates the voice; otherwise resets.
  // Updates buf with the new pitch-shifted output.
  void Process(float verbL, float shimmer_amt) {
    if (shimmer_amt <= 0.001f) {
      Reset();
      return;
    }

    // Peak follower: fast attack (~0.3 ms), slow release (~150 ms).
    // Gently reduces shimmer input when the loop gets hot — "breathes" rather than blows up.
    const float env_in = fabsf(verbL);
    if (env_in > env_)
      env_ += 0.03f    * (env_in - env_);
    else
      env_ += 0.00007f * (env_in - env_);

    // HPF at ~800 Hz: only harmonics get the octave treatment.
    // Full-band octave shift is muddy; high-pass only sparkles and twinkles.
    hpf_z_ += kHpfC * (verbL - hpf_z_);
    const float hi   = verbL - hpf_z_;
    const float duck  = 1.2f / (1.f + 4.f * env_);
    float       ps_in = hi * duck;

    buf = pitch_->Process(ps_in);
    if (!std::isfinite(buf)) buf = 0.f;
  }

private:
  // 1 - exp(-2π × 800 / 48000) ≈ 0.0995
  static constexpr float kHpfC = 0.0995f;

  float env_   = 0.f;  // peak follower for auto-duck
  float hpf_z_ = 0.f;  // one-pole HPF state

  daisysp::PitchShifter* pitch_ = nullptr;
};
