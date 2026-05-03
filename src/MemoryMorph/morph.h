#pragma once

// ── MORPH parameter interpolation ────────────────────────────────────────────
//
// Three anchor structs define the sonic character at Morph = 0, 0.5, and 1.
// Every audio block the current Morph position is used to linearly interpolate
// between the two nearest anchors, then the interpolated values drive each DSP
// module. This is the Chase Bliss approach: one macro knob, simultaneous ramp
// of all underlying parameters.

struct MorphParams {
  float delay_send;       // 0–1  how much delay is added to the wet signal
  float reverb_send;      // 0–1  how much of the delay+sat goes into reverb
  float mod_depth_scale;  // 0–1  scales KNOB_4 raw depth value
  float reverb_decay;     // 0.6–0.999  reverb feedback / decay time
  float reverb_lpf_hz;    // Hz  reverb high-frequency damping
};

// Tape anchor: pure saturation, no delay, no reverb
static constexpr MorphParams kAnchorTape = {
    /*delay_send=*/0.00f,
    /*reverb_send=*/0.00f,
    /*mod_depth_scale=*/0.00f,
    /*reverb_decay=*/0.75f,
    /*reverb_lpf_hz=*/9000.f,
};

// Echo anchor: Memory Man-style delay with moderate saturation
// reverb_send=0.30 lets the reverb start fading in before noon on the knob
static constexpr MorphParams kAnchorEcho = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/0.30f,
    /*mod_depth_scale=*/0.50f,
    /*reverb_decay=*/0.78f,
    /*reverb_lpf_hz=*/8500.f,
};

// Ambient anchor: full reverb wash with shimmer and deep modulation
// Higher decay gives shimmer enough tail to sustain; shimmer_amt is kept low
// so loop gain stays well below unity (0.25 × 0.95 = 0.24).
static constexpr MorphParams kAnchorAmbient = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/1.00f,
    /*mod_depth_scale=*/1.00f,
    /*reverb_decay=*/0.95f,
    /*reverb_lpf_hz=*/4000.f,
};

inline float lerpf(float a, float b, float t) { return a + t * (b - a); }

inline MorphParams LerpParams(const MorphParams& a, const MorphParams& b,
                              float t) {
  return {
      lerpf(a.delay_send,      b.delay_send,      t),
      lerpf(a.reverb_send,     b.reverb_send,     t),
      lerpf(a.mod_depth_scale, b.mod_depth_scale, t),
      lerpf(a.reverb_decay,    b.reverb_decay,    t),
      lerpf(a.reverb_lpf_hz,   b.reverb_lpf_hz,   t),
  };
}

inline MorphParams ComputeMorph(float m) {
  if (m <= 0.5f)
    return LerpParams(kAnchorTape, kAnchorEcho, m * 2.f);
  return LerpParams(kAnchorEcho, kAnchorAmbient, (m - 0.5f) * 2.f);
}
