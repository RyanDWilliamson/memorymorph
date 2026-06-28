# 2. Driftwood's companders are modeled circuit behaviour, not a utility AGC

Status: accepted

## Context

The user's hard rule is "anything analog-sounding must be modeled, not faked."
Driftwood chains several level-dependent models (BBD delay, PT2399 reverb) in
series, which raises a gain-staging question. The Echorec firmware previously
*rejected* a compander (ADR 0003 there: fixed hot gain, no compander) to keep its
gain staging simple.

That earlier decision was about a **utility** compander used as a noise-reduction
/ headroom tool. Driftwood faces the opposite situation: the real BBD and PT2399
chips *contain* companders, and their pumping/breathing is part of the sound.

## Decision

Model each chip's compander **inside** its model as part of the voice — the BBD's
syllabic 2:1/1:2 compander (`dsp::bbd`) and the PT2399's quantisation/companding
grit (`dsp::pt2399_reverb`). Do **not** add a global utility AGC. Manage headroom
instead with: a calibrated operating level (±1.0 ≈ nominal), a user **input gain**
(MASTER page), per-stage soft clips, a unity dry path, and an output soft limiter.

## Consequences

- The pumping/breathing artifacts that define BBD and PT2399 tones are preserved.
- This is the opposite of Echorec ADR 0003, and intentionally so — the distinction
  is *modeled-integral* compander vs *utility* compander.
- Inter-stage normalization (TIME output → SPACE input) is still needed so drive
  changes character rather than reverb-send level; this is a bench-voicing item.
