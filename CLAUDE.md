# Memory Morph — Claude Instructions

This is a C++ embedded audio firmware project. Read AGENTS.md for the full
technical context. The summary below is Claude-specific guidance.

## Before making any code change

1. Read `src/hothouse.h` to verify enum names (`KNOB_1`, `FOOTSWITCH_2`, etc.)
   before referencing them — do not guess.
2. Read `src/MemoryMorph/memory_morph.cpp` in full before editing — many
   parameters are interdependent through the MORPH lerp system.
3. Check `DaisySP/Source/` for the exact class/method names of any DaisySP
   module you add. Spellings that look right may be wrong (e.g. `Wavefolder`
   not `WaveFolder`).

## Absolute rules

- Every `DelayLine` or `ReverbSc` declaration **must** have `DSY_SDRAM_BSS`.
- `hw.Init(true)` — the `true` (boost mode) is mandatory.
- Sample rate is `SAI_96KHZ`, block size is `48`. Do not change either.
- `hw.ProcessAllControls()` is always the first line of `AudioCallback`.
- No heap allocation (`new`, `malloc`) anywhere in the file.
- LED updates belong in `while(true)`, not the audio callback.

## MORPH anchor points are sacred

The three-zone character sweep (Tape / Echo / Ambient) defines the product.
Any DSP change that breaks the perceptual identity of any of the three zones
is a regression, not an improvement. See AGENTS.md for the numeric anchor
values.

## DaisySP naming conventions

| Intended module | Correct DaisySP class |
|---|---|
| Wavefolder | `daisysp::Wavefolder` |
| State variable filter | `daisysp::Svf` |
| Overdrive / soft clip | `daisysp::Overdrive` |
| Pitch shifter | `daisysp::PitchShifter` |
| Reverb | `daisysp::ReverbSc` |
| DC blocker | `daisysp::DcBlock` |
| LFO oscillator | `daisysp::Oscillator` |

## Makefile

The build system is `libDaisy/core/Makefile` (ARM Cortex-M7 toolchain).
Never edit `libDaisy/` or `DaisySP/` — they are git submodules.
