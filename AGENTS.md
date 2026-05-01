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
src/hothouse.h / hothouse.cpp    — HotHouse board support (do NOT modify)
src/MemoryMorph/memory_morph.cpp — all DSP and control logic (~700 lines)
DaisySP/                          — git submodule (do NOT modify)
libDaisy/                         — git submodule (do NOT modify)
```

## Non-negotiable constraints

### 1. SDRAM placement for large buffers
```cpp
// CORRECT
static DelayLine<float, 192000> DSY_SDRAM_BSS delay_line;
static ReverbSc                 DSY_SDRAM_BSS reverb;

// WRONG — hard fault at init
static DelayLine<float, 192000> delay_line;
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

### 6. No tanhf inside any feedback loop
`tanhf` in a feedback path (reverb shimmer loop, delay feedback) progressively
flattens waveforms → `PitchShifter` grain crossfades cancel → shimmer cuts out.
`tanhf` is only permitted at the **final output mix** and inside `DmmCompress`
(which is not in a feedback path).

## DMM signal chain (current implementation)

Per-sample path inside `AudioCallback`:

```
guitar in
  → dc_block
  → tanhf(sig × preamp_gain)        — op-amp preamp clip
  → DmmCompress() × post_gain        — SA571 RMS 2:1 compressor
  → BiquadLP(8 kHz Butterworth)      — anti-alias before BBD
  → tone_filter (user Tone knob)
  → bbd_lpf_z (bandwidth narrows with longer delay time)
  → delay_line.Write( + fb_lpf_z×fb) — fb_lpf_z: 5 kHz feedback warmth LPF
  → delay_line.Read()
  → BiquadLP(same 8 kHz coeff)       — anti-image reconstruction
  → DmmExpand()                       — SA571 complementary expander
  → reverb / shimmer mix
  → tanhf() on final output only
```

## DMM drive levels (SW1)

All three positions use the **same compander model** — only gain differs.
Guitar volume directly controls saturation depth within each mode.

| SW1 | Mode | `preamp_gain` | `post_gain` | Character |
|---|---|---|---|---|
| UP | High | 5.0× | 0.60× | Heavy compressor pumping, rich harmonics |
| MID | Med | 2.5× | 0.90× | Nominal DMM operating point |
| DOWN | Low | 1.2× | 1.15× | Gentle, most transparent |

## SA571 compander time constants (48 kHz)

**Critical**: these must be computed as `1 - exp(-1/(τ × sr))`, NOT as small
ad-hoc decimals. Wrong values cause the compressor to lock at max gain,
producing hard clipping → square-wave harmonics → audible 1–3 kHz drone.

```cpp
// Attack  ~5 ms:  1 - exp(-1 / (0.005 * 48000)) = 0.004158
// Release ~60 ms: 1 - exp(-1 / (0.060 * 48000)) = 0.000347
static constexpr float kCompAttack  = 0.004158f;
static constexpr float kCompRelease = 0.000347f;
```

Expander uses the same time constants as the compressor (matched pair).

## Biquad LPF coefficients (Butterworth, 8 kHz)

Computed in `main()` via bilinear transform, stored in globals `aa_b0..aa_a2`.
The **same coefficient set** is reused for both the anti-alias and anti-image
filters (they share the same cutoff and Q). State variables are separate:
`aa_w1/aa_w2` (pre-BBD) and `ai_w1/ai_w2` (post-BBD).

```cpp
const float k    = tanf(kTwoPi * 8000.f / sr * 0.5f);
const float k2   = k * k;
const float norm = 1.f / (k2 + k / 0.7071f + 1.f);
aa_b0 = k2 * norm;  aa_b1 = 2.f * aa_b0;  aa_b2 = aa_b0;
aa_a1 = 2.f * (k2 - 1.f) * norm;
aa_a2 = (k2 - k / 0.7071f + 1.f) * norm;
```

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
| Knob 5 | `KNOB_5` | Tone LPF (log, 1200 Hz–18 kHz) |
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
