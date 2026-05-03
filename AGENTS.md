# Memory Morph — Agent Instructions

## What this is

A C++ guitar effects pedal firmware for the **Cleveland Music Co. HotHouse**
platform (Daisy Seed / STM32H750 ARM Cortex-M7, 480 MHz boost, **48 kHz**).
Implements a Chase Bliss–inspired morphable effect chain:
Electro-Harmonix Deluxe Memory Man preamp/compander → BBD delay → ambient
shimmer reverb, all swept by a single MORPH macro knob.

Current branch under active development: `dmm-deep-dive`.

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
src/MemoryMorph/memory_morph.cpp   — top-level DSP + control logic
src/MemoryMorph/morph.h            — MorphParams struct + ComputeMorph() interpolation
src/MemoryMorph/plate_reverb.h     — PlateReverb struct (Schroeder mono-in/stereo-out)
src/MemoryMorph/dmm_chain.h        — DmmChain struct (SA571 compander + BBD/biquad filters)
src/MemoryMorph/tap_tempo.h        — TapTempoState struct (FS1 tap/freeze state machine)
src/MemoryMorph/shimmer.h          — ShimmerVoice struct (HPF + PitchShifter + auto-duck)
DaisySP/                            — git submodule (do NOT modify)
libDaisy/                           — git submodule (do NOT modify)
```

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

### 6. No tanhf in the shimmer feedback loop
`tanhf` in the **shimmer/reverb feedback path** (reverb → PitchShifter → back to reverb)
progressively flattens waveforms → grain crossfades cancel → shimmer cuts out.

`tanhf` IS intentionally used in the **delay feedback path** (mild 2× overdrive on the
recycled signal only) to model BBD input op-amp clipping — this is safe because the
reverb's allpass diffusion scrambles the waveform before it reaches the PitchShifter.

Permitted locations: final output mix, `dmm.Compress()`, delay feedback write path.
Prohibited: shimmer/reverb recirculation loop.

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

## CPU budget

At 48 kHz / 480 MHz this chain runs well within budget. `PitchShifter` in the
shimmer loop is the most expensive single element. Do NOT add FFT pitch
shifters, additional reverbs, or loopers without profiling first.

## Future extensions (flagged TODOs in code)

- `PitchShifter` transposition: currently +12 semitones (octave). A second
  interval (+7, perfect 5th) is a natural extension — KNOB_4 upper range could
  split into interval selection when shimmer toggle is active.
- Expression pedal input on HotHouse maps well to MORPH for real-time
  foot-controlled morphing.
- The three drive levels (High/Med/Low) could expose a fourth "Fuzz" position
  via a long-press on FS1 — `preamp_gain` ≥ 10, post_gain scaled down.
