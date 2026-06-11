# Echorec — Design Spec

Implementation plan for the **Binson Echorec 2 (T7E)** instrument: a magnetic-
drum, fixed-multi-head echo. Built as a **standalone firmware** (ADR-0001),
running the whole engine at **96 kHz** (ADR-0002), with **no compander** and
analytic gain staging (ADR-0003). Domain terms: [CONTEXT.md](./CONTEXT.md).

## What it is

A circuit-level model of the 7-tube T7E: a steel recording wire wound on a
fixed-speed drum, one record head, four playback heads at fixed positions
(75 / 150 / 225 / 300 ms — a clean **1:2:3:4 ratio**), and a 12-position selector
that picks *combinations* of heads to make rhythmic multi-echo programs. Swell
recirculates the selected program so patterns multiply and darken.

## Core topology (one circular drum)

```
            ┌──────── circular drum buffer (length = head-4 / full-rotation period) ────────┐
record  →   write( drumSaturate( in·G + swell·feedbackSum ) )                                │
heads   →   read @ ¼ (75ms) · read @ 2/4 (150ms) · read @ ¾ (225ms) · read @ 4/4 (300ms)     │
selector →  sum active heads per the 12-program matrix  →  wetSum                             │
swell   →   feedbackSum = swellColor(wetSum)   (selected program recirculates → multiplies)   │
out     →   wetSum → playbackEQ → tone → ×(1/G) → mix(dry,wet) → outputCeiling → out L=R       │
```

- **One file-scope SDRAM circular buffer** is the drum (`DSY_SDRAM_BSS` can't go
  on struct members, so the buffer is a global; the `EchorecDrum` struct holds
  taps/state in SRAM and reads/writes it — same pattern as MemoryMorph's
  `delay_line`). Size for the worst case (Long range × slow speed); ~1 s @ 96 kHz
  ≈ 96k–131k samples is ample and SDRAM is plentiful.
- **Four read taps** at ¼/½/¾/1 of the *current* drum length. Drum speed and the
  speed-range switch scale the length the taps index into; the 1:2:3:4 ratio is
  preserved so every program stays rhythmically coherent at any speed. Use
  fractional (interpolated) reads — the taps move with drum speed and warble.
- **Saturator in the write path** = the magnetic medium; in-loop `tanh`/asym
  self-limits Swell into a controlled oscillating drone (authentic, and the
  MemoryMorph "no tanh in feedback" ban does **not** apply — that was about
  PitchShifter grain cancellation, and Echorec has no PitchShifter).
- **Swell ≤ 0.95**; self-oscillation is a bounded feature, not a fault.

## The 12-program head-selector matrix (K1, value-gated)

A plain pot quantized into 12 zones in firmware (block-rate, with boundary
hysteresis + LED confirm — no stepped-pot hardware). Authentic T7E order, with
program 9 filled as 1+4 (the source's #9 duplicated #6):

| # | Heads | | # | Heads |
|---|---|---|---|---|
| 1 | 1 | | 7 | 3+4 |
| 2 | 2 | | 8 | 1+3 |
| 3 | 3 | | 9 | **1+4** (filled) |
| 4 | 4 | | 10 | 1+2+3 |
| 5 | 1+2 | | 11 | 2+3+4 |
| 6 | 2+4 | | 12 | 1+2+3+4 |

## Modeled analog stages (the deep-dive)

All 7 stages modeled, in `dmm_chain.h` house style (standalone DSP structs):

1. **12AX7 input preamp** — asymmetric soft-clip `x + a·x²` (even-harmonic
   warmth). This is the SW1 drive stage: pre-gain `G`, makeup `1/G` (ADR-0003).
2. **Record amp + AC bias** — pre-emphasis + magnetic soft-saturation +
   **level-dependent HF loss** (real bias compromise: hot→mushy/dark,
   cold→thin). Bias point is folded into SW1 drive (record level) and K5 Age.
3. **Magnetic wire/drum medium** — hysteresis saturation + gap-loss HF rolloff +
   wire **noise floor** + subtle **warble** (gentler than tape).
4. **4× playback head amps** — resonant **head-bump** EQ + de-emphasis.
5. **12AU7 regeneration valve** — per-pass tube color + **progressive bandwidth
   narrowing** in the Swell loop → repeats darken & soften as they multiply
   (the defining Binson trail). Voiced by SW3 trail character.
6. **Bass/Treble tone network** — Baxandall-style **tilt** → K4.
7. **Output valve** — gentle final soft-clip = the output ceiling.

(EM81 magic-eye is a level meter, not audio — could inspire an LED behavior, no
DSP.)

## Control map

| Control | Function | Detail |
|---|---|---|
| **K1** | Head selector | 12 value-gated programs (table above) + hysteresis + LED confirm |
| **K2** | Drum speed | proportional time, noon = authentic ~300 ms; FS1 tap-syncable |
| **K3** | Swell | regeneration amount, ≤ 0.95 |
| **K4** | Tone | bass↔treble tilt |
| **K5** | Age | bias detune + HF loss + wire noise + warble depth |
| **K6** | Mix | dry/wet (folds in the "Echo" output level) |
| **SW1** | Drive | Clean 1× / Warm 3× / Hot 8× — sets `G`; makeup `1/G` |
| **SW2** | Speed range | Short ×0.5 / Vintage ×1 / Long ×2 (on the drum length) |
| **SW3** | Trail character | Clean / Vintage / Dub (per-pass darkening + in-loop sat) |
| **FS1** | Tap tempo (drum speed) | long-hold **reserved** (no freeze) |
| **FS2** | Bypass | LED 2 |

Output is **mono**, summed to both `out[0]`/`out[1]`.

**DFU bootloader:** hold **FS1 + FS2** for ~2 s with all toggles DOWN and Mix at
0 → both LEDs blink alternating 3× → `System::ResetToBootloader()`. The
toggle/mix guard prevents an accidental double-stomp from entering DFU. 2 s is
safe here because Echorec, unlike MemoryMorph, has no mode-switch combo to
disambiguate from.

## Gain staging (ADR-0003)

`in → ×G → drumSaturate() → ×(1/G) → … → mix → outputCeiling`. SW1 sets only
`G`; makeup is computed `1/G` (no table). Unity is structural — drive changes
character, not level. No compander, no `kOutTrim`, no send-scaling. The output
soft-clip catches only swell-oscillation peaks.

## Platform constraints (carried over)

- `hw.Init(true)` boost mode; `hw.SetAudioSampleRate(... SAI_96KHZ)`; block 48.
- `hw.ProcessAllControls()` first line of `AudioCallback`. No `malloc`/`new`/
  `printf`/blocking. LED updates only in `while(true)`, rate-limited.
- No `= {0}` on large struct-member arrays (forces FLASH `.data`); rely on BSS.
- Read `src/hothouse.h` for exact `KNOB_*` / `TOGGLESWITCH_*` / `FOOTSWITCH_*`
  enum names before referencing them.

## Proposed file layout (`src/Echorec/`)

```
echorec.cpp        — main() + AudioCallback glue; file-scope SDRAM drum buffer
echorec_drum.h     — EchorecDrum: 4-tap drum, selector matrix, swell loop
tube.h             — TubeStage (12AX7/12AU7 asym soft-clip) + record/bias model
tone.h             — Baxandall tilt (K4)
constants.h        — kSampleRateF = 96000, OnePoleCoeff(), etc.
Makefile           — cloned from MemoryMorph; TARGET=Echorec, CPP_SOURCES=echorec.cpp
```

Reusable references from the existing repo (fork, don't share — separate binary):
the SRE-555 tape-transport **flutter/drift** integrators and **head-bump** EQ
(AGENTS.md §390-422), the DMM **pre/de-emphasis** + **asym saturation** idioms
(`dmm_chain.h`), and the **value-gated control + hysteresis** pattern used for
SW1 drive levels.

## Build & verify

```bash
cd src/Echorec && make            # build/Echorec.bin
make program-dfu                  # DFU flash (exit code 2 = normal)
```

On-hardware checks: (1) each of the 12 programs produces the expected tap
pattern; (2) Hot drive stays at unity vs Clean (analytic 1/G); (3) Swell near
max self-oscillates into a bounded saturated drone, never digital blowout;
(4) repeats audibly darken as they multiply; (5) drum-speed sweep keeps program
ratios intact. Optional pre-flash: a small host harness compiling the `.h`
structs with a desktop `main`, feeding an impulse to confirm tap sample-offsets
and unity small-signal gain across drive positions before flashing.
