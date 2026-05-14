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
src/MemoryMorph/memory_morph.cpp   — top-level DSP + control logic (mode dispatcher + both per-sample loops)
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
  → dmm.BbdFilter(c)                             — one-pole bandwidth LPF (narrows with delay time)
  → fb_out = dmm.FbFilter(delay_line.Read())     — 5 kHz feedback warmth LPF
  → fb_sat = tanhf(fb_out × fb × 2) × 0.5       — feedback soft-clip (BBD input op-amp model)
  → delay_line.Write(BbdFilter(input) + fb_sat)
  → delay_line.Read() → dmm.AiFilter()           — 8 kHz Butterworth anti-image reconstruction
  → (no expander — digital has no noise floor)
  → reverb.Process()
      shimmer path: HPF(800 Hz) → PitchShifter(+12 st, fun=0.3) → × shimmer_amt → reverb input
  → tanhf(verbL) × reverb_send × 2.0            — reverb pre-clip before output mix
  → tanhf() on final output mix
```

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

```cpp
// Attack  ~5 ms:  1 - exp(-1 / (0.005 * 48000)) = 0.004158
// Release ~60 ms: 1 - exp(-1 / (0.060 * 48000)) = 0.000347
static constexpr float kCompAttack  = 0.004158f;
static constexpr float kCompRelease = 0.000347f;

// kCompMaxGain is capped at 2.0 — higher values cause audible digital whine
// when the sidechain HPF removes low-frequency content from the envelope.
static constexpr float kCompMaxGain = 2.0f;

// Sidechain HPF at ~164 Hz (one-pole, kCompHpfC = 0.02124) prevents 60 Hz hum
// from driving compressor gain upward and causing audible pumping.
static constexpr float kCompHpfC    = 0.02124f;
```

Expander uses the same time constants as the compressor (matched pair).

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

### SDD-555 MORPH lerp table

KNOB_1 in SDD-555 mode sweeps two parameters across three anchor points
(mirroring the DMM `MorphParams` pattern, but with only two fields since the
SDD-555 chain has fewer macro-controllable knobs). Tape echo runs at full
level across the entire sweep — MORPH only changes how much chorus motion
and reverb layer on top of the echo.

| MORPH | Zone | `verb_send` | `chorus_depth_scale` | Character |
|---|---|---|---|---|
| 0.0 | Echo only | 0.0 | 0.0 | Pure SRE-555 tape echo, no modulation, no verb |
| 0.5 | Echo + Chorus | 0.3 | 1.0 | The classic "Chorus Echo" sound |
| 1.0 | Echo + Chorus + Verb | 1.0 | 1.0 | Full ambient wash |

Interpolation is piecewise-linear between adjacent anchors. Helper is
`ComputeSddMorph(m)` inline in `memory_morph.cpp`.

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

### Tape echo

`memory_morph.cpp` inline — reuses the existing SDRAM `delay_line` (mono,
~96000 samples = 2 s capacity) since DMM and SDD-555 never run simultaneously.
`delay_line.Init()` is called on every mode switch to zero the previous
mode's residue (~5 ms SDRAM write, hidden under the LED blink).

```cpp
echo_repeat   = delay_line.Read()                       // single tap, mono
sdd_fb_lpf_z += sdd_fb_lpf_c · (echo_repeat − sdd_fb_lpf_z)  // 6 kHz tape rolloff
fb_sat        = tanhf(sdd_fb_lpf_z · echo_fb)            // soft-clip saturation
delay_line.Write(sig + fb_sat)                            // feedback into write head
sig           = sig + echo_repeat                         // dry+wet sum feeds chorus
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

**SW2 UP — `ProcessBbd` (Roland CE-1 / SRE-555):**

Full trapezoidal LFO depth, two read taps with 180° phase offset, per-channel
de-emphasis. This is the canonical "chorus" character — pronounced shimmer
with the LFO's flat-top "settle" replacing continuous sine warble.

**SW2 MID — `ProcessEventide` (Eventide H910 Micropitch):**

No LFO modulation. Two static short reads (~6 ms L, ~10 ms R) for stereo
decorrelation, then both channels mixed with the shared SDRAM PitchShifter's
mono output at +0.20 st (~20 cents detune). The detune IS the chorus motion.

**SW2 DOWN — `ProcessDimensionD` (Roland SDD-320):**

Half-swing trapezoidal LFO (~50% of CE-1 depth — subtler), then cross-channel
HPF subtraction with polarity inversion:

```
outL = wetL − HPF(wetR)
outR = wetR − HPF(wetL)
```

HPF cutoff 800 Hz, 1-pole. The minus sign cancels low-frequency cross-talk
and reinforces highs — "wider than stereo" without audible modulation.
`xfeed_c = 1 - exp(-2π · 800 / sr)`.

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
spring's midpoint (~half the main delay back), scaled by `kXcoupling = 0.25`:

```
A_in = drive + C_midpoint * 0.25
B_in = drive + A_midpoint * 0.25
C_in = drive + B_midpoint * 0.25
```

The 0.25 cross-coupling gain is the stability ceiling — higher values let
the A→B→C→A loop self-oscillate. Midpoints are snapshotted before any writes
so the read order doesn't matter.

**Input chain**: 5 ms pre-delay (240 samples — physical tank travel time
before first reflection) → 1-pole LPF at 5 kHz (springs can't transmit highs).

**Output mix**: `outL = 0.5·(a+b)`, `outR = 0.5·(b+c)` — B in both channels
anchors the centre while A and C provide the inter-spring beating in the
L/R image.

**Decay** is shared across all three springs (KNOB_4 in SDD-555 mode, mapped
0–0.95). Above 0.95 the feedback loops self-oscillate due to cross-coupling.

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
| Knob 1 | `KNOB_1` | MORPH: Echo → +Chorus → +Verb |
| Knob 2 | `KNOB_2` | Tape echo time 50–500 ms log — authentic SRE-555 range (tap-synced) |
| Knob 3 | `KNOB_3` | Tape echo feedback 0 – 0.95 |
| Knob 4 | `KNOB_4` | Verb decay — feeds whichever verb SW3 selects |
| Knob 5 | `KNOB_5` | Mechanical age — HF rolloff + breathing LFO |
| Knob 6 | `KNOB_6` | Dry/wet mix |
| Toggle 1 | `TOGGLESWITCH_1` | Input drive: UP=Hot / MID=Warm / DOWN=Clean |
| Toggle 2 | `TOGGLESWITCH_2` | Chorus type: UP=BBD / MID=Eventide / DOWN=Dimension D |
| Toggle 3 | `TOGGLESWITCH_3` | Verb: UP=AMS Non-Lin / MID=Wildcard / DOWN=Spring only |
| Footswitch 2 | `FOOTSWITCH_2` | Bypass (LED 2) |
| Footswitch 1 | `FOOTSWITCH_1` | Tap tempo → echo time sync · Freeze (hold ≥ 1500 ms) |

## SDD-555 memory budget

Current usage (Phase 7 complete): **SRAM 139 KB / 512 KB (27%)**, **FLASH 109 KB / 128 KB (83%)**.

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
