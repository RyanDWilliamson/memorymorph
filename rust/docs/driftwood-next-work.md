# Driftwood — Work Queue for Future Agents

Prerequisite reading, in order: `driftwood-agent-guide.md` (iron rules —
non-negotiable), then this file. Each task is self-contained, sized for a
single session, and lists acceptance criteria. Do them in order unless the
user says otherwise; **never combine tasks in one commit.** Every task ends
with: dsp tests green, firmware + dsp clippy clean (`-D warnings`), pushed to
`driftwood`, and flashed if the pedal is in DFU.

Current state when this queue was written: pedal plays; TIME voiced over four
bench passes; the runaway (age loop gain) and the birdie (quantizer limit
cycle) fixed with regression tests; SPACE knob-abuse pass awaiting bench
confirmation. Freeze and looper are explicitly **deprioritized by the user** —
polish them only when asked.

---

## Task 1 — Strip the diagnostic scaffolding, restore designed LED roles
*Size: small. Model: Sonnet is fine. Files: `src/bin/driftwood.rs` only.*

The silent-pedal hunt left LED1 overloaded (input-activity meter, ISR-alive
tick, DMA-error flicker, freeze, looper state, overrun hold). Restore the
design:

- LED1 = tap-tempo pulse (blink at the current delay tempo in delay modes),
  freeze solid while frozen, looper solid-record / 2 Hz-blink-play (keep).
- **Remove**: INPUT_HOLD / INPUT_R_HOLD meters and their ISR peak tracking,
  the ISR-alive tick, the DMA-error flicker (keep the DMA_ERRORS counter
  itself).
- **Keep permanently** (do NOT remove): the panic handler (both-LED strobe),
  HardFault handler (alternating strobe), the DMA self-heal flag clearing,
  the non-panicking ISR error path, `ISR_BLOCKS`/`DMA_ERRORS` counters, the
  overrun raw-set + OVERRUN_HOLD in the LED expression (it is the only signal
  that survives a starved main loop).
- Implement the tap-pulse: blink LED1 with period = current delay time while
  in delay modes and not frozen (state already available in the control loop
  via `tap_delay_s` / the TIME page knob; a coarse ms period is fine).

Acceptance: builds + clippy clean; LED comments at the top of the file match
the new behavior exactly; bench-check by the user (tap twice → LED pulses at
tempo; freeze → solid; no flicker in normal play).

## Task 2 — SPACE voicing pass (bench loop with the user)
*Size: interactive session. Model: Opus recommended (live DSP judgement).
Files: `src/driftwood/space_engine.rs`, `dsp/src/pt2399_reverb.rs` constants
only.*

Run the TIME playbook (guide §"voicing session") on SPACE. Knobs already
zipper-free and stability-guarded; this is ears only. Likely dials, from the
TIME experience:

- Character scales per Toggle-3 mode (`tone_scale`, `mod_scale` tuples).
- Wash loudness vs `output_norm_for` (if tails feel quiet at high regen,
  raise the `4.0` constant a little — re-run `resonant_peak_gain...` test and
  keep it passing; do NOT bypass the normalization).
- Age curve (`quant_levels` map, drive amount) — grit balance.
- Mod depth/rate ranges (`MOD_DEPTH_MAX`, `set_rate` law).
- Shimmer audibility within its stability budget (raise the knob-factor side,
  never the `(1−fb)` side; `shimmer_injection_stays_bounded...` must pass).

Hard line: any change to feedback laws, normalization, quantizer, or clip
topology requires a failing-then-passing host test, not taste.

## Task 3 — MOVEMENT bench verification
*Size: small-medium. Model: Sonnet OK. Files: `movement_engine.rs`,
`dsp/src/movement.rs` constants.*

MOVEMENT has never been heard on hardware. Bench-verify with the user:
- MASTER page: K1 rate (0.1–12 Hz exp), K2 depth (defaults 0 — user must
  raise it), K3 shape (sine/harmonic/square/ramp), K5 target (CCW=tremolo,
  center=vibrato, CW=swell).
- Check: square shape needs click-free edges (if it clicks, slew the square in
  `Lfo::shape_value` or smooth in `apply_amp` — add the smoothing at the
  consumer, mirroring `depth_s`).
- Check tap-sync feel (`tap_sync` resets phase on accepted taps).
- Harmonic-trem crossover (800 Hz) and depth laws to taste.

## Task 4 — Docs & phase-history refresh (lockstep debt)
*Size: small. Model: Sonnet fine. Files: docs only.*

- `driftwood-plan.md`: add a STATUS header — plan largely implemented;
  deviations: outer regen loop removed (see agent guide rule 3), FS1
  mode-dependent, TIME_ONLY bench switch exists, freeze/looper deprioritized.
- `driftwood-phases.md`: the tag table predates the debugging saga; add a
  "post-phase6 reality" section listing the key commits (codec default, DMA
  self-heal, channel-sum, drive make-up, age loop-gain fix, birdie fix) or
  regenerate from `git log --oneline`.
- `src/driftwood/CONTEXT.md`: verify glossary matches as-built (Regen entry
  already updated; check Freeze/Havoc/Shimmer wording).
- Root `AGENTS.md`: confirm the Rust pointer section (added alongside this
  queue) still accurate.

## Task 5 — Cassette parity check
*Size: small. Model: Sonnet fine.*

Cassette shares `dsp` and inherited changes it never bench-saw: warble
amount smoothing + `flutter_share` law, fastmath swaps, `TapeSat` untouched.
Read `src/main.rs` + `src/cassette.rs` against current dsp APIs; build
(`make build`); note in `rust/README.md` that cassette needs a bench re-pass
before trusting (its audio was never verified post-refactor). Do NOT redesign
anything — this is a drift audit.

## Task 6 (only if the bench demands) — reverb modal density
*Size: medium-large. Model: Opus. Files: `dsp/src/pt2399_reverb.rs`.*

If SPACE voicing hits a quality wall (metallic/sparse tails), grow the tank:
4 → 8 combs + input diffusion allpass. CPU first: check the overrun meter has
headroom on hardware. Every stability/regression test must be extended to the
new topology; `required_len` grows — check `REVERB_CAP` in the binary and the
boot assert. Keep `output_norm_for` calibrated (re-measure the resonant test).

---

## Standing interaction notes (how this user works)

- They report symptoms tersely and accurately ("gated by guitar volume",
  "toggle 3 up, knob 1 down kills it") — their bisects found the two biggest
  bugs. Give them ONE crisp bench experiment per round, with an exact decode
  table for what each observation means.
- They can enter DFU and leave the pedal connected: check `dfu-util -l` and
  flash for them (`make flash-driftwood`).
- Commit style: one finding per commit, bench quote + mechanism + guard test
  in the message. Push after every commit (they read from their phone via the
  `driftwood` branch on GitHub).
- Docs travel in the same commit as the change (repo lockstep rule).
