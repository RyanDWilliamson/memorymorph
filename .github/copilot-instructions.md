# Memory Morph — Copilot Instructions

## What this project is

A C++ guitar effects pedal firmware for the **Cleveland Music Co. HotHouse**
hardware platform. The HotHouse carries an **Electrosmith Daisy Seed**
(STM32H750 ARM Cortex-M7) and provides 6 analog knobs, 3 three-position toggle
switches, 2 footswitches, and 2 LEDs in a 125B guitar pedal enclosure.

Two complete instruments share one binary, selectable at runtime via a hidden
FS1+FS2 footswitch combo (3-second hold with all toggles DOWN and Mix dry):

1. **DMM mode** (boot default) — Chase Bliss–inspired morphable chain:
   Deluxe Memory Man preamp/compander → BBD delay → ambient shimmer reverb,
   swept by a single MORPH macro knob.
2. **SDD-555 mode** — Roland SRE-555 Chorus Echo + SDD-320 Dimension D model:
   NE570 VCA compander → BBD chorus → 3-spring tank → AMS Non-Lin / Wildcard verb.
   Built up in phases — see AGENTS.md for the per-phase status.

Current branch: `dmm-deep-dive`.

## Build

```bash
cd src/MemoryMorph          # must be in this directory
make                        # builds MemoryMorph.bin
make program-dfu            # flash via USB DFU (exit code 2 = normal)
```

Requires `arm-none-eabi-gcc` toolchain and `dfu-util`.

## Critical constraints — read before editing any .cpp file

### SDRAM placement
All large DSP buffers **must** use `DSY_SDRAM_BSS` to place them in the 64 MB
external SDRAM. Failure to do this causes hard faults at init.

```cpp
// CORRECT
static DelayLine<float, 96000> DSY_SDRAM_BSS delay_line;
static PitchShifter            DSY_SDRAM_BSS pitch;

// WRONG — will hard fault
static DelayLine<float, 96000> delay_line;

// PlateReverb (~60 KB) fits in SRAM — no DSY_SDRAM_BSS needed
static PlateReverb reverb;  // BSS zero-initialises all buffers
// DSY_SDRAM_BSS cannot be applied to struct members — custom structs live in SRAM.
```

### Boost mode and sample rate
```cpp
hw.Init(true);  // REQUIRED — true = 480 MHz boost for this DSP chain
hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);  // 48 kHz
hw.SetAudioBlockSize(48);
```

### Audio callback rules
- Call `hw.ProcessAllControls()` as the **first line** of `AudioCallback`.
- **No `malloc`, `new`, `printf`, or blocking calls** inside the callback.
- Signature: `void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)`
- Read mono input from `in[0][i]`; write both `out[0][i]` and `out[1][i]`.

### LED updates
LED `Set()` / `Update()` must be called from the **`while(true)` main loop**,
not from the audio callback. Rate-limit to ~1 kHz using `System::GetNow()`.

### No tanhf inside feedback loops
`tanhf` in a feedback path progressively flattens waveforms → `PitchShifter`
grain crossfades cancel → shimmer cuts out. `tanhf` is only permitted at the
**final output mix** and inside `dmm.Compress()` (not a feedback path).

## Project layout

```
src/hothouse.h / hothouse.cpp      — HotHouse hardware proxy (do not modify)
src/MemoryMorph/memory_morph.cpp   — AudioCallback dispatcher + DmmBlock / Sdd555Block per-sample loops + main()
src/MemoryMorph/constants.h        — shared math (kTwoPi, kSampleRateF, OnePoleCoeff)
src/MemoryMorph/morph.h            — MorphParams + ComputeMorph() interpolation (DMM)
src/MemoryMorph/plate_reverb.h     — PlateReverb (Schroeder mono-in/stereo-out, DMM)
src/MemoryMorph/dmm_chain.h        — DmmChain (SA571 compander + BBD filters, DMM)
src/MemoryMorph/tap_tempo.h        — TapTempoState (FS1 tap/freeze state machine, shared)
src/MemoryMorph/shimmer.h          — ShimmerVoice (HPF + PitchShifter + auto-duck, DMM)
src/MemoryMorph/ne570.h            — Ne570 VCA compander with even-order dirt (SDD-555)
src/MemoryMorph/bbd_chorus.h       — BBD / Eventide / Dimension D chorus (SDD-555)
src/MemoryMorph/spring_reverb.h    — 3-spring Accutronics 8AB2D1A tank (SDD-555)
src/MemoryMorph/nl_verb.h          — AMS Non-Lin gated + Wildcard Resonator (SDD-555)
DaisySP/                            — git submodule, do not modify
libDaisy/                           — git submodule, do not modify
```

Each `.h` in `src/MemoryMorph/` is a self-contained DSP struct with
`Init(sr)` / `Reset()` / inline per-sample methods. `memory_morph.cpp` is
pure glue: mode dispatch, parameter wiring, LED logic, two per-sample audio
loops. Full SDD-555 spec (signal chain, constants, control map) lives in `AGENTS.md`.

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

`sat_drive` has been **removed** from `MorphParams` — saturation is fixed by
SW1 (drive toggle), not morphed. MORPH sweeps these parameters only:

| MORPH | Zone | `delay_send` | `reverb_send` | `mod_depth_scale` | `reverb_decay` | `reverb_lpf_hz` |
|---|---|---|---|---|---|---|
| 0.0 | Tape | 0.00 | 0.00 | 0.00 | 0.75 | 9000 |
| 0.5 | Echo | 1.00 | 0.30 | 0.50 | 0.78 | 8500 |
| 1.0 | Ambient | 1.00 | 1.00 | 1.00 | 0.95 | 4000 |

## DMM compander time constants — MUST use correct formula

Wrong time constants (too small) lock the compressor at max gain → hard
clipping → square-wave harmonics → audible 1–3 kHz drone.

```cpp
// CORRECT — compute as 1 - exp(-1 / (τ_seconds × sample_rate))
static constexpr float kCompAttack  = 0.004158f; // 5 ms  at 48 kHz
static constexpr float kCompRelease = 0.000347f; // 60 ms at 48 kHz

// kCompMaxGain capped at 2.0 — higher causes digital whine via sidechain HPF
static constexpr float kCompMaxGain = 2.0f;
// Sidechain HPF at ~164 Hz prevents 60 Hz hum from pumping compressor gain
static constexpr float kCompHpfC    = 0.02124f;
```

## Footswitch behavior

- `FOOTSWITCH_2` single press → toggle bypass (5 ms linear ramp to eliminate pop)
- `FOOTSWITCH_1` short press → tap tempo
- `FOOTSWITCH_1` hold ≥ 1500 ms → momentary freeze (release to exit)

## CPU budget

This chain at 48 kHz / 480 MHz runs comfortably within budget. Do **not**
add additional heavy effects (FFT pitch shifters, multiple reverbs, loopers)
without profiling first. The `PitchShifter` shimmer path is the most expensive
single element.
