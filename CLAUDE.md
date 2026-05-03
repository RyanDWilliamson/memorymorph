# Memory Morph — Claude Instructions

This is a C++ embedded audio firmware project. Read AGENTS.md for the full
technical context. The summary below is Claude-specific guidance.

## Before making any code change

1. Read `src/hothouse.h` to verify enum names (`KNOB_1`, `FOOTSWITCH_2`, etc.)
   before referencing them — do not guess.
2. Read `src/MemoryMorph/memory_morph.cpp` in full before editing — many
   parameters are interdependent through the MORPH lerp system.
3. Also read the relevant header(s): `morph.h`, `plate_reverb.h`, `dmm_chain.h`.
   DSP logic has been extracted into these — edit them, not just the .cpp.
4. Check `DaisySP/Source/` for the exact class/method names of any DaisySP
   module you add. Spellings that look right may be wrong (e.g. `Wavefolder`
   not `WaveFolder`).

## Absolute rules

- `DelayLine` and `PitchShifter` **must** have `DSY_SDRAM_BSS`. `PlateReverb` and
  `DmmChain` are custom structs in SRAM — do **not** add `DSY_SDRAM_BSS` to them.
- `hw.Init(true)` — the `true` (boost mode) is mandatory.
- Sample rate is `SAI_48KHZ`, block size is `48`. Do not change either.
- `hw.ProcessAllControls()` is always the first line of `AudioCallback`.
- No heap allocation (`new`, `malloc`) anywhere in the file.
- LED updates belong in `while(true)`, not the audio callback.
- `tanhf` is **prohibited in the shimmer/reverb recirculation loop** — flattens waveforms,
  PitchShifter grain crossfades cancel, shimmer cuts out. Permitted: final output mix,
  `dmm.Compress()`, and the delay feedback write path. See AGENTS.md §6 for full rationale.

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
| Reverb | custom `PlateReverb` struct in `plate_reverb.h` (NOT `daisysp::ReverbSc`) |
| DC blocker | `daisysp::DcBlock` |
| LFO oscillator | `daisysp::Oscillator` |

## Makefile

The build system is `libDaisy/core/Makefile` (ARM Cortex-M7 toolchain).
Never edit `libDaisy/` or `DaisySP/` — they are git submodules.
