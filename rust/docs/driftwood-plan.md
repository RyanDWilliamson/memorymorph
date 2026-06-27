# Driftwood — New Rust HotHouse Pedal (Design Plan)

## Context

A new Rust firmware for the HotHouse / Daisy Seed platform (STM32H750, 96 kHz,
block size 48), joining the existing family — Memory Morph (C++ tape-echo→ambient
morph), Echorec (C++ Binson emulation), Cassette LoFi Junky (Rust Gen-Loss-style
degradation). **Driftwood** must carve a *distinct* identity from those: it is a
**dual-engine, Mood-style ambient machine** — a lo-fi TIME engine (looper/delay/
tape-slip) running into a lo-fi SPACE reverb, animated by an internal MOVEMENT
LFO — rather than another single lo-fi delay.

Inspirations mapped: **Mood mkII** → dual engines + internal movement + freeze;
**Generation Loss mkII** → modeled lo-fi degradation throughout; **Kilobyte** →
BBD delay + "havoc" runaway; **Météore** → modeled-PT2399 lo-fi reverb + bloom;
**Caroline Tremolo** → the MOVEMENT engine (incl. harmonic trem).

**Hard rule from the user — analog = modeled, not faked.** Every analog-sounding
voice is a physical/behavioral model of the real circuit: BBD (companding +
clock feedthrough + bandwidth collapse under feedback), tape (saturation + wow/
flutter), PT2399 reverb (12-bit-ish quantization, ~10 kHz BW, companding, dark
regen), optical/harmonic tremolo. Consistent with the project's existing custom
tape-saturation and Echorec magnetic-drum models. No stylized shortcuts.

## Control surface

Both engines always run in series; balance is by their mix knobs. No per-engine
stomp. The 6 knobs are **paged** (Toggle 1), with **soft-takeover** so a param
never jumps when the page changes.

**Footswitches**
- **FS1 = Tap / Transport (mode-dependent on Toggle 2).**
  - DELAY / TAPE-SLIP: short = tap tempo · hold = momentary **FREEZE / HAVOC**
  - LOOPER: short = record → play → overdub cycle · hold = stop / clear
  - LED_1 = tap pulse / looper state (recording vs playing).
- **FS2 = Bypass** (clean dry pass-through, **tails ring out** on bypass).
  LED_2 = effect active.
- **FS1 + FS2 held + guard (mix dry / toggles) = DFU bootloader.** Use the
  proven PG3/BOOT0 + `SCB::sys_reset()` path (no software jump); harden cache/
  systick/VTOR as in the Cassette firmware. (Bootloader gesture is mandatory.)

**Toggles**

| Toggle | Function | Up / Mid / Down |
|--------|----------|-----------------|
| 1 | Knob PAGE | TIME / MASTER / SPACE |
| 2 | TIME mode | Looper / Delay / Tape-slip |
| 3 | SPACE character | Dark / Modulated / Shimmer |

**Knob families** — each knob's role is the *analogous* one on every page
(honors the user rule "alt maps to the similar knob": delay-time ↔ other-time,
drive/gain ↔ wet/dry blend):

| Knob | family | TIME page | MASTER page | SPACE page |
|------|--------|-----------|-------------|------------|
| 1 | time / rate | delay time · loop length | movement rate (tap-syncable) | reverb decay |
| 2 | feedback / amount | repeats | movement depth | reverb regen |
| 3 | mod / character | warble depth | movement shape (sine/harmonic/sq/ramp) | reverb mod depth |
| 4 | drive / degrade | BBD drive · degrade | **INPUT gain** | reverb age/grit |
| 5 | mix / blend | delay mix | movement target (amp/time/space) | reverb mix |
| 6 | level | TIME-engine level | output level | reverb tone |

(A global dry/wet on MASTER was dropped as redundant with the per-engine TIME/
SPACE mix knobs; that slot funds the user-requested **INPUT gain**, placed in the
drive/degrade family on K4, with movement target shifting to K5.)

## Signal architecture

```
IN ─► [TIME engine] ─► [SPACE engine] ─► (mix dry tails on bypass) ─► OUT
        delay/loop        lo-fi PT2399
        + BBD/tape        reverb
   MOVEMENT LFO ──────────┴── modulates target: amplitude(trem) /
                              time(vibrato) / space(swell)
```

- **TIME engine** (Toggle 2): LOOPER (hands-free, SDRAM buffer, mode-dependent
  FS1), DELAY (BBD-modeled, feedback, Kilobyte havoc on FS1-hold), TAPE-SLIP
  (loop that drifts/varispeeds via the tape model).
- **SPACE engine**: modulated short-delay-line reverb with a modeled-PT2399
  lo-fi front; Toggle 3 = DARK (dub/cave) / MODULATED (seasick wash) / SHIMMER
  (+oct into regen). FS1-hold blooms it toward self-oscillation.
- **MOVEMENT**: one internal LFO/ramp (no expression jack exists — HAL reads
  only the 6 ADC1 knobs). TARGET selects amplitude (tremolo, incl. harmonic),
  time (vibrato), or space (swell). Rate tap-syncable.
- Serial wiring means repeats get reverberated → dub/ambient washes; freeze +
  havoc produce sustained blooms.

## Gain staging

Work in a normalized float domain (±1.0 ≈ nominal guitar peak; f32 gives large
internal headroom). Each modeled stage is calibrated to sit in its sweet spot at
noon — BBD, tape, and PT2399 are all level-dependent, so the operating point is
designed, not accidental.

1. **Input:** user-facing **INPUT gain** (MASTER K4) sets the operating point
   for varied pickups; TIME drive (K4 on TIME page) pushes the BBD/tape harder
   from there.
2. **Modeled companders stay inside the models.** BBD and PT2399 carry their own
   companders (compress→delay→expand) with the real pumping/breathing artifacts —
   modeled as *part of the sound*, **not** a global utility AGC. (Distinct from
   Echorec ADR 0003, which rejected a *utility* compander; here the companders
   are integral to the modeled circuits — record this distinction in an ADR.)
3. **Inter-stage normalization (critical):** the TIME engine output is
   re-normalized to a stable level before SPACE, so raising delay drive changes
   *character*, not the raw level feeding the reverb (prevents wash blow-up).
4. **Feedback/regen headroom:** each engine's feedback path uses a calibrated
   *gentle* soft-saturator so freeze/havoc self-oscillates musically and can
   never NaN/hard-clip. Avoid flat `tanhf` in feedback (CLAUDE.md caution — it
   flattened waveforms with the pitch-shifter); prefer a waveform-preserving
   shaper, leaning on the BBD/PT2399 models' own saturation.
5. **Output:** dry path is unity (click-free bypass, no engage jump); per-engine
   mix law tuned so noon is balanced and full-wet ≈ dry loudness; MASTER K6 =
   output trim; a final soft-limiter/clamp before the codec protects the DAC
   during blooms.

## Parameter smoothing — no digital zipper

Every control value is slew-limited before use (reuse `OnePole`/`one_pole_coeff`
as a control-rate smoother) so block-rate (48-sample) updates never zipper.
Specifics:
- **Delay / tape-slip time:** feed the existing linear-interpolated fractional
  `DelayLine` read a *slewed fractional length target* → sweeping time produces
  **tape/BBD varispeed pitch glide** (analog-faithful, on-brand, cheap). No
  zipper, no clicks.
- **Reverb decay / regen:** smooth the feedback/decay *coefficient* (no pitch
  concept) → click-free decay changes.
- **Looper length changes:** crossfade old→new read point to avoid clicks.
- All mix/level/tone knobs: one-pole smoothed.

## Reuse vs new code

**Reuse (Rust):**
- `rust/src/hothouse.rs` — control HAL (6 knobs, 3 toggles, 2 FS, 2 LED,
  `FootswitchTracker` long-press, `BootloaderGesture`) — unchanged.
- `rust/dsp/` — `OnePole`/`one_pole_coeff`, `TapeSat`, `TubeStage`,
  `CassetteTone`, `Warble`, `Hiss`, `Dropout` — host-tested building blocks.
- SDRAM pattern (`&'static mut [f32]`), audio-ISR pattern, Makefile/Cargo setup.

**New DSP modules in `rust/dsp/src/` (all host-testable, `cargo test -p dsp`):**
- `bbd.rs` — bucket-brigade delay model: companding (compress→delay→expand),
  clock-rate-dependent bandwidth, aliasing/clock feedthrough, bandwidth collapse
  with feedback.
- `pt2399_reverb.rs` — modulated multi-tap/FDN reverb with modeled PT2399 lo-fi
  (quantization, ~10 kHz BW, companding, dark regen); shimmer = +oct in regen.
- `movement.rs` — LFO/ramp with sine/harmonic-trem/square/ramp shapes + target
  routing; reuse `Warble` for vibrato-style pitch movement where apt.
- (Looper transport/overdub buffer logic lives in the engine module, not `dsp`.)

**New firmware structure** — refactor the workspace so two pedals coexist:
- Promote shared glue into `rust/src/lib.rs` (controls wiring, paging +
  soft-takeover, params publish, DFU). Keep Cassette as one binary.
- Add `rust/src/driftwood/` engine modules (`time_engine.rs`, `space_engine.rs`,
  `movement.rs`, `params.rs`) + a `rust/src/bin/driftwood.rs` main (audio ISR,
  control loop, SDRAM alloc).
- New `Params` struct holds per-page stored values + soft-takeover state.

## Implementation phases

1. **Scaffold + workspace refactor** — lib.rs extraction, `driftwood` binary
   that passes audio through, paging + soft-takeover + params publish, FS/LED/
   DFU wired. Bench-flash, confirm clean passthrough + bypass tails + DFU.
2. **DSP modules host-first (TDD)** — `bbd.rs`, then `pt2399_reverb.rs`, then
   `movement.rs`, each with `cargo test -p dsp` unit tests verifying the modeled
   behavior (companding gain, bandwidth vs clock, reverb decay, LFO shapes).
3. **TIME engine** — wire BBD delay + tape-slip + looper transport (mode-
   dependent FS1, overdub, freeze/havoc). Bench-tune.
4. **SPACE engine** — PT2399 reverb + Dark/Modulated/Shimmer + bloom on hold.
5. **MOVEMENT** — LFO + target routing + tap sync; harmonic-trem model.
6. **Integration + voicing pass** — serial blend, mix-knob calibration, CPU
   budget check; tune degradation order if tight.

## Engineering constraints / contingencies

- Boost mode mandatory; `ProcessAllControls`-equivalent first in callback; no
  heap; LED updates in the main loop, not the audio ISR.
- Both modeled engines must coexist @ 96 kHz on the one M7. If CPU-tight, degrade
  in order: drop BBD oversampling → reduce reverb network density → slow the
  modulation update rate. Never sacrifice either engine's perceptual presence.
- SDRAM (64 MB) is generous; lo-fi looper may store at reduced effective rate
  for longer loops. Every delay/loop buffer in SDRAM.
- Mono in → dual-mono out (reverb may be stereo internally, summed to out).

## Documentation (write during implementation — plan mode is read-only now)

- `rust/src/driftwood/CONTEXT.md` — glossary: TIME engine, SPACE engine,
  MOVEMENT, page, soft-takeover/pick-up, freeze, havoc, bloom, BBD model,
  PT2399 model, tape-slip, shimmer, trails. (Echorec's CONTEXT.md is the
  template.)
- ADRs worth recording (hard-to-reverse, surprising, real trade-off):
  - "Knob paging with soft-takeover" (vs more controls / fewer params).
  - "Dual modeled engines on one M7 @ 96 kHz" (CPU budget + degradation order).
  - "Mode-dependent FS1 (tap vs looper transport)" (overloaded footswitch).
  - "Modeled companders are integral, not a utility AGC" (gain staging; contrast
    with Echorec ADR 0003).
- Update `AGENTS.md` / `README.md` / `rust/README.md` in lockstep (project rule).

## Verification

- **Host:** `cargo test -p dsp --target x86_64-unknown-linux-gnu` for every new
  DSP module; add `validate`/`bringup`-style codec-independent checks for paging
  + soft-takeover + footswitch state machine.
- **Bench:** flash via the DFU gesture; verify per phase — clean passthrough,
  bypass trails, each TIME mode + looper transport, each SPACE character, freeze/
  havoc bloom, movement targets + tap sync, paging with no value jumps, and DFU
  re-entry without opening the enclosure.

## Picking this up from your iPhone

This plan lives at `/home/rdw/.claude/plans/lets-discuss-a-new-inherited-pebble.md`
(local). To reach it from the phone: once approved, commit it into the repo
(e.g. `rust/docs/driftwood-plan.md`) on a `driftwood` branch and push; then open
**claude.ai/code** against the `hothouse` repo on iPhone — the plan travels with
the code and stays version-controlled next to AGENTS.md. (Continuing this exact
conversation on mobile would require a cloud session; the local CLI session does
not sync to the phone.)
