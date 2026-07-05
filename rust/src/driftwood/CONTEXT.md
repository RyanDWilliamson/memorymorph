# Driftwood — Context & Glossary

Driftwood is a dual-engine, Mood-style ambient pedal for the HotHouse platform:
a lo-fi **TIME engine** running in series into a lo-fi **SPACE reverb**, animated
by an internal **MOVEMENT** LFO. Design rationale and the full control map live in
[`../../docs/driftwood-plan.md`](../../docs/driftwood-plan.md). This file is the
shared vocabulary — terms only, no implementation detail.

## Engines

- **TIME engine** — the Footswitch-1 side. One engine, three behaviours chosen by
  TOGGLE_2: **Looper**, **Delay**, **Tape-slip**.
- **SPACE engine** — the reverb side. A lo-fi reverb voiced by TOGGLE_3:
  **Dark**, **Modulated**, **Shimmer**.
- **MOVEMENT** — the internal modulation source (there is no expression jack on
  HotHouse). One LFO whose **target** is amplitude (tremolo), time (vibrato), or
  space (swell). Replaces Mood's "movement"/expression feel.

## Control vocabulary

- **Page** — TOGGLE_1 selects which layer the six knobs edit: **TIME**,
  **MASTER**, or **SPACE**. Both engines keep running regardless of page.
- **Soft-takeover** (a.k.a. **pick-up**) — when the page changes a knob's stored
  value stays frozen until the physical knob is swept through it, so values never
  jump.
- **Freeze / Havoc** — FOOTSWITCH_1 held in a delay mode: input is muted and
  feedback is pushed past unity into a sustained, self-oscillating bloom.
- **Bloom** — the SPACE reverb's counterpart to freeze: FS1-hold pushes decay and
  regeneration toward self-oscillation.
- **Looper transport** — mode-dependent FOOTSWITCH_1 in Looper mode: a short
  press cycles **record → play → overdub**; a hold **stops and clears**.
- **Trails** — on bypass the dry passes through clean while existing delay/reverb
  tails ring out rather than cutting abruptly.

## Voicing vocabulary

- **BBD model** — the behavioural model of a bucket-brigade delay chip: syllabic
  **compander** (the source of pumping/breathing), charge-transfer soft clip, and
  clock-dependent **bandwidth** that darkens with longer delay and collapses
  under feedback.
- **Varispeed glide** — sweeping the delay-time knob glides pitch like a tape/BBD
  clock change, rather than retuning cleanly.
- **Tape-slip** — a loop/delay whose time slowly drifts and wanders (varispeed
  wow), sustaining like a tape loop.
- **PT2399 model** — the lo-fi reverb voicing after the PT2399 echo chip: dark
  bandwidth, coarse **quantisation** grit (scaled by **Age**), and dirty,
  soft-clipped long decays.
- **Shimmer** — an octave-up (+12) tail folded into the SPACE reverb feedback.
- **Regen** — the SPACE regen knob lifts the reverb's **internal** comb feedback
  from the decay knob's base toward (never past) unity. There is deliberately no
  outer feedback loop around the reverb network — that topology is unstable
  (loop gain multiplies with the network's resonant gain).
