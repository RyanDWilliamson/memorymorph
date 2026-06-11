#pragma once
#include <cmath>
#include <cstddef>

// ── Echorec shared constants & math helpers ──────────────────────────────────
//
// Standalone firmware (ADR-0001). Whole engine runs at 96 kHz (ADR-0002) — the
// one value that differs from the 48 kHz MemoryMorph build. `static constexpr`
// at namespace scope gives each translation unit its own internal-linkage copy
// (safe to include from multiple headers; same idiom as MemoryMorph).

static constexpr float kTwoPi       = 6.28318530718f;
static constexpr float kSampleRateF = 96000.f;          // ADR-0002

// c = 1 - exp(-2π·hz/sr) for a 1-pole tracker  y += c·(x - y).
static inline float OnePoleCoeff(float hz, float sr) {
  return 1.f - expf(-kTwoPi * hz / sr);
}

// ── Drum geometry ────────────────────────────────────────────────────────────
// Head 4 reads the full drum rotation; heads 1–3 at ¼/½/¾ of it (1:2:3:4).
// Authentic full-rotation period at noon / ×1 speed range:
static constexpr float  kDrumNomSec = 0.300f;           // ~300 ms

// Worst-case drum buffer (SDRAM): 300 ms × 2.0 (max drum speed) × 2.0 (Long
// range) = 1.2 s. 131072 samples ≈ 1.365 s @ 96 kHz leaves guard headroom.
static constexpr size_t kDrumMaxSmp = 131072;
