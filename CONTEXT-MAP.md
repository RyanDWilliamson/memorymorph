# Context Map

This repo hosts multiple guitar-effects **instruments** for the Cleveland Music
Co. HotHouse (Daisy Seed / STM32H750) platform. Each instrument is a
self-contained effect model with its own domain language.

## Contexts

- **DMM** (`src/MemoryMorph/`) — Electro-Harmonix Deluxe Memory Man model.
  Vocabulary currently documented inline in `AGENTS.md`; no `CONTEXT.md` yet.
- **SDD-555** (`src/MemoryMorph/`) — Roland SRE-555 Chorus Echo model.
  Vocabulary currently documented inline in `AGENTS.md`; no `CONTEXT.md` yet.
- [Echorec](./src/Echorec/CONTEXT.md) — Binson Echorec 2 (T7E) magnetic-drum
  multi-head echo.

## Shared term

**Instrument**: a self-contained effect model with its own signal chain,
control map, and identity. _Avoid_: mode (reserved for the runtime-selectable
DMM/SDD-555 dispatch within the MemoryMorph firmware), preset, patch.

## Relationships

- **DMM ↔ SDD-555**: co-resident in the single `MemoryMorph` firmware, swapped
  at runtime via held footswitch+toggle combos. They share the SDRAM
  `delay_line`, `PitchShifter`, tap-tempo state, and DSP idioms; only one runs
  at a time.
- **Echorec → (DMM, SDD-555)**: shipped as a **separate firmware build**, not a
  runtime mode — the MemoryMorph firmware is at 85% FLASH and a faithful
  Echorec will not co-reside. Echorec reuses platform idioms and may fork DSP
  helpers (e.g. the SRE-555 tape-transport flutter/drift code) but is its own
  binary.
