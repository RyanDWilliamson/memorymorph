# 1. Driftwood uses knob paging with soft-takeover

Status: accepted

## Context

Driftwood is a dual-engine pedal (TIME + SPACE) plus an internal MOVEMENT
source — far more parameters than the HotHouse's six physical knobs. The user
required that alternate knob functions map to the *similar* primary knob
("delay time ↔ other time", "gain ↔ wet/dry") and that everything stay easy and
musical, with the footswitches fixed to tap and bypass (so they cannot host a
shift/page gesture).

## Decision

TOGGLE_1 selects a knob **page** — TIME / MASTER / SPACE — and the six knobs edit
that page. Each knob keeps its semantic family across pages. Because a physical
knob no longer matches the parameter it drives after a page change, every
page/knob slot uses **soft-takeover**: the stored value stays frozen until the
knob is swept through it, then tracks. Implemented as the pure, host-tested
`dsp::takeover::SoftTakeover`, banked per page/knob in `driftwood::PagedKnobs`.

## Consequences

- No value jumps on page changes; both engines keep running while you edit either.
- A toggle is spent on paging, leaving exactly two for the engine-mode selectors.
- Users must learn the page concept, and editing a hidden page means flipping a
  toggle — accepted as the only way to fit two engines + movement on six knobs
  without adding controls.
