#pragma once
#include "constants.h"

// ── Schroeder allpass diffusion cascade (Shoegaze trail) ─────────────────────
//
// Four series allpass filters that smear transients into a wash without adding
// delay you can hear as a discrete repeat — used only on the Echorec feedback
// path when SW3 = Shoegaze, so the recirculating echoes blur into an ambient
// cloud. Same allpass form as the lib's DelayLine::Allpass and the SRE-555
// spring tank: y = read − g·w, w = x + g·read.
//
// Buffers are plain arrays (SRAM, BSS-zeroed at startup — no `= {0}`, per the
// platform rule). Sizes ~1.5–4 ms at 96 kHz, mutually prime to avoid flutter.

static constexpr int kDiffSizes[4] = {149, 211, 281, 367};

struct Diffuser {
  static constexpr int kN = 4;
  float buf0[149];
  float buf1[211];
  float buf2[281];
  float buf3[367];
  float* bufs_[kN];
  int    idx_[kN];
  float  g_ = 0.6f;

  void Init() {
    bufs_[0] = buf0; bufs_[1] = buf1; bufs_[2] = buf2; bufs_[3] = buf3;
    Reset();
  }
  void Reset() {
    for (int i = 0; i < kN; ++i) {
      idx_[i] = 0;
      for (int j = 0; j < kDiffSizes[i]; ++j) bufs_[i][j] = 0.f;
    }
  }
  void SetCoeff(float g) { g_ = g; }

  inline float Process(float x) {
    for (int i = 0; i < kN; ++i) {
      float* b = bufs_[i];
      const int  n = kDiffSizes[i];
      const float read = b[idx_[i]];
      const float w = x + g_ * read;
      b[idx_[i]] = w;
      x = read - g_ * w;
      if (++idx_[i] >= n) idx_[i] = 0;
    }
    return x;
  }
};
