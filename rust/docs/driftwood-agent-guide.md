# Driftwood — Agent Engineering Guide

**Read this before touching any Driftwood code.** Every rule below was paid for
with a real bench failure (noted in *why*), and most are enforced by a host
test (noted in *guard*). If a change violates a rule, the change is wrong —
not the rule. The full debugging history lives in `git log` on the `driftwood`
branch; the design plan (`driftwood-plan.md`) is the original intent, this file
is the as-built truth.

## The iron rules of the signal path

1. **Never put a compander inside a feedback loop.** Recirculate in the
   *compressed domain*: `Bbd::pre_mix(input, read·fb)` feeds back exactly what
   was stored. Expanding + re-compressing gives an ideal 2:1 compander a stable
   non-zero fixed point (A\* = fb²·REF) — repeats that never decay.
   *Why:* the "eternal loop" bug. *Guard:* `bbd::feedback_loop_repeats_decay_to_silence`.

2. **Every nonlinearity inside a loop needs make-up.** `knee_clip(x·d)/d`,
   never a bare `x·(1+k)` — any bare multiplier in a feedback path IS loop
   gain. *Why:* THE runaway — age's `(1+drive)` in the comb feedback put loop
   gain at 1.04 at default knobs. *Guard:* `pt2399_reverb::age_adds_grit_but_no_loop_gain`.

3. **No outer feedback loops around resonant networks.** An outer loop's true
   gain is `regen × network resonant gain` (~1/(1−fb), i.e. 5–50×): it either
   runs to inf→NaN or a bounded permanent scream. Regeneration belongs
   *inside* the network (comb feedback), which is stable by construction.
   *Why:* two bench failure modes of the removed outer regen loop.
   *Guard:* topology — `SpaceEngine::process` has no recirculation; keep it so.

4. **Resonant banks need output normalization.** A sparse comb bank has
   ~1/(1−fb) gain at mode frequencies — broadband unity but a
   frequency-selective amplifier in any room/rig loop.
   `Pt2399Reverb::output_norm_for(fb)` holds peak gain constant: more regen =
   longer tails, never louder. *Guard:* `resonant_peak_gain_is_bounded_across_regen`.

5. **Wet paths must MEASURE unity — never assume it.** The drive knob was an
   uncompensated 1→3.3× booster (measured). `Bbd::drive_makeup(drive)`
   compensates; engine and test share the formula so they can't drift.
   *Guard:* `wet_path_net_gain_is_unity_across_drive_and_level` (a drive×level
   gain grid). When adding any gain-affecting knob, extend the grid.

6. **Quantizers inside loops truncate toward zero.** Round-to-nearest can
   return energy each pass → sustained limit-cycle "birdie" tones at the
   quantization floor, excited by any knob transient.
   *Guard:* `no_quantizer_limit_cycle_birdies`.

7. **Delay-time changes inside a feedback loop must be rate-limited below
   1 sample/sample** (`MAX_GLIDE = 0.5`): a faster glide moves the read head
   backwards over already-read material, duplicating energy into the loop
   (k·fb ≫ 1 runaway). Use *proportional-capped* glide
   (`step = clamp(0.0005·err, ±MAX_GLIDE)`) — pure rate limiting is bang-bang
   and chirps on every knob step.

8. **Clips in loops need a linear region.** `fastmath::knee_clip` (exactly
   linear below the knee), never `soft_clip` (cubic bends at every level →
   1–3% distortion per pass accumulates into fuzz over recirculations).

9. **Every knob is smoothed.** All engine parameters that touch audio get
   ~10 ms per-sample one-pole smoothing (`v_s += 0.001·(v − v_s)`), including
   inside dsp modules (`Warble` amount, `Pt2399Reverb` feedback/damp). Raw
   per-block parameter steps = zipper pops and loop transients.
   `PagedKnobs` quantizes output to 1/512 so block-rate memoization
   (`powf`/`expf` gates) actually hits despite ADC noise.

10. **fastmath, not libm, in per-sample code.** Software `sinf`/`sqrtf`/
    `floorf` at 96 kHz caused sustained audio-ISR overrun (locked pedal).
    `dsp::fastmath` provides `sin_01`, `sqrt`, `round`, `soft_clip`,
    `knee_clip` — all host-tested against libm. libm stays in block-rate
    mapping code only. Modulated delay reads in bright feedback loops use
    **cubic Hermite** (`DelayLine::read_cubic`) — linear interp regrinds HF
    error every pass.

11. **Compander envelopes stay slow (8 Hz).** Faster envelope followers ripple
    at audio rate on low notes; the ripple modulates gain into IM sidebands
    ("digital fuzz").

12. **Stability tests must sweep EVERY parameter that touches a loop.** THE
    runaway shipped because no stability test ever called `set_age` — the
    suite covered drive=0 only. When adding a loop-adjacent parameter, add it
    to the stability sweeps in the same commit.

## Architecture (as built)

```
bin/driftwood.rs   control loop, ISR, LED diagnostics, SDRAM partition, DFU
src/driftwood/
  engine.rs        chain: in → g_in → TIME → SPACE → movement amp → soft_limit
                   (TIME_ONLY const = bench isolation switch)
  time_engine.rs   BBD delay / tape-slip / looper; freeze; glide; smoothing
  space_engine.rs  PT2399 reverb wrapper; internal regen law; shimmer budget
  movement_engine.rs LFO routing (amp/time/space targets)
  params.rs        pages, modes, DEFAULT_KNOBS, accessors
  paging.rs        PagedKnobs (soft-takeover + 1/512 quantize)
src/board.rs       HAL: knobs/toggles/footswitches/LEDs/clock/DFU/raw_leds
src/delay.rs       shared DelayLine (linear + cubic reads, looper peek/poke)
dsp/               pure host-testable DSP (50 tests): bbd, pt2399_reverb,
                   pitch, movement, looper FSM, warble, fastmath, takeover
```

Key laws shared between engine and tests (never fork them):
`Bbd::drive_makeup`, `Pt2399Reverb::output_norm_for`, the shimmer budget
`(0.3 + 0.7·regen)·0.5·(1−fb)` capped 0.08.

## Hardware & bench operations

- **Board: Daisy Seed 1.2 → `seed_1_2` feature is the default.** Wrong codec =
  silence (`seed_1_1`) or motorboating (`seed`). Never change the default.
- **The chain input sums L+R** (`left + right` in the ISR) — the guitar lands
  on one codec channel; do not "simplify" back to left-only (silent pedal).
- **DFU**: hold both footswitches + **KNOB_5 (K5) fully CCW** ~1.5 s, or
  BOOT+RESET with the enclosure open. Flash: `make flash-driftwood` (the
  trailing `dfu-util get_status` error after "File downloaded successfully" is
  benign). An agent CAN flash directly when the pedal is in DFU
  (`dfu-util -l` shows `0483:df11`).
- **Workflow per change**: edit → `cargo test -p dsp --target
  x86_64-unknown-linux-gnu` (all 50 must pass) → `cargo build --release
  --bins` → `cargo clippy --release --bins -- -D warnings` (and `-p dsp`) →
  commit (small, one finding per commit, bench context in the message) → push
  `driftwood` → flash.
- **LED diagnostics currently in the firmware** (see `bin/driftwood.rs` top
  comments): LED1 = freeze/looper + input-activity meter + DMA-error flicker +
  ISR-alive tick; overrun raw-set from the ISR; both-LED strobe = panic;
  alternating = HardFault. The DMA self-heal (clear ALL stream-1 flags on
  `Err`) and the non-panicking ISR are **permanent** — the BSP misses error
  flags and an unwrap there bricks the pedal on one late block.
- **ISR budget**: block = `daisy::audio::BLOCK_LENGTH` (32/64, NOT 48) at
  5 000 cycles/sample; the overrun meter tells the truth, estimates don't.

## How to run a voicing session (the TIME playbook, reuse for SPACE)

1. Isolate if needed (`TIME_ONLY`-style const in `engine.rs`).
2. One change-set per bench report; put the user's exact words in the commit.
3. Constants live in the engine files with bench-note comments — tune, don't
   restructure.
4. User grades in plain language ("warble 20% too fast") → convert to one
   constant each → flash → repeat. Keep rounds under ~5 minutes.
5. Anything that smells like instability is NOT voicing — stop and write a
   failing host test first.
