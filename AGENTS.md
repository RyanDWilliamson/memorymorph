# Memory Morph — Agent Instructions

## What this is

A C++ guitar effects pedal firmware for the **Cleveland Music Co. HotHouse**
platform (Daisy Seed / STM32H750 ARM Cortex-M7, 480 MHz boost, 96 kHz).
Implements a Chase Bliss–inspired morphable effect chain:
tape saturation → Memory Man delay → ambient shimmer reverb.

## Build & flash

```bash
cd src/MemoryMorph
make                # produces build/MemoryMorph.bin
make program-dfu    # USB DFU flash via dfu-util
```

Prerequisites: `arm-none-eabi-gcc`, `dfu-util`, git submodules initialised.

```bash
git submodule update --init --recursive
```

## Repository layout

```
src/hothouse.h / hothouse.cpp   — HotHouse board support (do NOT modify)
src/MemoryMorph/memory_morph.cpp — all DSP and control logic
DaisySP/                         — git submodule (do NOT modify)
libDaisy/                        — git submodule (do NOT modify)
```

## Non-negotiable constraints

### 1. SDRAM placement for large buffers
```cpp
// CORRECT
static DelayLine<float, 192000> DSY_SDRAM_BSS delay_line;
static ReverbSc                 DSY_SDRAM_BSS reverb;

// WRONG — hard fault at init
static DelayLine<float, 192000> delay_line;
```

### 2. Boost mode + sample rate (mandatory for this chain)
```cpp
hw.Init(true);
hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);
hw.SetAudioBlockSize(48);
```

### 3. Audio callback
- First line must be `hw.ProcessAllControls()`
- No `malloc`, `new`, `printf`, or any blocking call
- Read mono input `in[0][i]`; write `out[0][i]` and `out[1][i]`

### 4. LED updates
Call `Led::Set()` and `Led::Update()` only from the `while(true)` main loop,
rate-limited to 1 kHz via `System::GetNow()`.

### 5. Do not use `daisysp::Chorus` at 96 kHz
Its internal buffer is hardcoded for 48 kHz (25 ms depth at 96 kHz).
Use `DelayLine + Oscillator` LFO instead — already implemented.

## MORPH three-zone design (preserve these anchor values)

| MORPH | Zone | delay_send | reverb_send | sat_drive | mod_depth_scale |
|---|---|---|---|---|---|
| 0.0 | Tape | 0 | 0 | 0.9 | 0 |
| 0.5 | Echo | 1 | 0 | 0.45 | 0.5 |
| 1.0 | Ambient | 1 | 1 | 0.1 | 1.0 |

Any changes to DSP parameters must preserve these perceptual anchor points.

## Control map

| Control | Identifier | Function |
|---|---|---|
| Knob 1 | `KNOB_1` | MORPH macro |
| Knob 2 | `KNOB_2` | Delay time (log, 50 ms–1600 ms) |
| Knob 3 | `KNOB_3` | Feedback (0–0.97) |
| Knob 4 | `KNOB_4` | Mod depth (scaled by MORPH) |
| Knob 5 | `KNOB_5` | Tone LPF (log, 800 Hz–18 kHz) |
| Knob 6 | `KNOB_6` | Dry/wet mix |
| Toggle 1 | `TOGGLESWITCH_1` | Saturation: UP=Tape / MID=Warm / DOWN=Clean |
| Toggle 2 | `TOGGLESWITCH_2` | Mod type: UP=Chorus / MID=Vibrato / DOWN=Wow |
| Toggle 3 | `TOGGLESWITCH_3` | Reverb: UP=Short / MID=Long / DOWN=Shimmer |
| Footswitch 2 | `FOOTSWITCH_2` | Bypass (LED 2) |
| Footswitch 1 | `FOOTSWITCH_1` | Freeze single-press / DFU 2 s hold (LED 1 pulses) |

## CPU budget

At 96 kHz / 480 MHz this chain runs ~60–80% CPU. Do NOT add FFT pitch
shifters, additional reverbs, or loopers without profiling. `PitchShifter` in
the shimmer loop is the most expensive element.

## Known 96 kHz gotchas

- `PitchShifter::SHIFT_BUFFER_SIZE = 16384` is not SR-scaled; grain window is
  ~171 ms at 96 kHz vs ~341 ms at 48 kHz. Character is slightly brighter but
  functionally correct.
- `ReverbSc` will hard-fault if not in SDRAM at 96 kHz.

## Future extensions (flagged TODOs in code)

- `PitchShifter` transposition: currently +12 semitones (octave). A second
  interval (+7, perfect 5th) is a natural extension — KNOB_4 upper range could
  split into interval selection when shimmer toggle is active.
- Expression pedal input on HotHouse maps well to MORPH for real-time
  foot-controlled morphing.
