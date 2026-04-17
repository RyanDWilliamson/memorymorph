# Memory Morph — Copilot Instructions

## What this project is

A C++ guitar effects pedal firmware for the **Cleveland Music Co. HotHouse**
hardware platform. The HotHouse carries an **Electrosmith Daisy Seed**
(STM32H750 ARM Cortex-M7) and provides 6 analog knobs, 3 three-position toggle
switches, 2 footswitches, and 2 LEDs in a 125B guitar pedal enclosure.

The effect is a Chase Bliss–inspired morphable chain: tape saturation →
Memory Man-style delay → ambient shimmer reverb, all swept by a single MORPH
macro knob.

## Build

```bash
cd src/MemoryMorph
make                # builds MemoryMorph.bin
make program-dfu    # flash via USB DFU (requires dfu-util)
```

Requires `arm-none-eabi-gcc` toolchain and `dfu-util`.

## Critical constraints — read before editing any .cpp file

### SDRAM placement
All large DSP buffers **must** use `DSY_SDRAM_BSS` to place them in the 64 MB
external SDRAM. Failure to do this causes hard faults at init.

```cpp
// CORRECT
static DelayLine<float, 192000> DSY_SDRAM_BSS delay_line;
static ReverbSc                 DSY_SDRAM_BSS reverb;

// WRONG — will hard fault
static DelayLine<float, 192000> delay_line;
```

`PitchShifter` (two 16384-float delay lines ≈ 128 KB) can live in internal
SRAM at 96 kHz but should be moved to SDRAM if other large objects are added.

### Boost mode and sample rate
```cpp
hw.Init(true);  // REQUIRED — true = 480 MHz boost for this DSP chain
hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);
hw.SetAudioBlockSize(48);
```

### Audio callback rules
- Call `hw.ProcessAllControls()` as the **first line** of `AudioCallback`.
- **No `malloc`, `new`, `printf`, or blocking calls** inside the callback — it
  runs in an interrupt context.
- Signature: `void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)`
- Read mono input from `in[0][i]`; write both `out[0][i]` and `out[1][i]`.

### LED updates
LED `Set()` / `Update()` must be called from the **`while(true)` main loop**,
not from the audio callback. Rate-limit to ~1 kHz using `System::GetNow()`.

### Known 96 kHz caveats
- `daisysp::Chorus::kDelayLength` is hardcoded for 48 kHz — at 96 kHz it only
  covers ~25 ms. **Do not use `daisysp::Chorus`** in this project; use a raw
  `DelayLine` + `Oscillator` LFO instead (already done in `memory_morph.cpp`).
- `PitchShifter::SHIFT_BUFFER_SIZE = 16384` is not SR-scaled; at 96 kHz the
  grain window is ~171 ms instead of ~341 ms. This slightly changes shimmer
  character but is functionally correct.
- `ReverbSc` **will fail to init** in internal SRAM at 96 kHz — always use
  `DSY_SDRAM_BSS`.

## Project layout

```
src/hothouse.h / hothouse.cpp  — HotHouse hardware proxy (do not modify)
src/MemoryMorph/memory_morph.cpp — all DSP + control logic
DaisySP/                        — git submodule, do not modify
libDaisy/                       — git submodule, do not modify
```

## Hardware API quick reference

```cpp
// Knobs — returns 0.0–1.0
hw.GetKnobValue(Hothouse::KNOB_1)  // … KNOB_6

// Toggle positions
hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1)
// returns: TOGGLESWITCH_UP | TOGGLESWITCH_MIDDLE | TOGGLESWITCH_DOWN

// Footswitch debounced state (use FootswitchCallbacks instead of polling)
hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge()
hw.switches[Hothouse::FOOTSWITCH_2].RisingEdge()

// LEDs (pin indices, not GPIO objects)
Hothouse::LED_1 == 22
Hothouse::LED_2 == 23
```

## MORPH three-zone design

MORPH must always sweep through **exactly these three anchor zones**:

| MORPH | Zone | delay_send | reverb_send | sat_drive | mod_depth_scale |
|---|---|---|---|---|---|
| 0.0 | Tape | 0 | 0 | 0.9 | 0 |
| 0.5 | Echo | 1 | 0 | 0.45 | 0.5 |
| 1.0 | Ambient | 1 | 1 | 0.1 | 1.0 |

When changing DSP parameters, preserve these perceptual anchor points.

## Footswitch behavior

- `FOOTSWITCH_2` single press → toggle bypass
- `FOOTSWITCH_1` single press → toggle freeze (infinite reverb + delay hold)
- `FOOTSWITCH_1` 2 s hold → `hw.CheckResetToBootloader()` (DFU flash mode)
  — this is handled automatically; call `CheckResetToBootloader()` in `while(true)`.

## CPU budget

This chain at 96 kHz / 480 MHz runs at approximately 60–80% CPU. Do **not**
add additional heavy effects (FFT pitch shifters, multiple reverbs, loopers)
without profiling first. The `PitchShifter` shimmer path is the most expensive
single element.
