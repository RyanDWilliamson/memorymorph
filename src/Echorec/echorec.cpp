// Echorec
// Binson Echorec 2 (T7E) — magnetic-drum, fixed-multi-head echo.
// Standalone firmware (ADR-0001) for the Cleveland Music Co. HotHouse.
//
// Hardware: Daisy Seed (STM32H750 ARM Cortex-M7)
// Sample rate: 96 kHz (ADR-0002)  |  Boost: 480 MHz  |  Block size: 48
//
// ── Controls (DESIGN.md) ─────────────────────────────────────────────────────
//   KNOB_1  Head selector — 12 value-gated programs (authentic T7E matrix)
//   KNOB_2  Drum speed    — proportional time, noon ≈ 300 ms (FS1 tap — TODO)
//   KNOB_3  Swell         — regeneration 0 – 0.95
//   KNOB_4  Tone          — bass↔treble tilt
//   KNOB_5  Age           — bias detune + HF loss + wire noise + warble
//   KNOB_6  Mix           — dry/wet
//   SW1  Drive: UP=Hot(8×) MID=Warm(3×) DOWN=Clean(1×)  (G; makeup 1/G, ADR-0003)
//   SW2  Speed range: UP=Long ×2  MID=Vintage ×1  DOWN=Short ×0.5
//   SW3  Trail character: UP=Clean  MID=Vintage  DOWN=Dub
//   FS2  Bypass (LED_2)   |   FS1  Tap tempo (long-hold reserved)
//   DFU bootloader: hold FS1+FS2 ~2 s with all toggles DOWN + Mix at 0
//                   (both LEDs blink alternating 3× to confirm)
//
// Gain staging: in → ×G → drum(saturate in loop) → ×(1/G) → tone → mix → ceiling.
// No compander. Output is mono, summed to both channels.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>

#include "daisysp.h"
#include "hothouse.h"
#include "constants.h"
#include "tube.h"
#include "tone.h"
#include "echorec_drum.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::Parameter;
using daisy::SaiHandle;
using daisy::System;
using daisysp::DcBlock;
using daisysp::DelayLine;

// ── Large DSP buffer — MUST live in 64 MB external SDRAM ──────────────────────
// The drum. Omitting DSY_SDRAM_BSS hard-faults at Init(). Cannot be a struct
// member (the attribute doesn't apply to members), so it is file-scope and
// EchorecDrum reads/writes it through a pointer.
static DelayLine<float, kDrumMaxSmp> DSY_SDRAM_BSS drum;

// ── DSP objects (all SRAM) ────────────────────────────────────────────────────
static Hothouse    hw;
static DcBlock     dc_block;
static EchorecDrum echo;
static TubeStage   out_valve;   // stage 7 — output ceiling
static ToneTilt    tone;        // stage 6 — K4

// ── Parameters ────────────────────────────────────────────────────────────────
static Parameter p_speed;   // KNOB_2 — drum-speed multiplier (log)
static Parameter p_swell;   // KNOB_3 — 0 – 0.95
static Parameter p_tone;    // KNOB_4 — 0 – 1
static Parameter p_age;     // KNOB_5 — 0 – 1
static Parameter p_mix;     // KNOB_6 — 0 – 1

// ── LEDs ────────────────────────────────────────────────────────────────────
static Led led_bypass;   // LED_2 — solid on = active
static Led led_select;   // LED_1 — brief blink on selector program change

// ── State ─────────────────────────────────────────────────────────────────────
static volatile bool bypass = false;       // boot active
static volatile int  program = 11;         // current selector program (0..11)
static volatile bool select_changed = false; // → LED_1 confirm blink (while loop)

// Warble (stage 3) — slow wire pitch instability, folded into the drum length.
static float    warble_phase = 0.f;
static float    warble_drift = 0.f;
static uint32_t warble_seed  = 0x51EDC0DEu;

// FS1 tap tempo → drum speed (sets the head-4 / full-rotation period).
static volatile uint32_t tap_last_ms  = 0;
static volatile float    tap_period_s = 0.f;
static volatile bool     tap_active   = false;
static float             last_speed_knob = -1.f;  // cancels tap when KNOB_2 moves

// Toggle tables indexed by ToggleswitchPosition (UP=0, MIDDLE=1, DOWN=2).
static constexpr float kDriveG[3]   = {8.0f, 3.0f, 1.0f};       // UP=Hot MID=Warm DOWN=Clean (G; makeup 1/G)
static constexpr float kRangeMul[3] = {2.0f, 1.0f, 0.5f};       // UP=Long MID=Vintage DOWN=Short
static constexpr float kTrailHz[3]  = {9000.f, 6000.f, 3500.f}; // UP=Clean MID=Vintage DOWN=Dub

// ── Value-gated head selector with boundary hysteresis ───────────────────────
// 12 zones across the pot; require crossing ~0.25 of a zone past the boundary
// before switching so a pot resting on an edge doesn't flicker.
static int SelectProgram(float knob01, int cur) {
  const float z   = knob01 * 12.f;            // 0 .. 12
  const float lo  = (float)cur + 0.0f;        // zone the current program owns
  const float hi  = (float)cur + 1.0f;
  if (z > hi + 0.25f) { int n = cur + 1; return n > 11 ? 11 : n; }
  if (z < lo - 0.25f) { int n = cur - 1; return n < 0  ? 0  : n; }
  return cur;
}

// ── Footswitches ──────────────────────────────────────────────────────────────
static void OnNormalPress(Hothouse::Switches fs) {
  if (fs == Hothouse::FOOTSWITCH_2) { bypass = !bypass; return; }
  if (fs == Hothouse::FOOTSWITCH_1) {
    const uint32_t now = System::GetNow();
    const uint32_t dt  = now - tap_last_ms;
    tap_last_ms = now;
    if (dt > 100u && dt < 2000u) {          // valid tap interval 100 ms – 2 s
      tap_period_s = (float)dt * 0.001f;
      tap_active   = true;
    }
  }
  // FS1 long-hold reserved.
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size) {
  __set_FPSCR(__get_FPSCR() | (1u << 24));   // flush-to-zero per ISR entry
  hw.ProcessAllControls();                   // always first

  // ── Per-block control snapshot ──────────────────────────────────────────────
  const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  const float g      = kDriveG[sw1];
  const float makeup = 1.f / g;              // analytic — unity by construction
  const float age    = p_age.Process();

  // ── Warble (stage 3): subtle wire pitch instability, gentler than tape ──────
  // 6 Hz flutter + slow red-noise drift, depth growing with Age. Block-rate
  // (0.5 ms blocks → smooth); folded into drum length so all four taps wobble
  // together (playback Doppler).
  warble_phase += kTwoPi * 6.0f * (float)size / kSampleRateF;
  if (warble_phase > kTwoPi) warble_phase -= kTwoPi;
  warble_seed = warble_seed * 1664525u + 1013904223u;
  const float rnd = (float)(int32_t)warble_seed * (1.f / 2147483648.f);
  warble_drift += 0.0015f * (rnd - warble_drift);
  const float warble = 1.f + (0.0005f + age * 0.0035f) * (sinf(warble_phase) + warble_drift);

  // ── Drum length: tap tempo (absolute) or KNOB_2 drum speed × range ──────────
  // KNOB_2 noon (×1.0) = authentic ~300 ms. Turning KNOB_2 cancels an active tap.
  const float speed_mul  = p_speed.Process();           // services the knob each block
  const float speed_knob = hw.GetKnobValue(Hothouse::KNOB_2);
  if (last_speed_knob < 0.f) last_speed_knob = speed_knob;
  if (fabsf(speed_knob - last_speed_knob) > 0.03f) { tap_active = false; last_speed_knob = speed_knob; }

  const float base = tap_active
      ? (tap_period_s * kSampleRateF)                    // tapped head-4 period (absolute)
      : (kDrumNomSec * kSampleRateF * speed_mul * kRangeMul[sw2]);
  float len = base * warble;                             // all four taps wobble together
  if (len > (float)(kDrumMaxSmp - 2)) len = (float)(kDrumMaxSmp - 2);
  if (len < 64.f) len = 64.f;
  echo.SetLengthSmp(len);

  echo.SetSwell(p_swell.Process());
  echo.SetTrailCutoff(OnePoleCoeff(kTrailHz[sw3], kSampleRateF));

  // ── Bias/gap HF-loss (stage 2/3): hotter drive + more Age ⇒ darker playback ─
  float gap_hz = 14000.f - (g - 1.f) * 900.f - age * 8000.f;
  if (gap_hz < 3000.f) gap_hz = 3000.f;
  echo.SetPlaybackCutoff(OnePoleCoeff(gap_hz, kSampleRateF));

  echo.SetNoise(age * 0.0015f);              // wire floor grows with Age

  tone.SetTilt(p_tone.Process());
  const float mix = p_mix.Process();

  // Selector (block-rate, value-gated + hysteresis).
  const int next = SelectProgram(hw.GetKnobValue(Hothouse::KNOB_1), program);
  if (next != program) { program = next; select_changed = true; }
  echo.SetProgram((uint8_t)program);

  // ── Per-sample loop ─────────────────────────────────────────────────────────
  for (size_t i = 0; i < size; ++i) {
    float dry = dc_block.Process(in[0][i]);

    if (bypass) { out[0][i] = out[1][i] = in[0][i]; continue; }

    // ×G → drum (saturates in the swell loop) → ×(1/G): unity small-signal.
    float wet = echo.Process(dry * g) * makeup;
    wet = tone.Process(wet);
    wet = out_valve.Process(wet);            // stage 7 — soft ceiling on swell peaks

    const float y = dry * (1.f - mix) + wet * mix;
    out[0][i] = out[1][i] = y;               // mono, both channels
  }
}

int main() {
  hw.Init(true);                             // boost mandatory
  __set_FPSCR(__get_FPSCR() | (1u << 24));
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);  // ADR-0002
  hw.SetAudioBlockSize(48);
  const float sr = hw.AudioSampleRate();     // 96000.f

  // Drum-speed multiplier 0.5×–2.0× (log), symmetric about unity so KNOB_2 noon
  // = ×1.0 = authentic ~300 ms (150–600 ms at ×1 range; SW2 range scales it).
  p_speed.Init(hw.knobs[Hothouse::KNOB_2], 0.5f, 2.0f,  Parameter::LOGARITHMIC);
  p_swell.Init(hw.knobs[Hothouse::KNOB_3], 0.f,   0.95f, Parameter::LINEAR);
  p_tone.Init (hw.knobs[Hothouse::KNOB_4], 0.f,   1.f,   Parameter::LINEAR);
  p_age.Init  (hw.knobs[Hothouse::KNOB_5], 0.f,   1.f,   Parameter::LINEAR);
  p_mix.Init  (hw.knobs[Hothouse::KNOB_6], 0.f,   1.f,   Parameter::LINEAR);

  dc_block.Init(sr);
  drum.Init();
  echo.Init(sr, &drum);
  tone.Init(sr);
  out_valve.Init(sr);
  out_valve.SetSkew(0.0f);                   // symmetric ceiling (no added harmonics)

  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false, 8000.f);  // 8 kHz PWM
  led_select.Init(hw.seed.GetPin(Hothouse::LED_1), false, 8000.f);

  static Hothouse::FootswitchCallbacks cbs = {OnNormalPress, nullptr, nullptr};
  hw.RegisterFootswitchCallbacks(&cbs);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  // ── Main loop: LED updates only, ~1 kHz ─────────────────────────────────────
  uint32_t last_led_ms     = 0;
  uint32_t select_blink_ms = 0;
  while (true) {
    const uint32_t now = System::GetNow();
    if (now - last_led_ms >= 1) {
      last_led_ms = now;

      led_bypass.Set(bypass ? 0.f : 1.f);
      led_bypass.Update();

      // Selector confirm: blink LED_1 for ~120 ms when the program changes.
      if (select_changed) { select_changed = false; select_blink_ms = now; }
      const bool blink = (now - select_blink_ms) < 120;
      led_select.Set(blink ? 1.f : 0.f);
      led_select.Update();
    }

    // ── DFU bootloader entry ─────────────────────────────────────────────────
    // Hold FS1 + FS2 together for ~2 s with all three toggles DOWN and Mix at 0.
    // The toggle/mix guard stops an accidental double-stomp from dumping into
    // DFU mid-song. (Echorec has no mode-switch combo to disambiguate, so a 2 s
    // hold is safe — MemoryMorph needs 10 s only because of its combos.)
    static uint32_t dfu_hold_start = 0;
    static bool     dfu_was_held   = false;
    const bool both_fs = hw.switches[Hothouse::FOOTSWITCH_1].Pressed() &&
                         hw.switches[Hothouse::FOOTSWITCH_2].Pressed();
    if (both_fs && !dfu_was_held) dfu_hold_start = now;   // rising edge
    const bool toggles_down =
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3) == Hothouse::TOGGLESWITCH_DOWN;
    const bool mix_zero = hw.GetKnobValue(Hothouse::KNOB_6) < 0.02f;
    if (both_fs && toggles_down && mix_zero && (now - dfu_hold_start) >= 2000) {
      hw.StopAudio();
      hw.StopAdc();
      for (int i = 0; i < 3; ++i) {        // both LEDs blink alternating 3×
        led_select.Set(1.f); led_bypass.Set(0.f); led_select.Update(); led_bypass.Update();
        System::Delay(100);
        led_select.Set(0.f); led_bypass.Set(1.f); led_select.Update(); led_bypass.Update();
        System::Delay(100);
      }
      System::ResetToBootloader();
    }
    dfu_was_held = both_fs;
  }
}
