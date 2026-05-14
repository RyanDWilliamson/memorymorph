#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// ── NlVerb — two "non-spring" reverb algorithms behind SW3 ───────────────────
//
// Both algorithms share one struct because only one runs at a time (selected
// by SW3 in SDD-555 mode):
//
//   ProcessAms      — SW3 UP  — AMS RMX16 "Non-Linear" gated drum reverb
//   ProcessWildcard — SW3 MID — Wildcard Resonator (A2-harmonic comb bank)
//
//   (SW3 DOWN routes around NlVerb entirely and uses SpringReverb only.)
//
// AMS Non-Linear gate (Phil Collins "In the Air Tonight" character):
//   - Six series allpass filters diffuse the input into a dense burst
//   - An envelope detector + rising-edge trigger arms a gate counter
//   - While the counter is open, the diffused signal passes through; when
//     it closes, output is silenced with a short fade at the end
//   - No long feedback decay — the "tail" is the diffusion itself, abruptly cut
//
// Wildcard Resonator (tuned drone reverb):
//   - Five parallel comb filters with delay = SR / (110 Hz × harmonic_n)
//   - Harmonics 1–5 → 110 / 220 / 330 / 440 / 550 Hz
//   - Feedback gain shared with KNOB_4 — at high values the combs ring with
//     the input's harmonic content, producing a tuned droning reverb that
//     emphasises notes in A minor / C major
//   - Stereo split: odd harmonics (110/330/550) → L, even (220/440) → R
//
// Memory: ~10 KB SRAM (allpass + comb buffers, no SDRAM).

struct NlVerb {
  // ── AMS allpass sizes (prime-ish, ~2–10 ms each at 48 kHz) ────────────────
  static constexpr size_t kAp1 = 89,  kAp2 = 127, kAp3 = 181;
  static constexpr size_t kAp4 = 257, kAp5 = 353, kAp6 = 467;

  // ── Wildcard comb sizes — A2 harmonics @ 48 kHz ───────────────────────────
  // period = round(48000 / (110 × n)) for n = 1..5
  static constexpr size_t kCb1 = 436;  // ~110 Hz (A2 fundamental)
  static constexpr size_t kCb2 = 218;  // ~220 Hz (A3)
  static constexpr size_t kCb3 = 145;  // ~330 Hz (E4)
  static constexpr size_t kCb4 = 109;  // ~440 Hz (A4)
  static constexpr size_t kCb5 = 87;   // ~552 Hz (C#5)

  static constexpr float kAllpassG  = 0.65f;
  static constexpr float kAmsThresh = 0.05f;   // ~-26 dB trigger floor
  static constexpr float kEnvAttack = 0.05f;   // peak follower attack
  static constexpr float kEnvRelease = 0.001f; // peak follower release

  // ── State (NO `= {0}` — see constraint #6 in AGENTS.md) ───────────────────
  float ams_ap1[kAp1], ams_ap2[kAp2], ams_ap3[kAp3];
  float ams_ap4[kAp4], ams_ap5[kAp5], ams_ap6[kAp6];
  size_t ami1 = 0, ami2 = 0, ami3 = 0, ami4 = 0, ami5 = 0, ami6 = 0;
  float ams_env       = 0.f;
  bool  ams_was_above = false;
  uint32_t ams_gate_count = 0;
  uint32_t gate_window_smp = 9600;  // ~200 ms default at 48 kHz

  float cb1[kCb1], cb2[kCb2], cb3[kCb3], cb4[kCb4], cb5[kCb5];
  size_t ci1 = 0, ci2 = 0, ci3 = 0, ci4 = 0, ci5 = 0;

  float decay = 0.7f;

  // ── Init / Reset ──────────────────────────────────────────────────────────
  void Init(float sr) {
    gate_window_smp = static_cast<uint32_t>(0.20f * sr);
    Reset();
  }

  void Reset() {
    for (size_t i = 0; i < kAp1; ++i) ams_ap1[i] = 0.f;
    for (size_t i = 0; i < kAp2; ++i) ams_ap2[i] = 0.f;
    for (size_t i = 0; i < kAp3; ++i) ams_ap3[i] = 0.f;
    for (size_t i = 0; i < kAp4; ++i) ams_ap4[i] = 0.f;
    for (size_t i = 0; i < kAp5; ++i) ams_ap5[i] = 0.f;
    for (size_t i = 0; i < kAp6; ++i) ams_ap6[i] = 0.f;
    for (size_t i = 0; i < kCb1; ++i) cb1[i] = 0.f;
    for (size_t i = 0; i < kCb2; ++i) cb2[i] = 0.f;
    for (size_t i = 0; i < kCb3; ++i) cb3[i] = 0.f;
    for (size_t i = 0; i < kCb4; ++i) cb4[i] = 0.f;
    for (size_t i = 0; i < kCb5; ++i) cb5[i] = 0.f;
    ami1 = ami2 = ami3 = ami4 = ami5 = ami6 = 0;
    ci1  = ci2  = ci3  = ci4  = ci5  = 0;
    ams_env = 0.f;
    ams_was_above = false;
    ams_gate_count = 0;
  }

  void SetDecay(float d) { decay = d; }

  // Wire to KNOB_4 in AMS mode if a user-facing gate length is desired.
  void SetGateMs(float ms, float sr) {
    gate_window_smp = static_cast<uint32_t>(ms * 0.001f * sr);
  }

  // ── AMS gated reverb: dense allpass diffusion + hard envelope gate ────────
  inline void ProcessAms(float in, float& outL, float& outR) {
    // Peak envelope follower (1 ms attack / 50 ms release).
    const float abs_in = fabsf(in);
    if (abs_in > ams_env) ams_env += kEnvAttack  * (abs_in - ams_env);
    else                  ams_env += kEnvRelease * (abs_in - ams_env);

    // Rising-edge trigger: re-arms the gate counter on each transient.
    const bool now_above = ams_env > kAmsThresh;
    if (now_above && !ams_was_above)
      ams_gate_count = gate_window_smp;
    ams_was_above = now_above;

    // Six-allpass diffusion. The L tap is mid-chain (after AP3); R is the
    // final output — gives audible stereo decorrelation without separate paths.
    float d = in;
    d = Allpass(d, ams_ap1, kAp1, ami1, kAllpassG);
    d = Allpass(d, ams_ap2, kAp2, ami2, kAllpassG);
    d = Allpass(d, ams_ap3, kAp3, ami3, kAllpassG);
    const float tap_l = d;
    d = Allpass(d, ams_ap4, kAp4, ami4, kAllpassG);
    d = Allpass(d, ams_ap5, kAp5, ami5, kAllpassG);
    d = Allpass(d, ams_ap6, kAp6, ami6, kAllpassG);
    const float tap_r = d;

    // Gate envelope: full while counter > kRelease, linear fade in the last
    // ~5 ms. Without the fade, gate close clicks audibly.
    constexpr uint32_t kRelease = 240;
    float gate = (ams_gate_count >= kRelease)
                     ? 1.f
                     : static_cast<float>(ams_gate_count) / static_cast<float>(kRelease);
    if (ams_gate_count > 0) ams_gate_count--;

    outL = tap_l * gate;
    outR = tap_r * gate;
  }

  // ── Wildcard resonator: A2-harmonic comb bank ─────────────────────────────
  inline void ProcessWildcard(float in, float& outL, float& outR) {
    const float c1 = Comb(in, cb1, kCb1, ci1, decay);
    const float c2 = Comb(in, cb2, kCb2, ci2, decay);
    const float c3 = Comb(in, cb3, kCb3, ci3, decay);
    const float c4 = Comb(in, cb4, kCb4, ci4, decay);
    const float c5 = Comb(in, cb5, kCb5, ci5, decay);
    // Odd harmonics (110/330/550) anchor the left channel — those are the
    // notes that ring most. Even harmonics (220/440) feed the right.
    outL = (c1 + c3 + c5) * (1.f / 3.f);
    outR = (c2 + c4)      * 0.5f;
  }

private:
  static inline float Allpass(float x, float* buf, size_t size,
                              size_t& idx, float g) {
    const float bufout = buf[idx];
    const float in     = x + g * bufout;
    buf[idx] = in;
    if (++idx >= size) idx = 0;
    return bufout - g * in;
  }

  static inline float Comb(float x, float* buf, size_t size,
                           size_t& idx, float fb) {
    const float bufout = buf[idx];
    buf[idx] = x + bufout * fb;
    if (++idx >= size) idx = 0;
    return bufout;
  }
};
