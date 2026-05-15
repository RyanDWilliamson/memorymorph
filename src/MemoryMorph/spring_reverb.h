#pragma once
#include <cmath>
#include <cstddef>
#include "constants.h"

// ── Spring reverb — Accutronics 8AB2D1A 3-spring tank model ─────────────────
//
// The Roland SRE-555 uses a 3-spring Accutronics tank, not the more common
// 2-spring. Three springs at slightly different physical lengths create
// interference patterns — the characteristic "boing" on transients comes from
// inter-spring beating, and the third spring also extends tail density.
//
// Each spring is modelled as:
//
//   in → main delay (~44/61/72 ms — physical spring travel time)
//      → 3 series allpass filters in the feedback loop (diffuses transients
//        so the spring doesn't sound like a single comb filter)
//      → feedback gain (KNOB_4 — spring decay, common to all three springs)
//
// Cross-coupling: each spring receives a signal tapped from the previous
// spring's midpoint (A←C, B←A, C←B). This is the A→B→C→A cycle that creates
// the inter-spring beating and the "wider than mono" stereo image.
//
// Pre-delay (5 ms) + input HF rolloff (~5 kHz) match the real tank — springs
// physically can't transmit high frequencies, and there's always a few ms
// of travel time before the first reflection.
//
// Dispersive allpass chain on the input (4 cascaded allpass filters with
// growing delay lengths) generates the descending "BOING" chirp on transients
// that defines the spring sound — real spring steel has frequency-dependent
// propagation velocity (high frequencies travel faster), and a cascade of
// allpass filters approximates that group-delay-vs-frequency curve cheaply.
//
// Mono in, stereo out:
//   outL = 0.5 * (a_out + b_out)
//   outR = 0.5 * (b_out + c_out)
//
// Memory: ~40 KB SRAM. No SDRAM needed.

// ── SpringLine — one spring's main delay + 3-allpass diffusor ────────────────
// Pointer-managed buffers so one struct definition serves all three springs
// (saves flash vs. three template instantiations).
struct SpringLine {
  float* main_buf = nullptr;
  size_t main_size = 0;
  size_t main_idx  = 0;

  float* ap1_buf = nullptr; size_t ap1_size = 0; size_t ap1_idx = 0;
  float* ap2_buf = nullptr; size_t ap2_size = 0; size_t ap2_idx = 0;
  float* ap3_buf = nullptr; size_t ap3_size = 0; size_t ap3_idx = 0;

  void Init(float* mb, size_t ms,
            float* a1, size_t a1s,
            float* a2, size_t a2s,
            float* a3, size_t a3s) {
    main_buf = mb;  main_size = ms;  main_idx = 0;
    ap1_buf  = a1;  ap1_size  = a1s; ap1_idx  = 0;
    ap2_buf  = a2;  ap2_size  = a2s; ap2_idx  = 0;
    ap3_buf  = a3;  ap3_size  = a3s; ap3_idx  = 0;
  }

  void Reset() {
    for (size_t i = 0; i < main_size; ++i) main_buf[i] = 0.f;
    for (size_t i = 0; i < ap1_size;  ++i) ap1_buf[i]  = 0.f;
    for (size_t i = 0; i < ap2_size;  ++i) ap2_buf[i]  = 0.f;
    for (size_t i = 0; i < ap3_size;  ++i) ap3_buf[i]  = 0.f;
    main_idx = ap1_idx = ap2_idx = ap3_idx = 0;
  }

  // Midpoint tap (~half the spring length back) — feeds cross-coupling.
  inline float Midpoint() const {
    const size_t mid = main_size / 2;
    const size_t r   = (main_idx + main_size - mid) % main_size;
    return main_buf[r];
  }

  // Per-sample. in already includes cross-coupling. Returns spring output.
  inline float Process(float in, float ap_g, float decay) {
    const float main_out = main_buf[main_idx];
    float fb = main_out;
    fb = Allpass(fb, ap1_buf, ap1_size, ap1_idx, ap_g);
    fb = Allpass(fb, ap2_buf, ap2_size, ap2_idx, ap_g);
    fb = Allpass(fb, ap3_buf, ap3_size, ap3_idx, ap_g);
    main_buf[main_idx] = in + fb * decay;
    if (++main_idx >= main_size) main_idx = 0;
    return main_out;
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
};

struct SpringReverb {
  // ── Spring sizes @ 48 kHz (Accutronics 8AB2D1A character) ─────────────────
  static constexpr size_t kMainA = 2112;   // 44 ms
  static constexpr size_t kMainB = 2928;   // 61 ms
  static constexpr size_t kMainC = 3456;   // 72 ms
  static constexpr size_t kPre   = 240;    //  5 ms input pre-delay

  static constexpr size_t kAp1A = 89,  kAp2A = 113, kAp3A = 157;
  static constexpr size_t kAp1B = 97,  kAp2B = 127, kAp3B = 179;
  static constexpr size_t kAp1C = 101, kAp2C = 139, kAp3C = 197;

  // Dispersive input chain — four cascaded allpasses with increasing delay
  // (89 + 157 + 211 + 257 = 714 samples ≈ 15 ms) and high g create the
  // descending chirp characteristic of real spring steel on transients.
  static constexpr size_t kDispAp1 = 89;
  static constexpr size_t kDispAp2 = 157;
  static constexpr size_t kDispAp3 = 211;
  static constexpr size_t kDispAp4 = 257;
  static constexpr float  kDispG   = 0.7f;   // strong chirp character

  static constexpr float kInputLpHz  = 7000.f;   // raised from 5 kHz — keep more high-end alive so the boing chirp survives
  static constexpr float kAllpassG   = 0.6f;     // diffusion strength
  static constexpr float kXcoupling  = 0.18f;    // reduced from 0.25 for stability at high decay

  // ── Buffers ───────────────────────────────────────────────────────────────
  // NO `= {0}` here — that would force aggregate initialisers into FLASH `.data`.
  // BSS zero-initialisation handles these at startup because the SpringReverb
  // instance has static storage duration. ~40 KB of zeros stayed in FLASH the
  // first time this was written, blowing the 128 KB region.
  float main_a[kMainA], main_b[kMainB], main_c[kMainC];
  float ap1a[kAp1A], ap2a[kAp2A], ap3a[kAp3A];
  float ap1b[kAp1B], ap2b[kAp2B], ap3b[kAp3B];
  float ap1c[kAp1C], ap2c[kAp2C], ap3c[kAp3C];
  float pre_buf[kPre];
  size_t pre_idx = 0;

  // Dispersive input allpasses — generate the spring "BOING" chirp.
  float disp_buf1[kDispAp1], disp_buf2[kDispAp2];
  float disp_buf3[kDispAp3], disp_buf4[kDispAp4];
  size_t disp_idx1 = 0, disp_idx2 = 0, disp_idx3 = 0, disp_idx4 = 0;

  SpringLine sa, sb, sc;

  float input_lpf_z = 0.f;
  float input_lpf_c = 0.f;
  float decay       = 0.5f;

  // ── Init / Reset ──────────────────────────────────────────────────────────
  void Init(float sr) {
    input_lpf_c = OnePoleCoeff(kInputLpHz, sr);
    sa.Init(main_a, kMainA, ap1a, kAp1A, ap2a, kAp2A, ap3a, kAp3A);
    sb.Init(main_b, kMainB, ap1b, kAp1B, ap2b, kAp2B, ap3b, kAp3B);
    sc.Init(main_c, kMainC, ap1c, kAp1C, ap2c, kAp2C, ap3c, kAp3C);
    Reset();
  }

  void Reset() {
    sa.Reset();
    sb.Reset();
    sc.Reset();
    for (size_t i = 0; i < kPre; ++i) pre_buf[i] = 0.f;
    for (size_t i = 0; i < kDispAp1; ++i) disp_buf1[i] = 0.f;
    for (size_t i = 0; i < kDispAp2; ++i) disp_buf2[i] = 0.f;
    for (size_t i = 0; i < kDispAp3; ++i) disp_buf3[i] = 0.f;
    for (size_t i = 0; i < kDispAp4; ++i) disp_buf4[i] = 0.f;
    pre_idx     = 0;
    disp_idx1 = disp_idx2 = disp_idx3 = disp_idx4 = 0;
    input_lpf_z = 0.f;
  }

  // KNOB_4 maps 0–1 → 0–0.95. Above 0.95 the cross-coupling loop can self-oscillate.
  void SetDecay(float d) { decay = d; }

  // ── Per-sample: mono in, stereo out ───────────────────────────────────────
  inline void Process(float in, float& outL, float& outR) {
    // 1. Pre-delay (~5 ms — tank travel time before first reflection).
    const float pre_out = pre_buf[pre_idx];
    pre_buf[pre_idx] = in;
    if (++pre_idx >= kPre) pre_idx = 0;

    // 2. Input HF rolloff — springs physically can't transmit highs.
    input_lpf_z += input_lpf_c * (pre_out - input_lpf_z);
    float drv = input_lpf_z;

    // 3. Dispersive allpass chain — generates the "BOING" chirp on transients.
    //    Real spring steel has high frequencies arriving before low — a
    //    cascade of allpass filters approximates that group-delay curve.
    drv = DispAp(drv, disp_buf1, kDispAp1, disp_idx1, kDispG);
    drv = DispAp(drv, disp_buf2, kDispAp2, disp_idx2, kDispG);
    drv = DispAp(drv, disp_buf3, kDispAp3, disp_idx3, kDispG);
    drv = DispAp(drv, disp_buf4, kDispAp4, disp_idx4, kDispG);

    // 3. Snapshot midpoint taps BEFORE writes — keeps cross-coupling order
    //    independent and prevents one spring's write from leaking into the
    //    same-sample read of another spring.
    const float a_mid = sa.Midpoint();
    const float b_mid = sb.Midpoint();
    const float c_mid = sc.Midpoint();

    // 4. Cross-couple A←C, B←A, C←B (the A→B→C→A cycle).
    const float a_out = sa.Process(drv + c_mid * kXcoupling, kAllpassG, decay);
    const float b_out = sb.Process(drv + a_mid * kXcoupling, kAllpassG, decay);
    const float c_out = sc.Process(drv + b_mid * kXcoupling, kAllpassG, decay);

    // 5. Stereo mix — A+B left, B+C right. Putting B in both anchors the
    //    centre while A and C provide the inter-spring beating in the L/R image.
    outL = 0.5f * (a_out + b_out);
    outR = 0.5f * (b_out + c_out);
  }

private:
  // Schroeder allpass — identical algebra to SpringLine::Allpass but with
  // free-standing index/buf so the dispersive input chain can reuse it.
  static inline float DispAp(float x, float* buf, size_t size,
                             size_t& idx, float g) {
    const float bufout = buf[idx];
    const float in     = x + g * bufout;
    buf[idx] = in;
    if (++idx >= size) idx = 0;
    return bufout - g * in;
  }
};
