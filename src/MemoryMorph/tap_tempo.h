#pragma once
#include <cmath>
#include <cstdint>

// ── FS1 dual-function state machine ─────────────────────────────────────────
//
// Short press (< 1500 ms): tap tempo — sets delay time by press-to-press interval.
// Hold (≥ 1500 ms):        momentary freeze — release to unfreeze.
//
// 1500 ms threshold lets you tap tempos down to 40 BPM without triggering freeze.
// Interval is measured press-to-press so you tap in time, not to a metronome.
//
// Call Update() once per audio block. Read accessors for DSP and LED logic.

struct TapTempoState {
  // ── Shared with main loop (volatile — ISR writes, main loop reads) ────────
  volatile bool     is_freeze = false;  // true while FS1 held long enough
  volatile bool     active    = false;  // true = use tapped tempo, not knob
  volatile uint32_t blink_ms  = 0;      // timestamp of last tap (LED flash)

  // ── AudioCallback-only state (never read from main loop) ─────────────────
  uint32_t press_start    = 0;
  bool     was_pressed    = false;
  uint32_t last_tap_ms    = 0;
  float    tempo_s        = 0.f;
  float    prev_knob_time = 0.f;  // detects knob movement to cancel tap tempo

  // Call once per block from AudioCallback.
  // pressed = FS1 Pressed(), now_ms = System::GetNow()
  void Update(bool pressed, uint32_t now_ms) {
    if (pressed && !was_pressed)
      press_start = now_ms;

    if (pressed && !is_freeze && (now_ms - press_start) >= 1500)
      is_freeze = true;

    if (!pressed && was_pressed) {
      if (is_freeze) {
        is_freeze = false;
      } else {
        // Short press → tap tempo (press-to-press interval).
        // Minimum 80 ms (750 BPM) allows slapback taps.
        // Only advance last_tap_ms on a VALID tap so rapid re-tapping after a
        // slow tempo converges correctly instead of locking out fast tempos.
        const uint32_t interval = press_start - last_tap_ms;
        blink_ms = now_ms;
        if (interval >= 80 && interval <= 1500) {
          tempo_s     = static_cast<float>(interval) * 0.001f;
          active      = true;
          last_tap_ms = press_start;
        } else if (interval > 1500) {
          last_tap_ms = press_start;  // first tap after a pause — reset ref
        }
        // interval < 80 ms: too fast — ignore, keep old reference
      }
    }

    was_pressed = pressed;
  }

  // Returns effective delay time in seconds.
  // Cancels tap tempo if the knob moves more than 3%.
  float GetTimeS(float knob_time, float max_s = 2.f) {
    if (active && fabsf(knob_time - prev_knob_time) > 0.03f)
      active = false;
    prev_knob_time = knob_time;
    return active ? (tempo_s < max_s ? tempo_s : max_s) : knob_time;
  }

  bool     IsFreeze()   const { return is_freeze; }
  bool     IsActive()   const { return active; }
  uint32_t GetBlinkMs() const { return blink_ms; }
};
