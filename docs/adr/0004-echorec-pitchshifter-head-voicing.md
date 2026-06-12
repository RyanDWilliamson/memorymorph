# Echorec adds a PitchShifter for SW1 head voicing

The original Echorec design had **no pitch shifter** (DESIGN.md called it out as a
simplicity win — every effect came from delay/filtering). Hardware play-testing
found the drive switch (SW1) musically uninteresting once the saturation lived in
the always-Hot record stage, so drive is now **fixed at Hot** and SW1 is
repurposed to **head voicing**: Normal / Octave-shimmer (+12) / Sub (−12). That
requires a pitch shifter, reversing the earlier no-shifter stance.

## Considered options

- **Keep SW1 as drive** — rejected: the user found it didn't change the sound
  enough to earn a switch; the auto-makeup made levels track regardless.
- **Octave via delay modulation only** (the SRE-555 chorus trick) — rejected:
  gives detune/vibrato, not a clean ±12 interval for shimmer/sub.

## Consequences

- One `daisysp::PitchShifter DSY_SDRAM_BSS pitch;` at file scope (~128 KB SDRAM,
  ample). FLASH rose 70% → ~73% — comfortably under the 128 KB ceiling, so the
  octave voicing stays (it was the flagged first-cut if budget were tight).
- Octave injects +12 into the **swell feedback** (rising shimmer that builds with
  repeats); Sub blends −12 into the **output** (thickening). Blend levels and the
  feedback path keep swell bounded (≤0.95 + in-loop saturation, ADR-0003).
- The "Echorec has no PitchShifter" note in DESIGN.md is superseded by this ADR.
