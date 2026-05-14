---
applyTo: "src/**/*.cpp"
---

# HotHouse C++ DSP Patterns

## Required includes

```cpp
#include "daisysp.h"
#include "hothouse.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::Parameter;
using daisy::SaiHandle;
using daisy::System;
```

## Global object placement

Declare all DSP objects and state as **file-scope statics** — never on the
stack or heap. Large objects require SDRAM:

```cpp
// Large — MUST be in SDRAM
static DelayLine<float, 96000>  DSY_SDRAM_BSS delay_line;  // 2 s @ 48 kHz
static PitchShifter             DSY_SDRAM_BSS pitch;       // ~128 KB internal buffers

// Small — internal SRAM is fine
static Hothouse   hw;
static Overdrive  drive;
static Oscillator mod_lfo;
static Parameter  p_morph, p_time;
static Led        led_bypass, led_freeze;
static bool       bypass = true;
static bool       frozen = false;
```

## Canonical AudioCallback structure

```cpp
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size) {
  hw.ProcessAllControls();  // ALWAYS first

  // Read all Parameters and toggles once per block
  float morph = p_morph.Process();
  auto  sw1   = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);

  // Update DSP object parameters based on control readings
  reverb.SetFeedback(0.85f);

  // Per-sample loop
  for (size_t i = 0; i < size; ++i) {
    float dry = in[0][i];
    // ... DSP ...
    out[0][i] = out[1][i] = dry;
  }
}
```

## Canonical main() structure

```cpp
int main() {
  hw.Init(true);  // boost = 480 MHz — required for this chain
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
  hw.SetAudioBlockSize(48);
  float sr = hw.AudioSampleRate();  // 48000.f

  // Init all DSP objects with sr
  reverb.Init(sr);
  delay_line.Init();

  // Init Parameters
  p_morph.Init(hw.knobs[Hothouse::KNOB_1], 0.f, 1.f, Parameter::LINEAR);

  // Init LEDs  (pin, invert, update_rate_hz)
  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false, 1000.f);

  // Register footswitch callbacks
  static Hothouse::FootswitchCallbacks cbs = { OnNormalPress, OnDoublePress, OnLongPress };
  hw.RegisterFootswitchCallbacks(&cbs);

  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  uint32_t last_led_ms = 0;
  while (true) {
    uint32_t now = System::GetNow();
    if (now - last_led_ms >= 1) {
      last_led_ms = now;
      led_bypass.Set(bypass ? 0.f : 1.f);
      led_bypass.Update();
    }
    // DFU bootloader entry: detect a 10 s hold of FS1 with all toggles DOWN
    // and Mix at 0, then call System::ResetToBootloader() yourself. The
    // hothouse helper `CheckResetToBootloader()` is NOT used in this project
    // because the same FS1 footswitch already carries tap-tempo / freeze
    // semantics; the combo gates accidental DFU entry during normal playing.
  }
}
```

## Parameter curves

- **Time/frequency knobs** → `Parameter::LOGARITHMIC`
- **Linear mix/level knobs** → `Parameter::LINEAR`
- **Drive/saturation** → `Parameter::EXPONENTIAL` (or LINEAR at low values)

## Lerp helper

```cpp
static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }
```

## Footswitch callback signatures

```cpp
static void OnNormalPress(Hothouse::Switches fsw) { /* toggle state */ }
static void OnDoublePress(Hothouse::Switches fsw) { /* future use   */ }
static void OnLongPress  (Hothouse::Switches fsw) { /* future use   */ }
```

## Things that will break silently

- Forgetting `DSY_SDRAM_BSS` on large buffers → hard fault at `Init()`
- Calling `Led::Update()` inside `AudioCallback` → PWM timing corruption
- Calling `hw.ProcessAllControls()` outside the callback → stale ADC reads
- Using `daisysp::Chorus` at all → buffer hard-coded for 48 kHz but API is fragile; use a `DelayLine + Oscillator` LFO instead. The SDD-555 BBD chorus in `bbd_chorus.h` is the worked example.
