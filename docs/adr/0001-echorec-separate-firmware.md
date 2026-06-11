# Echorec ships as a separate firmware build, not a third runtime mode

The MemoryMorph firmware already carries two instruments (DMM, SDD-555) swapped
at runtime, and sits at **85% FLASH (111/128 KB)** with ~17 KB free. A faithful
Binson Echorec 2 (4-head drum model + 12-program selector + drum saturation +
tone/age) will not co-reside in that budget without cutting an existing
instrument. We therefore build Echorec as its own binary under `src/Echorec/`,
reusing platform idioms and possibly forking DSP helpers (e.g. the SRE-555
tape-transport flutter/drift), but flashed independently.

## Consequences

- No "one pedal, three instruments" — Echorec is flashed in place of MemoryMorph.
- Echorec is free of the MemoryMorph-wide constraints (48 kHz lock, compander
  conventions); see ADR-0002 and ADR-0003.
- The shared SDRAM `delay_line` / `PitchShifter` allocation in MemoryMorph does
  not apply; Echorec sizes its own buffers.
