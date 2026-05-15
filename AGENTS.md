# Memory Morph — Agent Instructions

## What this is

A C++ guitar effects pedal firmware for the **Cleveland Music Co. HotHouse**
platform (Daisy Seed / STM32H750 ARM Cortex-M7, 480 MHz boost, **48 kHz**).

The pedal carries **two complete instruments** selectable at runtime via a
hidden footswitch combo. Boot always lands in DMM mode.

1. **DMM mode** (default) — Chase Bliss–inspired morphable chain:
   Electro-Harmonix Deluxe Memory Man preamp/compander → BBD delay → ambient
   shimmer reverb, swept by a single MORPH macro knob.
2. **SDD-555 mode** — circuit-level model of the Roland SRE-555 Chorus Echo
   fused with the SDD-320 Dimension D: NE570 VCA compander (the "dirt source")
   → tape echo (single-tap, **50–500 ms log** — authentic SRE-555 multi-head
   range; tape-darkened soft-clipped feedback) → BBD chorus with trapezoidal
   LFO → 3-spring Accutronics tank → AMS Non-Lin or Wildcard Resonator verb.
   Built up in phases; current status below.

Mode switch combo: hold **FS1 + FS2** while all toggles are DOWN and Mix is
fully dry, for **3 seconds**. Both LEDs blink alternating 3× to confirm.

Current branch: `dmm-deep-dive`. SDD-555 status — Phases 1–7 complete plus
the **tape-echo addition** (the SRE-555's namesake — was missing from the
original plan; wired in after Phase 7 once the omission was caught). Mode
dispatch; NE570 compander; SRAM-shared tape echo on KNOB_2/3 with FS1 tap
sync; BBD/Eventide/Dimension D chorus algorithms behind SW2; 3-spring
Accutronics tank; AMS Non-Lin gated reverb + Wildcard Resonator behind SW3;
SDD-555 MORPH lerp table for KNOB_1; MechAge HF rolloff + breathing LFO on
KNOB_5. Phase 8 (on-hardware tuning) is the last remaining step.

## Build & flash

```bash
cd src/MemoryMorph          # MUST be in this directory, not src/
make                        # produces build/MemoryMorph.bin
make program-dfu            # USB DFU flash via dfu-util (exit code 2 = normal)
```

Prerequisites: `arm-none-eabi-gcc`, `dfu-util`, git submodules initialised.

```bash
git submodule update --init --recursive
```

## Repository layout

```
src/hothouse.h / hothouse.cpp      — HotHouse board support (do NOT modify)
src/MemoryMorph/memory_morph.cpp   — AudioCallback dispatcher + DmmBlock / Sdd555Block per-sample loops + main()
src/MemoryMorph/constants.h        — shared math: kTwoPi, kSampleRateF, OnePoleCoeff()
src/MemoryMorph/morph.h            — MorphParams + ComputeMorph() interpolation (DMM)
src/MemoryMorph/plate_reverb.h     — PlateReverb (Schroeder mono-in/stereo-out, DMM)
src/MemoryMorph/dmm_chain.h        — DmmChain (SA571 compander + BBD/biquad filters, DMM)
src/MemoryMorph/tap_tempo.h        — TapTempoState (FS1 tap/freeze state machine, shared)
src/MemoryMorph/shimmer.h          — ShimmerVoice (HPF + PitchShifter + auto-duck, DMM)
src/MemoryMorph/ne570.h            — Ne570 (NE570 VCA compander with even-order dirt, SDD-555)
src/MemoryMorph/bbd_chorus.h       — BbdChorus (BBD / Eventide / Dimension D algorithms, SDD-555)
src/MemoryMorph/spring_reverb.h    — SpringReverb (3-spring Accutronics 8AB2D1A tank, SDD-555)
src/MemoryMorph/nl_verb.h          — NlVerb (AMS Non-Lin gated + Wildcard Resonator, SDD-555)
DaisySP/                            — git submodule (do NOT modify)
libDaisy/                           — git submodule (do NOT modify)
```

Each `.h` file is a standalone DSP struct with `Init(sr)` / `Reset()` /
per-sample inline methods. `memory_morph.cpp` is pure glue — mode dispatch,
parameter wiring, LED logic, and the two per-sample audio loops.

## Non-negotiable constraints

### 1. SDRAM placement for large buffers
```cpp
// CORRECT — DelayLine and PitchShifter must be in SDRAM
static DelayLine<float, 96000> DSY_SDRAM_BSS delay_line;
static PitchShifter            DSY_SDRAM_BSS pitch;

// WRONG — hard fault at init
static DelayLine<float, 96000> delay_line;

// PlateReverb (~60 KB) fits in SRAM — no DSY_SDRAM_BSS needed
static PlateReverb reverb;  // BSS zero-initialises all buffers
// NOTE: DSY_SDRAM_BSS cannot be applied to struct members,
// so custom structs always live in SRAM regardless of size.
```

### 2. Boost mode + sample rate (mandatory for this chain)
```cpp
hw.Init(true);
hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);  // 48 kHz
hw.SetAudioBlockSize(48);
```

### 3. Audio callback
- First line must be `hw.ProcessAllControls()`
- No `malloc`, `new`, `printf`, or any blocking call
- Read mono input `in[0][i]`; write `out[0][i]` and `out[1][i]`

### 4. LED updates
Call `Led::Set()` and `Led::Update()` only from the `while(true)` main loop,
rate-limited to 1 kHz via `System::GetNow()`.

### 5. Do not use `daisysp::Chorus`
Its internal buffer is hardcoded for 48 kHz operation but the API is fragile.
Use `DelayLine + Oscillator` LFO instead — already implemented.

### 6. No `= {0}` on large struct-member arrays
Aggregate zero-initialisers force buffers into FLASH `.data` instead of BSS,
wasting flash 1:1 with the buffer size. Use plain declarations and rely on
BSS zero-init at startup (static storage duration guarantees it):

```cpp
// WRONG — emits N floats of zeros into FLASH .data
struct Foo { float buf[2400] = {0}; };

// CORRECT — BSS zeroes at startup, no FLASH cost
struct Foo { float buf[2400]; };
```

This bit Phase 5: spring_reverb.h had ~40 KB of `= {0}` arrays and overflowed
the 128 KB FLASH region by 24 KB. bbd_chorus.h had been wasting ~10 KB silently
since Phase 3. Single-float defaults (`float foo = 0.f;`) are fine — only the
aggregate array initialisers waste flash.

### 7. No tanhf in the shimmer feedback loop
`tanhf` in the **shimmer/reverb feedback path** (reverb → PitchShifter → back to reverb)
progressively flattens waveforms → grain crossfades cancel → shimmer cuts out.

`tanhf` IS intentionally used in the **delay feedback path** (mild 2× overdrive on the
recycled signal only) to model BBD input op-amp clipping — this is safe because the
reverb's allpass diffusion scrambles the waveform before it reaches the PitchShifter.

Permitted locations: final output mix, `dmm.Compress()`, delay feedback write path,
**DMM preamp only** (`tanhf(sig × preamp_gain)` models the NJM4558 input op-amp).
Prohibited: shimmer/reverb recirculation loop, **SDD-555 preamp**. The real SRE-555
input was a clean JRC4558 buffer — its `Ne570::Compress()` is the only
intentional nonlinearity. Use linear gain (`sig * sdd_drive`) in SDD-555 mode.

## DMM signal chain (current implementation)

Per-sample path inside `AudioCallback`:

```
guitar in
  → dc_block
  → tanhf(sig × preamp_gain)                    — op-amp preamp clip
  → dmm.Compress() × post_gain                   — SA571 RMS 2:1 compressor (sidechain HPF at 164 Hz)
  → dmm.AaFilter()                               — 8 kHz Butterworth anti-alias before BBD
  → tone_filter (user Tone knob, 4000–18000 Hz)
  → dmm.PreEmph()                                — +9.5 dB HF shelf at 1.5 kHz (BBD noise-reduction trick)
  → dmm.BbdFilter(c)                             — one-pole bandwidth LPF (narrows with delay time)
  → dmm.AsymSat()                                — asymmetric soft-clip (2nd-harmonic from x² term)
  → fb_out = dmm.FbFilter(delay_line.Read())     — 5 kHz feedback warmth LPF
  → fb_sat = tanhf(fb_out × fb × 2) × 0.5       — feedback soft-clip (BBD input op-amp model)
  → delay_line.Write(AsymSat(BbdFilter(PreEmph(input))) + dmm.Noise() + fb_sat)
                                                  — Noise() = -65 dBFS pink-ish floor (BBD intrinsic noise)
  → delay_line.Read() → dmm.AiFilter()           — 8 kHz Butterworth anti-image reconstruction
  → dmm.DeEmph()                                 — -9.5 dB HF shelf at 1.5 kHz (inverse of PreEmph)
  → dmm.Expand()                                 — SA571 matched expander → "breathing" noise floor
  → reverb.Process()
      shimmer path: HPF(800 Hz) → PitchShifter(+12 st, fun=0.3) → × shimmer_amt → reverb input
  → tanhf(verbL) × reverb_send × 2.0            — reverb pre-clip before output mix
  → tanhf() on final output mix
```

The PreEmph / DeEmph shelf pair around the BBD, the AsymSat 2nd-harmonic, the
injected noise floor, and the matched Expander are the four authenticity
additions that take the model from "clean BBD simulation" to "sounds like the
box". Net frequency response around the BBD is unity within ±0.5 dB; the
character lives entirely in the time-domain breathing between repeats.

## DMM drive levels (SW1)

All three positions use the **same compander model** — only gain differs.
Guitar volume directly controls saturation depth within each mode.

All three `post_gain` values are equal — the SA571 compressor targets the same 0.25 RMS
output in every mode, so equal `post_gain` gives matched perceived loudness. The modes
differ in dynamics and harmonic character, not in volume.

| SW1 | Mode | `preamp_gain` | `post_gain` | Character |
|---|---|---|---|---|
| UP | High | 5.0× | 0.80× | Heavy compressor pumping, rich harmonics |
| MID | Med | 2.5× | 0.80× | Nominal DMM operating point |
| DOWN | Low | 1.2× | 0.80× | Gentle, most transparent |

## SA571 compander time constants (48 kHz)

**Critical**: these must be computed as `1 - exp(-1/(τ × sr))`, NOT as small
ad-hoc decimals. Wrong values cause the compressor to lock at max gain,
producing hard clipping → square-wave harmonics → audible 1–3 kHz drone.

Values match the real DMM PCB's 100 µF rectifier cap rather than the SA571
datasheet voice setting — slower release is what produces the audible noise-
floor breathing between repeats.

```cpp
// Attack  ~50 ms:  1 - exp(-1 / (0.050 * 48000)) ≈ 0.000417
// Release ~250 ms: 1 - exp(-1 / (0.250 * 48000)) ≈ 0.0000833
static constexpr float kCompAttack  = 0.000417f;
static constexpr float kCompRelease = 0.0000833f;

// kCompMaxGain is capped at 2.0 — higher values cause audible digital whine
// when the sidechain HPF removes low-frequency content from the envelope.
static constexpr float kCompMaxGain = 2.0f;

// Sidechain HPF at ~164 Hz (one-pole, kCompHpfC = 0.02124) prevents 60 Hz hum
// from driving compressor gain upward and causing audible pumping.
static constexpr float kCompHpfC    = 0.02124f;
```

Expander uses the same time constants as the compressor (matched pair). The
expander is now active in the DMM read path so the BBD noise floor breathes
correctly — see `DmmChain::Expand()` and the `dmm.Noise()` injection at the
BBD write point.

## Biquad LPF coefficients (Butterworth, 8 kHz)

Encapsulated in `DmmChain::Init(float sr)` — no longer global variables.
The same bilinear-transform coefficients are reused for both the anti-alias
(`dmm.AaFilter()`) and anti-image (`dmm.AiFilter()`) filters. State is separate.

Call `dmm.Init(sr)` once in `main()` before starting audio. `dmm.Reset()` zeroes
all filter state; call it on bypass entry/exit to silence transients.

## MORPH three-zone design (preserve these anchor values)

`sat_drive` was **removed** from `MorphParams` — saturation is now purely
controlled by SW1 (drive level), not morphed by the knob.

| MORPH | Zone | `delay_send` | `reverb_send` | `mod_depth_scale` | `reverb_decay` | `reverb_lpf_hz` |
|---|---|---|---|---|---|---|
| 0.0 | Tape | 0.00 | 0.00 | 0.00 | 0.75 | 9000 |
| 0.5 | Echo | 1.00 | 0.30 | 0.50 | 0.78 | 8500 |
| 1.0 | Ambient | 1.00 | 1.00 | 1.00 | 0.95 | 4000 |

## Control map

| Control | Identifier | Function |
|---|---|---|
| Knob 1 | `KNOB_1` | MORPH macro |
| Knob 2 | `KNOB_2` | Delay time (log, 50 ms–2000 ms) |
| Knob 3 | `KNOB_3` | Feedback (0–0.97) |
| Knob 4 | `KNOB_4` | Mod depth (scaled by MORPH) |
| Knob 5 | `KNOB_5` | Tone LPF (log, 4000 Hz–18 kHz) |
| Knob 6 | `KNOB_6` | Dry/wet mix |
| Toggle 1 | `TOGGLESWITCH_1` | Drive: UP=High / MID=Med / DOWN=Low |
| Toggle 2 | `TOGGLESWITCH_2` | Mod type: UP=Chorus / MID=Vibrato / DOWN=Wow |
| Toggle 3 | `TOGGLESWITCH_3` | Reverb: UP=Short / MID=Long / DOWN=Shimmer |
| Footswitch 2 | `FOOTSWITCH_2` | Bypass (LED 2) |
| Footswitch 1 | `FOOTSWITCH_1` | Tap tempo short-press / Freeze hold ≥1500 ms (LED 1) |

## Mode switch (DMM ↔ SDD-555)

Detected in the `while(true)` main loop at 1 kHz:

- Both `FOOTSWITCH_1` **and** `FOOTSWITCH_2` pressed
- All three toggles `TOGGLESWITCH_DOWN`
- `KNOB_6` (mix) < 0.02
- Held continuously for **3 seconds**

On trigger: `hw.StopAudio()` → both LEDs blink alternating 3× at 150 ms →
`tap.is_freeze` / `tap.active` cleared → `active_mode` flipped → `hw.StartAudio()`.

The DFU bootloader hold (10 s + same toggle/mix combo on **FS1 only**) is
guarded with `!fs2_held` so the mode-switch combo cannot accidentally trigger
DFU. The bypass-settled early-return resets the **active mode's** state only;
DMM and SDD-555 chains are isolated.

## SDD-555 signal chain (in progress)

Per-sample path inside `AudioCallback`'s `active_mode == SDD555` dispatch.
Bracketed sections are pending phases; bullet-listed elements are implemented:

```
guitar in
  → dc_block                                       — shared with DMM
  → sig × sdd_drive                                 — SW1: Hot=2× / Warm=1× / Clean=0.5×  (LINEAR — no preamp clip)
  → ne570.Compress()                                — NE570 VCA, RMS detector + sidechain HPF + polynomial dirt
  → tape echo (SRE-555 echo section):               — post-Phase-7 addition
      echo_smp     : KNOB_2 → 50–500 ms log (own mapping, not DMM's 2 s pipeline)
                     FS1 tap.tempo_s overrides, clamped to 500 ms
      feedback LPF : one-pole at 6 kHz (tape HF rolloff)
      feedback sat : tanhf soft-clip (BBD input op-amp model, safe outside chorus loop)
      sig = sig + delay_line.Read()                 — mono dry+wet sum feeds chorus
  → SW2 dispatch — chorus algorithm:                — Phase 4
      UP   chorus.ProcessBbd       : trapezoidal LFO, two taps @ 180° phase offset
      MID  chorus.ProcessEventide  : static pre-delays + shared PitchShifter (+0.20 st)
      DOWN chorus.ProcessDimensionD: half-swing trapezoidal LFO + cross-channel HPF
      common: pre_emp +6 dB / shelf 3 kHz before delay; de_emp_l/r −6 dB after;
              mono delay line 2400 samples (~50 ms headroom)
  → ne570_exp_l.Expand(wetL)   ╲
  → ne570_exp_r.Expand(wetR)   ╱  — matched expanders, one envelope per channel
  → MechAge LPF (per channel)                          — Phase 7
      block-rate cutoff lerps 18 kHz (KNOB_5=0) → 4 kHz (KNOB_5=1),
      modulated ±20% × age by a 0.4 Hz "breathing" LFO
  → SW3 verb selection — only one runs per sample:    — Phase 6
      UP   nl_verb.ProcessAms      : 6-allpass diffusion + envelope-armed gate (AMS RMX16)
      MID  nl_verb.ProcessWildcard : 5 combs at A2 harmonics (110/220/330/440/550 Hz)
      DOWN spring.Process          : 3-spring Accutronics tank with cross-coupling
  → wet = aged + verb * sm.verb_send                   — MORPH-scaled return (Phase 7)
  → lerpf(dry, wet, mix * bypass_ramp)                 — same bypass pattern as DMM
```

### SDD-555 MORPH lerp tables

Both SDD-555 presets share the `SddMorphParams` struct
(`echo_send`, `chorus_wet`, `chorus_depth_scale`, `verb_send`) and the
DMM-style three-zone shape. The NE570 preamp/compressor is always-on and its
level is set by SW1 — drive is **not** morphed. `chorus_wet` crossfades the
chorus block in/out so the Drive zone is truly clean NE570 dirt (the chorus
EQ pair and algorithmic processing are bypassed entirely, not just LFO-zeroed).

**SDD555_DELAY — `ComputeDelayMorph(m)`:**

Echo buffer keeps writing and recirculating at every MORPH position so a
sweep into the Echo zone doesn't reveal an empty tape.

| MORPH | Zone | `echo_send` | `chorus_wet` | `chorus_depth_scale` | `verb_send` | Character |
|---|---|---|---|---|---|---|
| 0.0 | Drive   | 0.0 | 0.0 | 0.0 | 0.0 | NE570 dirt only, dry signal |
| 0.5 | Echo    | 1.0 | 1.0 | 0.5 | 0.3 | Tape echo with light chorus + verb |
| 1.0 | Ambient | 1.0 | 1.0 | 1.0 | 1.0 | Full chorus + verb wash on the echo |

**SDD555_CHORUSVERB — `ComputeChorusVerbMorph(m)`:**

Tape echo is skipped entirely (the per-sample `if (tape_echo)` block doesn't
run). `echo_send` is forced to 0 across the full sweep.

| MORPH | Zone | `echo_send` | `chorus_wet` | `chorus_depth_scale` | `verb_send` | Character |
|---|---|---|---|---|---|---|
| 0.0 | Drive  | 0.0 | 0.0 | 0.0 | 0.0 | NE570 dirt only, dry signal |
| 0.5 | Chorus | 0.0 | 1.0 | 1.0 | 0.0 | Full chorus character, no verb |
| 1.0 | Verb   | 0.0 | 1.0 | 1.0 | 1.0 | Full chorus + full verb wash |

Interpolation is piecewise-linear between adjacent anchors in both tables.

### Three-preset mode switch combos

Each combo is a direct toggle with DMM. From either SDD-555 preset, the same
combo returns to DMM. To switch directly between Delay and Chorus Verb, route
through DMM (hit combo A, then combo B).

| Combo | Target | Pattern |
|---|---|---|
| A | `SDD555_DELAY`      | FS1+FS2 + **SW1 UP / SW2 DOWN / SW3 DOWN** + Mix dry, held 3 s |
| B | `SDD555_CHORUSVERB` | FS1+FS2 + **SW1 DOWN / SW2 UP / SW3 DOWN** + Mix dry, held 3 s |

The all-toggles-DOWN pattern is reserved for the 10 s DFU bootloader hold
(FS1 only, FS2 released) — each SDD-555 entry combo therefore has a unique
toggle pattern that cannot trigger DFU or the other preset.

The 3 s timer restarts whenever the combo breaks **or** the toggle pattern
changes mid-hold to the other combo's target (so a slide between patterns
can't accumulate time toward an unintended switch).

### MechAge (KNOB_5)

Inline state in `memory_morph.cpp`: one-pole LPF per channel + a single phase
accumulator for the ~0.4 Hz breathing LFO. Total state < 20 bytes.

```cpp
mech_base_hz = lerp(18000, 4000, age);
mech_hz      = mech_base_hz * (1 + sin(wow_phase·2π) * age * 0.2);
mech_c       = 1 − exp(−2π · mech_hz / sr);
```

Coefficient is computed block-rate; the LPF state itself advances per sample.
At age=0 the LPF cutoff sits at 18 kHz (effectively transparent); at age=1
it lerps down to 4 kHz with ±20% LFO modulation, giving a worn-tape feel
without the cost of a wow delay line.

### Tape echo (multi-head)

`memory_morph.cpp` inline — reuses the existing SDRAM `delay_line` (mono,
~96000 samples = 2 s capacity) since DMM and SDD-555 never run simultaneously.
`delay_line.Init()` is called on every mode switch to zero the previous
mode's residue (~5 ms SDRAM write, hidden under the LED blink).

Three playback heads at positions 0.33, 0.66, and 1.0 of the user's main
echo time model the SRE-555's actual multi-head transport. Each input
generates a triplet of taps spaced through one echo cycle, and the whole
pattern recirculates via the longest head's feedback. The multi-tap sum
feeds the chorus mono input, getting the tape-stage character through the
rest of the chain. Per-block flutter (5 Hz sine) and a noise-driven slow
drift integrator (~0.1 Hz red noise) wobble all three tap read offsets in
unison for authentic tape transport feel. A two-LPF-difference head bump
adds ~+4 dB at 100 Hz on the multi-head sum, modelling the playback head
gap-loss compensation EQ. The record write is run through an asymmetric
soft-clip (`x + α·x²` then tanh, DC-blocked) so the tape stage produces a
fatter 2nd-harmonic-rich saturation when driven hard.

```cpp
main_smp     = sdd_smooth_echo_smp · wf_mult                   // wow/flutter multiplier
tap1         = delay_line.Read(main_smp · 0.33)                 // head 1 (shortest)
tap2         = delay_line.Read(main_smp · 0.66)                 // head 2 (middle)
tap3         = delay_line.Read(main_smp)                        // head 3 (longest, primary)
sdd_fb_lpf_z += sdd_fb_lpf_c · (tap3 − sdd_fb_lpf_z)            // 6 kHz tape rolloff
fb_sat        = tanhf(sdd_fb_lpf_z · echo_fb)                   // soft-clip saturation
record_pre    = (sig + fb_sat) + kTapeAsymA · (sig + fb_sat)²   // record-stage asymmetric drive
delay_line.Write(tanhf(record_pre − tape_asym_z))               // tape_asym_z = slow HPF for DC
multi_head    = tap3 + 0.6·tap2 + 0.5·tap1                      // head-mix levels
head_bumped   = multi_head + 0.6·(head_lp1_z − head_lp2_z)      // ~+4 dB at 100 Hz
sig           = sig + head_bumped · sm.echo_send                // dry+wet sum feeds chorus
```

#### Echo time mapping

The real SRE-555 maxed out around **320 ms single-head / ~500 ms multi-head**
— it was a 3.75 ips tape transport with 4 playback heads, nowhere near the
2 s the DMM gives you. SDD-555 mode therefore reads KNOB_2 with its own
log curve rather than reusing the DMM `p_time` Parameter:

```cpp
sdd_knob_s = 0.05 · exp(KNOB_2 · log(0.5 / 0.05))   // 50–500 ms log
echo_time_s = tap.IsActive() ? min(tap.tempo_s, 0.5) : sdd_knob_s
```

`tap.GetTimeS()` is still called once at the top of `AudioCallback` for its
cancellation side effect — turning KNOB_2 clears `tap.active` via the same
mechanism in both modes.

#### Feedback

Feedback is capped at 0.95 below self-oscillation; the in-loop `tanhf` is
the BBD input op-amp model — safe here because the chorus PitchShifter
(Eventide mode) is in a forward path, not this feedback loop.

### Tap tempo → echo time sync

FS1 short-press taps drive `TapTempoState` (shared with DMM). In both modes
the tap interval sets the primary delay time:

- **DMM**: `time_s` drives `smooth_delay_smp` for the BBD delay
- **SDD-555**: `time_s` drives `sdd_smooth_echo_smp` for the tape echo

Cancellation works automatically — `tap.GetTimeS()` is called once at the
top of `AudioCallback` regardless of mode, and turning KNOB_2 > 3% clears
`tap.active` via the same mechanism in both modes.

Chorus rate in SDD-555 mode is fixed at 0.6 Hz (no separate user control)
since the chorus character is set by SW2 and the depth by MORPH.

### Mode switch — PitchShifter reconfiguration

The SDRAM `pitch` object is shared between DMM shimmer and SDD-555 Eventide
chorus. Only one mode runs at a time so there is no contention, but each
mode needs different transposition / fun settings. The mode-switch block
reconfigures the PitchShifter while audio is stopped:

| Mode | Transposition | Fun |
|---|---|---|
| DMM (shimmer) | +12 st (octave) | 0.3 (grain jitter — sparkle) |
| SDD-555 (Eventide) | +0.20 st (~20 cents) | 0.0 (clean detune) |

## SDD-555 NE570 compander model

The NE570 (and SA571) compress via a log-domain VCA whose translinear transfer
function produces program-dependent even-order harmonic coloration — this is
the "dirt source" that makes companded BBD units sound alive vs. clean digital.

Model in `ne570.h`:

```cpp
y = x * gain;
return tanhf(y + kK2 * y*y + kK3 * y*y*y);   // kK2 = 0.08, kK3 = 0.02
```

The polynomial is **only on the compressor**. The expander (`Ne570::Expand()`)
is kept clean so the dirt from compression survives to the output rather than
being partially cancelled by inverse polynomial expansion.

Time constants are deliberately looser than the DMM's SA571 (~10 ms attack /
~120 ms release, vs ~5 ms / ~60 ms) — the SRE-555 is "seasoning, not squash":

```cpp
static constexpr float kAttack  = 0.002083f;  // ~10 ms  at 48 kHz
static constexpr float kRelease = 0.000174f;  // ~120 ms at 48 kHz
static constexpr float kMaxGain = 1.8f;
static constexpr float kHpfC    = 0.02124f;   // shared 164 Hz sidechain HPF
```

Three `Ne570` instances live in `memory_morph.cpp`: one mono compressor
(`ne570`) before the BBD, and two matched expanders (`ne570_exp_l/r`) after
it so each stereo channel's envelope is tracked independently.

## SDD-555 chorus model (three algorithms)

`bbd_chorus.h` — one mono delay line (2400 samples = 50 ms at 48 kHz),
mono-in / stereo-out. Three algorithms share the same write side, pre-emphasis,
and per-channel de-emphasis filters; they differ in how the delay line is
read and how stereo is constructed.

**Shared elements:**

- **Pre-emphasis** (mono write side): `x + HPF(x)` → +6 dB shelf above 3 kHz
- **De-emphasis** (per channel): `0.5 * (x + LPF(x))` → −6 dB shelf above 3 kHz
- Pre/de-emphasis shelf corner: 3 kHz. `emph_c = 1 - exp(-2π · 3000 / sr)`
- **Trapezoidal LFO** (BBD + Dimension D): 20% rise / 30% hold high / 20% fall
  / 30% hold low — pitch sits still for 60% of each cycle, then ramps.

All three algorithms target canonical hardware reference settings cited from
the original service manuals:

| Unit | Reference setting | Source |
|---|---|---|
| Boss CE-1 / JC chorus | Mode=Chorus, Intensity 12:00–1:00, unity level, below clipping | Boss CE-1 service notes |
| Eventide H910 | Mix 25–35%, Pitch ±7 cents, Delay 15–25 ms, Feedback 0–10% | H910 hardware manual |
| Roland Dimension D | "Buttons 1+4" (widest preset), 100% wet hardware insert, unity level | SDD-320 owner's manual |

**LFO waveform:** all algorithms that use modulation now use a **sine LFO**.
An earlier revision used a piecewise trapezoid (rise 20% / hold 30% / fall 20%
/ hold 30%) for a "shimmer + settle" feel, but the slope discontinuities at
the rise→hold and hold→fall corners were audible as a square / sawtooth edge
each cycle. The real CE-1 and SDD-320 use a digital LFO that is integrated
by the BBD clock divider — the waveform that actually drives the delay tap
in hardware is approximately sinusoidal. Sine in our model gives the same
smooth, classic chorus motion with no audible corner artefact.

**SW2 UP — `ProcessBbd` (Roland CE-1 / SRE-555):**

BBD center ~7.5 ms (MN3002-accurate), swing ±4 ms (the "intensity 12:00–1:00"
target — chewy but not seasick). LFO rate fixed at 0.5 Hz (CE-1 internal),
sine shape. Two read taps at 180° phase offset, per-channel de-emphasis.

**SW2 MID — `ProcessEventide` (Eventide H910 Micropitch):**

No LFO modulation — the pitch shift IS the motion. 20 ms pre-delay tap feeds
the shared SDRAM PitchShifter at **+0.20 st (≈+12 cents)**. The H910 manual
reference is the ±7c dual-shifter "micro-pitch" sound, but with our single
shared shifter at +0.07 st the effect was inaudible on guitar input — bumped
to +0.20 st to register as a clearly audible micro-pitch widening while still
being subtler than a chorus. L channel = **50% wet** pitched, R channel = 50%
wet 20 ms-delayed dry; the two together stand in for the H910's classic ±7c
dual image (the -7c side is approximated by the delayed dry tap on R).
Feedback is 0 per the manual reference.

**SW2 DOWN — `ProcessDimensionD` (Roland SDD-320):**

Full-swing sine LFO at 0.3 Hz (SDD-320 internal "Buttons 1+4" — the widest
hardware preset), then cross-channel HPF subtraction with polarity inversion:

```
outL = wetL − HPF(wetR)
outR = wetR − HPF(wetL)
```

HPF cutoff 800 Hz, 1-pole. The minus sign cancels low-frequency cross-talk
and reinforces highs — wide and glassy without audible modulation.
`xfeed_c = 1 - exp(-2π · 800 / sr)`. The morph-level send governs the 100%
wet ratio; at MORPH=1 the cross-channel image is at full depth.

## SDD-555 spring reverb (Accutronics 8AB2D1A)

`spring_reverb.h` — three parallel spring lines, mono in / stereo out, ~40 KB
SRAM. The defining "boing + beat" character comes from inter-spring beating
(three slightly different lengths) and cross-coupled feedback (A→B→C→A cycle).

**Per-spring topology** (`SpringLine`):

```
in (+ xcoupling)
  → write main_buf at idx
  → main_out = main_buf[idx]
  → 3 series allpass filters (kAllpassG = 0.6)
  → feedback: main_buf[idx] += fb * decay
```

Spring sizes @ 48 kHz, picked for inter-spring beating:

| Spring | Main delay | Allpass sizes |
|---|---|---|
| A | 44 ms (2112 smp) | 89, 113, 157 |
| B | 61 ms (2928 smp) | 97, 127, 179 |
| C | 72 ms (3456 smp) | 101, 139, 197 |

**Cross-coupling**: each spring receives a sample tapped from the previous
spring's midpoint (~half the main delay back), scaled by `kXcoupling = 0.18`:

```
A_in = drive + C_midpoint * 0.18
B_in = drive + A_midpoint * 0.18
C_in = drive + B_midpoint * 0.18
```

Originally 0.25, lowered to 0.18 for stability at high decay (with the new
dispersive input chain adding latency to the composite loop, 0.25 was too
hot). Midpoints are snapshotted before any writes so the read order doesn't
matter.

**Spring-specific decay cap**: `spring.SetDecay(KNOB_4 * 0.65f)` — much lower
than `nl_verb`'s 0.85 cap because the cross-coupled 3-spring tank has a much
longer composite feedback path (4 dispersive APs + 3 main delays + 9 internal
APs all in the loop). 0.65 still feels like an "endless" spring tail at
KNOB_4=1, with zero self-oscillation at extremes.

**Input chain (dispersive)**: 5 ms pre-delay → 1-pole LPF at **7 kHz** (raised
from 5 kHz so the boing chirp has high-end to live in) → **4 cascaded Schroeder
allpass filters** at sizes 89/157/211/257 samples, g=0.7. Total chain length
714 samples ≈ 15 ms. This is what generates the descending "BOING" chirp on
transients — real spring steel has frequency-dependent propagation velocity
(high frequencies arrive first), and a cascade of strong allpass filters
approximates that group-delay-vs-frequency curve cheaply. Without this chain,
each spring is just a damped comb-filtered delay — present but not "springy".

**Output mix**: `outL = 0.5·(a+b)`, `outR = 0.5·(b+c)` — B in both channels
anchors the centre while A and C provide the inter-spring beating in the
L/R image.

**Decay** is shared across all three springs (KNOB_4 in SDD-555 mode, mapped
0–0.95). Above 0.95 the feedback loops self-oscillate due to cross-coupling.

## SDD-555 robustness — bypass quiescing and verb headroom

Two hardening passes after the third preset (Chorus Verb) landed:

**Silent bypass.** LED PWM frequency is initialised at **8 kHz** (was 1 kHz)
in `main()` so any GPIO-borne coupling into the analog input is above the
audio band and the input LPF. The bypass settled early-return inside
`AudioCallback` calls `dmm.Reset()` / `ne570.Reset()` / `chorus.Reset()` etc.
**exactly once** on bypass entry via a `bypass_state_reset` latch — previously
those Reset bursts ran every block (1 kHz of memory-store activity) and were
audible as a low-level periodic noise. The latch clears as soon as the bypass
ramp leaves zero. `bypass_ramp` is also snapped to `0.f` inside the
early-return so float rounding can't keep the early-return from firing.

**Verb runaway / shutoff.** The SDD-555 wet path used to be an unguarded
sum (`aged + verb * verb_send`) with verb decay capped at `0.95` — high MORPH
+ high KNOB_4 could push the spring tank or `nl_verb` into runaway and lock
the audio path with non-finite samples. Three changes restore musical
behaviour at extremes:

1. **Decay cap lowered to 0.85** for both `spring.SetDecay()` and
   `nl_verb.SetDecay()` (was 0.95). 0.85 stays well clear of self-oscillation
   while still feeling like an infinite tail at top settings.
2. **NaN/inf guard + soft tanh saturation** on the wet sum. Mirrors the DMM
   output stage. Pre/post gain (×0.85 / ×1.176) gives unity in the linear
   region — musical settings are sonically unchanged; only signals above
   ~−2 dBFS get a 2nd/3rd-harmonic compression rather than digital clip.
3. **Emergency Reset latch** — if `|verb_l|` or `|verb_r|` ever exceeds 4.0
   per sample, `spring.Reset()` / `nl_verb.Reset()` is called and the verb
   output is zeroed. With the 0.85 cap this should never fire under normal
   playing; it's a final fallback against external NaN injection.

## SDD-555 NlVerb — AMS gated + Wildcard Resonator

`nl_verb.h` — one struct that hosts two algorithms because only one runs at
a time (SW3 in the SDD-555 control map). SW3 DOWN bypasses NlVerb entirely
and uses `SpringReverb` instead. Memory: ~10 KB SRAM.

### `ProcessAms` — AMS RMX16 Non-Linear gated reverb

The Phil Collins "In the Air Tonight" gated drum sound. Architecture:

```
in → peak envelope follower (1 ms attack / 50 ms release)
   → rising-edge trigger → re-arm gate_counter
   → 6 series allpass filters (sizes 89/127/181/257/353/467 samples, g=0.65)
   → tap_l = output after AP3   (mid-chain — stereo decorrelation)
   → tap_r = output after AP6   (final diffusion)
   → gate envelope (1.0 while counter > 240 smp, linear fade to 0 in last 5 ms)
   → outL = tap_l * gate / outR = tap_r * gate
```

The "tail" is the diffusion itself, abruptly cut. No long feedback decay.
Default gate window is 200 ms; `SetGateMs()` can change it. Trigger threshold
`kAmsThresh = 0.05` (~−26 dB) is permissive enough to fire on single guitar
notes; on percussive material it gates per-transient.

The 5 ms linear fade at gate-close prevents the click that a hard cut would
produce — without it the gate sounds broken on busy material.

### `ProcessWildcard` — A2-harmonic resonator

Five parallel comb filters tuned to harmonics of A2 (110 Hz). Delay sizes
are `round(48000 / (110 × n))` for n = 1..5:

| Comb | Period (smp) | Frequency |
|---|---|---|
| 1 | 436 | 110 Hz (A2 fundamental) |
| 2 | 218 | 220 Hz (A3) |
| 3 | 145 | 330 Hz (E4) |
| 4 | 109 | 440 Hz (A4) |
| 5 |  87 | 552 Hz (~C#5) |

At high `decay` (KNOB_4) the combs ring at their tuned frequencies, producing
a droning reverb that emphasises notes in A minor / C major. Below ~0.6 it
behaves as a more conventional comb-bank reverb.

Stereo split via harmonic class: odd harmonics (110/330/550 Hz) sum into L
at 1/3 each, even harmonics (220/440 Hz) sum into R at 1/2 each. The asymmetric
split intentionally — gives a noticeably different timbre per channel.

## SDD-555 drive levels (SW1)

**Linear gain only — no preamp clip.** The real SRE-555 input was a clean
JRC4558 op-amp buffer with ~24× headroom at guitar levels; it didn't clip.
All program-dependent coloration comes from `Ne570::Compress()` — its
log-domain VCA polynomial (`y + 0.08·y² + 0.02·y³`) and the compressor's
RMS-detector pumping. Higher drive pushes the NE570 harder so the polynomial
and pumping become more audible, but the input itself stays clean.

`memory_morph.cpp` uses `sig = sig * sdd_drive` (linear), **not** `tanhf(sig * sdd_drive)`.
The `tanhf` preamp model belongs to DMM (NJM4558 character).

| SW1 | Mode | `sdd_drive` | Character |
|---|---|---|---|
| UP | Hot | 2.0× | Pushes NE570 hard — prominent polynomial dirt + compressor pumping |
| MID | Warm | 1.0× | Nominal SRE-555 operating point |
| DOWN | Clean | 0.5× | Below NE570 target — nearly transparent, dynamics intact |

## SDD-555 control map

| Control | Identifier | Function |
|---|---|---|
| Knob 1 | `KNOB_1` | MORPH: Drive → +Echo → +Chorus/Verb (DMM-style three-zone) |
| Knob 2 | `KNOB_2` | **Delay**: tape echo time 50–500 ms log (tap-synced) · **ChorusVerb**: chorus rate 0.1–3 Hz log (tap-synced) |
| Knob 3 | `KNOB_3` | **Delay**: tape echo feedback 0–0.95 · **ChorusVerb**: chorus depth 0–1 (direct, not morphed) |
| Knob 4 | `KNOB_4` | Verb decay — capped at 0.85 internally to stay below the runaway knee |
| Knob 5 | `KNOB_5` | Mechanical age — HF rolloff + breathing LFO |
| Knob 6 | `KNOB_6` | Dry/wet mix |
| Toggle 1 | `TOGGLESWITCH_1` | Input drive: UP=Hot / MID=Warm / DOWN=Clean |
| Toggle 2 | `TOGGLESWITCH_2` | Chorus type: UP=BBD / MID=Eventide / DOWN=Dimension D |
| Toggle 3 | `TOGGLESWITCH_3` | Verb: UP=AMS Non-Lin / MID=Wildcard / DOWN=Spring only |
| Footswitch 2 | `FOOTSWITCH_2` | Bypass (LED 2) |
| Footswitch 1 | `FOOTSWITCH_1` | Tap tempo (Delay → echo time, ChorusVerb → chorus rate) · Freeze (hold ≥ 1500 ms) |

## SDD-555 memory budget

Current usage (Phase 7 + authenticity pass complete): **SRAM 140 KB / 512 KB (27%)**, **FLASH 111 KB / 128 KB (85%)**.

The authenticity pass added ~+976 B FLASH and ~+56 B SRAM total: DMM
pre/de-emphasis + asymmetric BBD saturation + slower compander timing +
coupled noise floor + matched expander; SRE-555 multi-head tape echo +
multi-rate flutter + head-bump EQ + asymmetric tape record saturation.

| Object | Location | Approx size |
|--------|----------|-------------|
| `SpringReverb` (3 spring lines + 9 allpasses + pre-delay) | SRAM | ~40 KB |
| `NlVerb` (6 AMS allpasses + 5 Wildcard combs + state) | SRAM | ~10 KB |
| `BbdChorus` (one mono delay line + state) | SRAM | ~9.7 KB |
| `Ne570` × 3 instances (compressor + L/R expanders) | SRAM | ~72 B |
| MechAge inline state (2 LPF floats + phase) | SRAM | ~12 B |
| Existing DMM objects (`DmmChain`, `PlateReverb`, etc.) | SRAM | ~79 KB |
| `pitch` PitchShifter (shared between DMM shimmer & SDD-555 Eventide) | SDRAM | ~128 KB |
| `delay_line` (shared mono delay, DMM only) | SDRAM | ~384 KB |

All build-out is complete. Phase 8 is on-hardware tuning of the constants
already in code (spring decay limit, AMS gate threshold, MORPH anchors, etc.)
— no new SRAM or FLASH expected.

## CPU budget

At 48 kHz / 480 MHz both chains run comfortably within budget. `PitchShifter`
in the DMM shimmer loop is the most expensive single element; it's re-used
by SDD-555's Eventide chorus mode in Phase 4 (the two modes never run
simultaneously). Do NOT add FFT pitch shifters, additional reverbs, or
loopers without profiling first.

## Future extensions (flagged TODOs in code)

- `PitchShifter` transposition: DMM uses +12 semitones (octave). A second
  interval (+7, perfect 5th) is a natural extension — KNOB_4 upper range could
  split into interval selection when shimmer toggle is active.
- Expression pedal input on HotHouse maps well to MORPH for real-time
  foot-controlled morphing — applies to both modes.
- The three drive levels (High/Med/Low) could expose a fourth "Fuzz" position
  via a long-press on FS1 — `preamp_gain` ≥ 10, post_gain scaled down (DMM only).
