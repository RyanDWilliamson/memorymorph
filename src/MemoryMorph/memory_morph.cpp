// Memory Morph
// Chase Bliss–inspired morphable tape saturation / Memory Man delay /
// ambient shimmer reverb for the Cleveland Music Co. HotHouse.
//
// Hardware: Daisy Seed (STM32H750 ARM Cortex-M7)
// Sample rate: 48 kHz  |  Boost mode: 480 MHz  |  Block size: 48
//
// ── MORPH three-zone sweep (KNOB_1) ──────────────────────────────────────────
//   0.0  Tape    — saturated tape tone, no delay, no reverb
//   0.5  Echo    — warm Memory Man-style echo with light saturation
//   1.0  Ambient — modulated shimmer reverb wash
//
// ── Controls ─────────────────────────────────────────────────────────────────
//   KNOB_1  Morph        — sweeps all parameters across three zones
//   KNOB_2  Time         — delay time 50 ms – 1600 ms (log)
//   KNOB_3  Repeats      — delay feedback 0 – 97%
//   KNOB_4  Depth        — modulation intensity (scaled by Morph)
//   KNOB_5  Tone         — LPF cutoff 800 Hz – 18 kHz (log)
//   KNOB_6  Mix          — dry/wet blend
//
//   TOGGLESWITCH_1  DMM input drive level (all modes use the same circuit model)
//                   UP=High drive  MID=Med drive  DOWN=Low drive
//   TOGGLESWITCH_2  Modulation type
//                   UP=Chorus  MID=Vibrato  DOWN=Wow/Flutter
//   TOGGLESWITCH_3  Reverb tail
//                   UP=Short plate  MID=Long plate  DOWN=Shimmer (+1 octave)
//
//   FOOTSWITCH_2  Bypass toggle    (LED_2 on = active)
//   FOOTSWITCH_1  Tap tempo (single press, LED_1 blinks on tap)
//                 Freeze (momentary — hold to freeze)
//                 DFU bootloader (hold 10 s + all toggles DOWN + mix at 0)
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>

#include "daisysp.h"
#include "hothouse.h"

using clevelandmusicco::Hothouse;
using daisy::AudioHandle;
using daisy::Led;
using daisy::Parameter;
using daisy::SaiHandle;
using daisy::System;
using daisysp::DcBlock;
using daisysp::DelayLine;
using daisysp::Oscillator;
using daisysp::PitchShifter;
using daisysp::ReverbSc;
using daisysp::Svf;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kSampleRate  = 48000;
static constexpr uint32_t kMaxDelaySmp = kSampleRate * 2;  // 2 seconds
static constexpr float    kTwoPi       = 6.28318530718f;

// ── Large DSP buffers — MUST live in 64 MB external SDRAM ─────────────────────
// Omitting DSY_SDRAM_BSS causes a hard fault at Init().

static DelayLine<float, kMaxDelaySmp> DSY_SDRAM_BSS delay_line;

// ReverbSc holds aux_[98936] (~387 KB) with 8 delay taps at random offsets.
// Random-access cache-miss pattern: placing this in AXI SRAM (RAM_D1, 512 KB)
// costs only ~3–5 AXI cycles per miss vs ~30–60 CPU cycles for SDRAM.
// Total BSS without DSY_SDRAM_BSS: ~279 KB — well within the 512 KB RAM_D1.
static ReverbSc reverb;

// PitchShifter holds two DelayLine<float,16384> (~128 KB total).
// Its grain algorithm sweeps sequentially, so SDRAM burst-read latency is
// amortised — far more cache-friendly in SDRAM than the reverb's scatter reads.
// Only active when shimmer is on, so SDRAM pressure is limited to Ambient zone.
static PitchShifter DSY_SDRAM_BSS pitch;

// ── Other DSP objects ─────────────────────────────────────────────────────────

static Hothouse    hw;
static DcBlock     dc_block;
static Svf         tone_filter; // Post-saturation LPF/HPF (KNOB_5)
static Oscillator  mod_lfo;     // Delay-time modulation LFO

// ── Parameters ────────────────────────────────────────────────────────────────

static Parameter p_morph;    // KNOB_1 — 0.0 – 1.0
static Parameter p_time;     // KNOB_2 — 0.05 – 1.6 s (log)
static Parameter p_repeats;  // KNOB_3 — 0.0 – 0.97
static Parameter p_depth;    // KNOB_4 — 0.0 – 1.0
static Parameter p_tone;     // KNOB_5 — 800 – 18000 Hz (log)
static Parameter p_mix;      // KNOB_6 — 0.0 – 1.0

// ── LEDs ──────────────────────────────────────────────────────────────────────

static Led led_bypass;  // LED_2: solid on = active
static Led led_freeze;  // LED_1: pulsing = freeze or mod active

// ── Global state ──────────────────────────────────────────────────────────────

static volatile bool bypass = false;  // start active — LED_2 lights on boot

// Shimmer feedback sample (read and written each audio sample)
static float shimmer_buf = 0.f;

// ── Custom tape saturator state ───────────────────────────────────────────────


// ── BBD delay bandwidth state ─────────────────────────────────────────────────
// One-pole LPF on the delay write path; cutoff narrows with longer delay time,
// matching real bucket-brigade (MN3005) bandwidth characteristics.
static float bbd_lpf_z = 0.f;

// ── SA571 compander state ─────────────────────────────────────────────────────
// Squared-signal RMS envelope detector for the compressor (before BBD).
// Attack ~5 ms / release ~60 ms — matches real SA571 time constants.
static float comp_env_sq = 0.f;

// ── Anti-alias / anti-image 2-pole Butterworth LPF ───────────────────────────
// Before BBD (aa): prevents aliasing. After BBD (ai): reconstruction filter.
// Direct-form II transposed biquad. Coefficients computed in main() at 8 kHz.
static float aa_w1 = 0.f, aa_w2 = 0.f;
static float ai_w1 = 0.f, ai_w2 = 0.f;
static float aa_b0, aa_b1, aa_b2, aa_a1, aa_a2;  // shared by both filters

// ── Feedback path LPF ─────────────────────────────────────────────────────────
// One-pole at ~5 kHz in the delay feedback path — darkens each successive repeat,
// one of the defining characters of the real DMM at long echo times.
static float fb_lpf_z = 0.f;
static float fb_lpf_c = 0.f;  // coefficient computed in main()

// Envelope follower for shimmer auto-ducking — tracks reverb output level
// and gently reduces shimmer feedback when the loop gets hot. Creates a
// musical "breathing" effect instead of hard cutout.
static float shimmer_env = 0.f;

// Smoothed delay time to avoid zipper artifacts when turning the knob
static float smooth_delay_smp = 2400.f;  // ~50 ms default

// Smoothed freeze blend (0=normal, 1=frozen) — ramps over ~10 ms to avoid pops
static float freeze_blend = 0.f;

// Tap tempo state
static volatile float    tap_tempo_s  = 0.f;     // computed tap interval in seconds
static volatile bool     tap_active   = false;   // true = use tap tempo instead of knob
static volatile uint32_t tap_blink_ms = 0;       // LED blink timestamp

// FS1 disambiguation: short press = tap tempo; hold ≥ 1500 ms = momentary freeze.
// 1500 ms threshold allows tapping down to 40 BPM without triggering freeze.
static volatile bool     fs1_is_freeze   = false;  // true while held in freeze
static volatile uint32_t fs1_press_start = 0;      // timestamp of most recent press
static volatile bool     fs1_was_pressed = false;  // previous Pressed() state
static volatile uint32_t fs1_last_tap_ms = 0;      // press timestamp of previous tap

// ── MORPH parameter interpolation ────────────────────────────────────────────
//
// Three anchor structs define the sonic character at Morph = 0, 0.5, and 1.
// Every audio block the current Morph position is used to linearly interpolate
// between the two nearest anchors, then the interpolated values drive each DSP
// module. This is the Chase Bliss approach: one macro knob, simultaneous ramp
// of all underlying parameters.

struct MorphParams {
  float delay_send;       // 0–1  how much delay is added to the wet signal
  float reverb_send;      // 0–1  how much of the delay+sat goes into reverb
  float mod_depth_scale;  // 0–1  scales KNOB_4 raw depth value
  float reverb_decay;     // 0.6–0.999  reverb feedback / decay time
  float reverb_lpf_hz;    // Hz  reverb high-frequency damping
};

// Tape anchor: pure saturation, no delay, no reverb
static constexpr MorphParams kAnchorTape = {
    /*delay_send=*/0.00f,
    /*reverb_send=*/0.00f,
    /*mod_depth_scale=*/0.00f,
    /*reverb_decay=*/0.75f,
    /*reverb_lpf_hz=*/9000.f,
};

// Echo anchor: Memory Man-style delay with moderate saturation
// reverb_send=0.30 lets the reverb start fading in before noon on the knob
static constexpr MorphParams kAnchorEcho = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/0.30f,
    /*mod_depth_scale=*/0.50f,
    /*reverb_decay=*/0.78f,
    /*reverb_lpf_hz=*/8500.f,
};

// Ambient anchor: full reverb wash with shimmer and deep modulation
// Higher decay gives shimmer enough tail to sustain; shimmer_amt is kept low
// so loop gain stays well below unity (0.25 × 0.95 = 0.24).
static constexpr MorphParams kAnchorAmbient = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/1.00f,
    /*mod_depth_scale=*/1.00f,
    /*reverb_decay=*/0.95f,
    /*reverb_lpf_hz=*/4000.f,
};

static inline float lerpf(float a, float b, float t) {
  return a + t * (b - a);
}

static MorphParams LerpParams(const MorphParams& a, const MorphParams& b,
                               float t) {
  return {
      lerpf(a.delay_send,       b.delay_send,       t),
      lerpf(a.reverb_send,      b.reverb_send,      t),
      lerpf(a.mod_depth_scale,  b.mod_depth_scale,  t),
      lerpf(a.reverb_decay,     b.reverb_decay,     t),
      lerpf(a.reverb_lpf_hz,    b.reverb_lpf_hz,    t),
  };
}

static MorphParams ComputeMorph(float m) {
  if (m <= 0.5f)
    return LerpParams(kAnchorTape, kAnchorEcho, m * 2.f);
  else
    return LerpParams(kAnchorEcho, kAnchorAmbient, (m - 0.5f) * 2.f);
}

// ── Footswitch callbacks ───────────────────────────────────────────────────────

static void OnNormalPress(Hothouse::Switches fsw) {
  if (fsw == Hothouse::FOOTSWITCH_2) bypass = !bypass;
}

static void OnDoublePress(Hothouse::Switches /*fsw*/) {
  // Reserved for future use
}

static void OnLongPress(Hothouse::Switches /*fsw*/) {
  // DFU is handled by 10 s hold detection in the main loop instead
}

// ── DMM DSP helpers ───────────────────────────────────────────────────────────

// 2-pole Butterworth LPF — direct-form II transposed biquad.
// w1/w2 are the two state variables (persist across calls).
// Coefficients (b0,b1,b2,a1,a2) are computed once in main() from sample rate.
static inline float BiquadLP(float x, float& w1, float& w2,
                              float b0, float b1, float b2,
                              float a1, float a2) {
  const float y = b0 * x + w1;
  w1 = b1 * x - a1 * y + w2;
  w2 = b2 * x - a2 * y;
  return y;
}

// SA571 compressor — RMS-based 2:1 gain reduction before the BBD.
// Pre-gain scales input to the desired drive level; the compressor then
// prevents the BBD from seeing signals larger than ~±1.
// kTarget: RMS level the compressor aims to hold at its output.
// Returns the compressed sample; updates comp_env_sq in-place.
static constexpr float kCompTarget    = 0.25f;    // ~–12 dBFS RMS target
static constexpr float kCompAttack    = 0.004158f; // ~5 ms at 48 kHz  [1-exp(-1/(0.005*48000))]
static constexpr float kCompRelease   = 0.000347f; // ~60 ms at 48 kHz [1-exp(-1/(0.060*48000))]
static constexpr float kCompMaxGain   = 2.5f;      // punchy but not harsh at hot input
static constexpr float kCompMinGain   = 0.1f;

static inline float DmmCompress(float x) {
  const float x2 = x * x;
  if (x2 > comp_env_sq)
    comp_env_sq += kCompAttack   * (x2 - comp_env_sq);
  else
    comp_env_sq += kCompRelease  * (x2 - comp_env_sq);
  const float rms  = sqrtf(comp_env_sq + 1e-12f);
  const float gain = fmaxf(kCompMinGain, fminf(kCompMaxGain, kCompTarget / rms));
  return tanhf(x * gain);  // tanhf here is safe — NOT in a feedback path
}

// ── Audio callback ─────────────────────────────────────────────────────────────

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size) {
  // FPSCR is part of the FPU exception frame on Cortex-M7 and is reset to 0
  // on every ISR entry — the FZ bit set in main() does NOT carry over here.
  // Must be set at the top of the callback so every block has flush-to-zero.
  __set_FPSCR(__get_FPSCR() | (1u << 24));
  // Always call ProcessAllControls() first.
  hw.ProcessAllControls();

  // ── FS1: short press = tap tempo; hold ≥ 1500 ms = momentary freeze ─────────
  // Decision is made on RELEASE — so any musical tempo tap (even slow, 40 BPM)
  // is unambiguous. Interval is measured press-to-press so you tap in time.
  {
    const bool     fs1_now = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();
    const uint32_t now_ms  = System::GetNow();

    if (fs1_now && !fs1_was_pressed)
      fs1_press_start = now_ms;  // rising edge: record when press started

    // Transition to freeze once held long enough (but not if still tapping)
    if (fs1_now && !fs1_is_freeze && (now_ms - fs1_press_start) >= 1500)
      fs1_is_freeze = true;

    if (!fs1_now && fs1_was_pressed) {
      // Falling edge: classify the press
      if (fs1_is_freeze) {
        fs1_is_freeze = false;  // release freeze on lift
      } else {
        // Short press → tap tempo (press-to-press interval).
        // Minimum 80 ms (750 BPM) allows slapback taps.
        // Only update fs1_last_tap_ms on a VALID tap so rapid re-tapping
        // after a slow tempo converges correctly (doesn't reset reference
        // against rejected taps and lock you out of fast tempos).
        const uint32_t interval = fs1_press_start - fs1_last_tap_ms;
        tap_blink_ms = now_ms;
        if (interval >= 80 && interval <= 1500) {
          tap_tempo_s     = static_cast<float>(interval) * 0.001f;
          tap_active      = true;
          fs1_last_tap_ms = fs1_press_start;  // only advance on valid tap
        } else if (interval > 1500) {
          // First tap after a long pause — reset reference without setting tempo
          fs1_last_tap_ms = fs1_press_start;
        }
        // If interval < 80 ms: too fast, ignore AND keep old reference so
        // the next tap measures from the last valid press, not this one.
      }
    }

    fs1_was_pressed = fs1_now;
  }

  // ── Read all parameters once per block ──────────────────────────────────────

  const float morph_raw  = p_morph.Process();
  const float knob_time  = p_time.Process();      // seconds
  const float repeats    = p_repeats.Process();   // 0 – 0.97

  // Tap tempo: use tapped interval if active; turning the knob overrides
  static float prev_knob_time = 0.f;
  if (tap_active && fabsf(knob_time - prev_knob_time) > 0.03f)
    tap_active = false;  // knob moved — return to knob control
  prev_knob_time = knob_time;
  const float time_target = tap_active ? fminf(tap_tempo_s, 2.f) : knob_time;
  const float depth   = p_depth.Process();     // 0 – 1
  const float tone_hz = p_tone.Process();      // Hz
  const float mix_raw = p_mix.Process();       // 0 – 1

  // Per-sample smoothing targets — actual smoothing happens inside the DSP loop
  // to avoid the 1 kHz staircase whine that block-rate smoothing produces.
  static float smooth_morph   = 0.f;
  static float smooth_mix     = 0.5f;
  static float smooth_time    = 0.3f;
  static float smooth_tone    = 4000.f;
  // smooth_repeats: ADC output on KNOB_3 has 1–2 LSB of jitter even when still.
  // That jitter ± one step per block AM-modulates the delay feedback at 1 kHz,
  // producing a quiet but audible standing tone at hot input levels with any
  // feedback. Per-sample smoothing eliminates the jitter; lag is imperceptible.
  static float smooth_repeats = 0.f;

  // Block-rate glide for delay time (tap tempo slide — slow enough to
  // avoid audible doppler chirp when the read head moves)
  smooth_time  += 0.01f * (time_target - smooth_time);
  const float time_s = smooth_time;

  // Block-rate glide for LFO amplitude — without this, turning KNOB_4
  // causes LFO amplitude to step at 1 kHz, producing audible clicks.
  static float smooth_depth = 0.f;
  smooth_depth += 0.02f * (depth - smooth_depth);

  // Freeze: ramp smoothly over ~10 ms to avoid pops on engage/release.
  const float freeze_target = fs1_is_freeze ? 1.f : 0.f;
  freeze_blend += 0.002f * (freeze_target - freeze_blend);
  const bool frozen_now = freeze_blend > 0.5f;

  // NOTE: fb is computed per-sample inside the loop using smooth_repeats.

  // ── Toggle positions ─────────────────────────────────────────────────────────

  const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  // ── Bypass: block-level early return ─────────────────────────────────────────
  // Do NOT call reverb.Process() or delay_line operations during bypass.
  // ReverbSc::NextRandomLineseg() fires periodically (slowest: ~0.891 Hz,
  // period ~1.12 s) and jumps its read pointer, causing a burst of SDRAM
  // cache misses that overrun the audio DMA deadline — audible as a pulse
  // of noise even with dry signal passing through. Early return skips all
  // DSP and all SDRAM access for the entire block.
  // Filter states are zeroed here so the chain starts clean on re-engage.
  if (bypass) {
    bbd_lpf_z   = 0.f;
    comp_env_sq = 0.f;
    aa_w1 = aa_w2 = ai_w1 = ai_w2 = fb_lpf_z = 0.f;
    shimmer_buf = 0.f;
    shimmer_env = 0.f;
    for (size_t i = 0; i < size; ++i) {
      out[0][i] = in[0][i];
      out[1][i] = in[0][i];
    }
    return;
  }

  // ── Update DSP module parameters ─────────────────────────────────────────────

  // DMM drive level from SW1.
  // preamp_gain: scales the guitar signal before the compressor.
  // post_gain: compensates output level so all three positions match at
  //   nominal guitar volume (~0.2–0.3 peak). Guitar volume controls saturation
  //   depth within each mode — turn up for more harmonic content.
  float preamp_gain, post_gain;
  if (sw1 == Hothouse::TOGGLESWITCH_UP) {
    // High: overdriven preamp, strong compressor pumping, rich harmonics
    preamp_gain = 5.0f;  post_gain = 0.60f;
  } else if (sw1 == Hothouse::TOGGLESWITCH_MIDDLE) {
    // Med: nominal DMM operating point — punchy, slightly warm
    preamp_gain = 2.5f;  post_gain = 0.90f;
  } else {
    // Low: gentle compression, most transparent; guitar vol drives saturation
    preamp_gain = 1.2f;  post_gain = 1.15f;
  }

  // Tone filter res is constant
  tone_filter.SetRes(0.f);

  // Shimmer and decay targets — applied per-sample with smoothed morph
  const bool shimmer_on = (sw3 == Hothouse::TOGGLESWITCH_DOWN);
  const bool short_plate = (sw3 == Hothouse::TOGGLESWITCH_UP);

  // LFO rate depends on modulation type (sw2) and depth knob
  float lfo_hz;
  if      (sw2 == Hothouse::TOGGLESWITCH_UP)     lfo_hz = 0.5f + depth * 4.5f;  // Chorus:     0.5–5 Hz
  else if (sw2 == Hothouse::TOGGLESWITCH_MIDDLE)  lfo_hz = 1.5f + depth * 6.5f;  // Vibrato:    1.5–8 Hz
  else                                             lfo_hz = 0.1f + depth * 1.4f;  // Wow/Flutter: 0.1–1.5 Hz

  mod_lfo.SetFreq(lfo_hz);
  // Depth is scaled by MORPH so at Tape anchor (morph=0) there is no modulation
  // Use current smooth_morph for block-rate LFO amplitude (doesn't need per-sample)
  const MorphParams mp_lfo = ComputeMorph(smooth_morph);
  mod_lfo.SetAmp(smooth_depth * mp_lfo.mod_depth_scale);

  // Delay time in samples
  const float base_delay_smp = time_s * static_cast<float>(kSampleRate);

  // BBD bandwidth: cutoff narrows with longer delay time, matching real
  // bucket-brigade (MN3005) chip behaviour — 9 kHz at 50 ms, 4 kHz at 1 s.
  // expf is computed block-rate (once per 48 samples) then applied per-sample.
  const float bbd_cutoff = fminf(9000.f, fmaxf(2000.f, 4000.f / time_s));
  const float bbd_c      = 1.f - expf(-kTwoPi * bbd_cutoff / static_cast<float>(kSampleRate));

  // ── Per-sample DSP loop ───────────────────────────────────────────────────────

  // Per-sample smoothing coefficient (~5 ms time constant at 48 kHz)
  static constexpr float kSmooth = 0.0002f;  // morph, mix, repeats
  static float smooth_verb_lpf = 8500.f;

  // ── Block-rate reverb + tone config ────────────────────────────────────────
  // reverb.SetFeedback() and SetLpFreq() each recompute an expf() coefficient.
  // smooth_morph changes < 0.01 per block — computing these once saves 47 expf()
  // calls per block with no perceptible difference.
  {
    const MorphParams mp_b = ComputeMorph(smooth_morph);
    const float rev_decay_b = lerpf(mp_b.reverb_decay, 0.999f, freeze_blend);
    float eff_decay_b = rev_decay_b;
    if (short_plate)
      eff_decay_b = frozen_now ? 0.999f : fminf(rev_decay_b, 0.80f);
    const float shimmer_amt_b = shimmer_on ? 0.25f * mp_b.reverb_send : 0.f;
    if (shimmer_amt_b > 0.001f && !frozen_now)
      eff_decay_b = fminf(eff_decay_b, 0.93f);
    reverb.SetFeedback(eff_decay_b);
    // Gate SetLpFreq: ReverbSc::Process() recomputes cosf()+sqrtf() every time
    // lpfreq changes. Only push a new value when the glide has moved > 0.5 Hz.
    const float new_verb_lpf = smooth_verb_lpf + 0.01f * (mp_b.reverb_lpf_hz - smooth_verb_lpf);
    if (fabsf(new_verb_lpf - smooth_verb_lpf) > 0.5f) {
      reverb.SetLpFreq(new_verb_lpf);
    }
    smooth_verb_lpf = new_verb_lpf;
  }

  // tone_filter.SetFreq() recomputes SVF coefficients. Advance smooth_tone and
  // call SetFreq once per block instead of 48× — imperceptible difference.
  smooth_tone += 0.01f * (tone_hz - smooth_tone);
  tone_filter.SetFreq(smooth_tone);

  for (size_t i = 0; i < size; ++i) {
    const float dry = in[0][i];

    // ── Per-sample parameter smoothing (eliminates 1 kHz staircase whine) ──
    smooth_morph   += kSmooth * (morph_raw - smooth_morph);
    smooth_mix     += kSmooth * (mix_raw   - smooth_mix);
    smooth_repeats += kSmooth * (repeats   - smooth_repeats);
    const float morph = smooth_morph;
    const float mix   = smooth_mix;

    // fb per-sample: eliminates ADC jitter on KNOB_3 creating a 1 kHz AM tone.
    const float fb = lerpf(smooth_repeats, 0.999f, freeze_blend);

    const MorphParams mp = ComputeMorph(morph);

    // shimmer_amt per-sample from morph (used for mix scaling below).
    // Reverb API config (SetFeedback / SetLpFreq) is done once per block above.
    const float shimmer_amt = shimmer_on ? 0.25f * mp.reverb_send : 0.f;

    // DC block (removes low-frequency offset from guitar pickups)
    float sig = dc_block.Process(dry);

    // ── DMM signal chain ──────────────────────────────────────────────────────
    //
    // 1. Preamp: scale to drive level, then tanhf for op-amp soft clip.
    //    tanhf here is the saturation character — NOT in a feedback path,
    //    so no waveform-flattening accumulation risk.
    const float preamp_out = tanhf(sig * preamp_gain);

    // 2. SA571 compressor: RMS 2:1 gain reduction before the BBD.
    //    Attack ~5 ms lets transients punch through (the DMM "snap").
    //    Release ~60 ms causes the characteristic sag on sustain notes.
    const float comp_out = DmmCompress(preamp_out) * post_gain;

    // 3. Anti-alias LPF (2-pole Butterworth, 8 kHz) — before BBD write.
    //    Removes content the BBD can't reproduce; adds the slight HF rolloff
    //    present on all DMM dry tones even without delay.
    const float aa_out = BiquadLP(comp_out, aa_w1, aa_w2,
                                  aa_b0, aa_b1, aa_b2, aa_a1, aa_a2);

    // ── Tone filter ────────────────────────────────────────────────────────────
    tone_filter.Process(aa_out);
    const float tape_out = tone_filter.Low();  // Low-pass output

    // ── Delay with LFO modulation ──────────────────────────────────────────────
    // LFO modulates delay time by ±1.5% (wow/flutter range).
    // Chorus/Vibrato modes use the same LFO but at higher rates.
    const float lfo_val    = mod_lfo.Process();
    float       target_smp = base_delay_smp + lfo_val * base_delay_smp * 0.015f;
    target_smp = fmaxf(target_smp, 48.f);
    target_smp = fminf(target_smp, static_cast<float>(kMaxDelaySmp - 1));

    // One-pole smoothing — ~50 ms ramp eliminates zipper / chirp artifacts
    smooth_delay_smp += 0.0004f * (target_smp - smooth_delay_smp);

    delay_line.SetDelay(smooth_delay_smp);
    const float delay_out_raw = delay_line.Read();

    // 4. Feedback path LPF: warms each successive repeat — one-pole at ~5 kHz.
    //    Feeds back through the BBD-bandwidth LPF too, so long echoes get
    //    progressively darker and thicker, exactly like the real DMM.
    fb_lpf_z += fb_lpf_c * (delay_out_raw - fb_lpf_z);
    const float delay_out = delay_out_raw;  // read head output (unfiltered for mix)

    // During freeze, fade new input to zero so delay just recirculates.
    const float delay_input = tape_out * (1.f - freeze_blend);

    // 5. BBD bandwidth LPF on the write path (block-rate bbd_c coefficient).
    //    At short times: near-transparent (~9 kHz).
    //    At long times: dark (~2 kHz) — matches real MN3005 characteristics.
    bbd_lpf_z += bbd_c * (delay_input - bbd_lpf_z);
    delay_line.Write(bbd_lpf_z + fb_lpf_z * fb);

    // 6. Anti-image LPF after BBD (same 8 kHz Butterworth coefficients as aa).
    //    Reconstruction filter; also rounds off any BBD clock-noise edges.
    const float ai_out = BiquadLP(delay_out, ai_w1, ai_w2,
                                  aa_b0, aa_b1, aa_b2, aa_a1, aa_a2);

    // 7. No expander — digital has no analog noise floor to suppress.
    //    The compressor's attack/release character is fully preserved.
    const float expanded = ai_out;

    // ── Reverb + shimmer feedback loop ────────────────────────────────────────
    //
    // NO tanhf inside the loop! tanhf in a feedback loop progressively
    // flattens the waveform toward a square shape. PitchShifter uses
    // sinusoidal grain crossfades — when both grains read flat-topped
    // (tanhf-saturated) waveforms, they cancel during crossfade and the
    // output drops to zero. That's the "cutout."
    //
    // ReverbSc internally scales output by ×0.35, so levels are naturally
    // modest. Linear gain scaling keeps the loop stable without changing
    // the waveshape.
    //
    if (!std::isfinite(shimmer_buf)) shimmer_buf = 0.f;

    // Reverb input: crossfade tone-filtered signal → BBD-expanded output.
    // expanded carries the full DMM delay character; tape_out is the dry tone.
    const float verb_src = lerpf(tape_out, expanded, mp.delay_send)
                         * (1.f - freeze_blend);
    float pre_verb_l = verb_src + shimmer_buf * shimmer_amt;

    // Gate reverb when its contribution to the mix would be inaudible.
    // reverb_send < 0.005 → output scaling < 0.5% → silent in the mix.
    // Skips 8-tap SDRAM scatter-gather reads every sample in Tape zone.
    // State is NOT zeroed — residual content × 0.005 × 1.8 is inaudible,
    // and the reverb drains naturally once the gate reopens.
    float verbL, verbR;
    if(mp.reverb_send > 0.005f || shimmer_amt > 0.001f)
    {
        reverb.Process(pre_verb_l, pre_verb_l, &verbL, &verbR);
    }
    else
    {
        verbL = verbR = 0.f;
    }

    // Guard against NaN (can propagate from reverb internal state)
    if (!std::isfinite(verbL)) verbL = 0.f;
    if (!std::isfinite(verbR)) verbR = 0.f;

    // Shimmer with envelope-following auto-duck.
    // Track reverb output level; when the loop gets loud, smoothly
    // reduce shimmer gain so it "breathes" instead of blowing up.
    if (shimmer_amt > 0.001f) {
      // Peak follower: fast attack (~0.3 ms), slow release (~150 ms)
      const float env_in = fabsf(verbL);
      if (env_in > shimmer_env)
        shimmer_env += 0.03f * (env_in - shimmer_env);    // attack
      else
        shimmer_env += 0.00007f * (env_in - shimmer_env);  // release

      // Start at 0.6× gain, duck toward 0.15× as envelope grows.
      // Gentle curve — keeps shimmer present but loop gain < 1.
      const float duck = 0.6f / (1.f + 4.f * shimmer_env);
      float ps_in = verbL * duck;

      shimmer_buf = pitch.Process(ps_in);
      if (!std::isfinite(shimmer_buf)) shimmer_buf = 0.f;
    } else {
      shimmer_buf = 0.f;
      shimmer_env = 0.f;
    }

    // ── Output mix ────────────────────────────────────────────────────────────
    //
    // No limiter inside the chain — levels are naturally controlled.
    // Reverb output ×0.35 internal, so verb×1.5 at the output gives
    // a full ambient wash (~0.5–0.7 peak). tanhf on the FINAL output
    // only — one pass, no feedback, so no waveform accumulation issue.
    // Dry path: crossfade tape→delay (same blend as reverb input).
    // Reverb blends in on top, scaled by reverb_send.
    // Total wet level stays consistent across the morph range.
    const float dry_path = lerpf(tape_out, expanded, mp.delay_send);
    const float dry_level = fmaxf(0.15f, 1.f - mp.reverb_send * 0.7f);
    const float wetL = dry_path * dry_level
                     + verbL   * mp.reverb_send * 1.8f;
    const float wetR = dry_path * dry_level
                     + verbR   * mp.reverb_send * 1.8f;

    // Single tanhf at the output — NOT in a feedback loop, so no
    // waveform flattening. Just smooth, warm analog-style limiting.
    out[0][i] = lerpf(dry, tanhf(wetL), mix);
    out[1][i] = lerpf(dry, tanhf(wetR), mix);
  }
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main() {
  // boost=true: 480 MHz overclock — comfortable headroom at 48 kHz.
  hw.Init(true);
  // FTZ for the main loop context (LED math etc.). The audio callback ISR
  // sets FTZ independently on each entry because FPSCR is reset per-ISR.
  __set_FPSCR(__get_FPSCR() | (1u << 24));  // FZ bit: flush denormals to zero
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
  hw.SetAudioBlockSize(48);
  const float sr = hw.AudioSampleRate();  // 48000.f

  // ── Parameters ───────────────────────────────────────────────────────────────

  p_morph.Init  (hw.knobs[Hothouse::KNOB_1],  0.f,    1.f,     Parameter::LINEAR);
  p_time.Init   (hw.knobs[Hothouse::KNOB_2],  0.05f,  2.0f,    Parameter::LOGARITHMIC);
  p_repeats.Init(hw.knobs[Hothouse::KNOB_3],  0.f,    0.97f,   Parameter::LINEAR);
  p_depth.Init  (hw.knobs[Hothouse::KNOB_4],  0.f,    1.f,     Parameter::LINEAR);
  p_tone.Init   (hw.knobs[Hothouse::KNOB_5],  1200.f, 18000.f, Parameter::LOGARITHMIC);
  p_mix.Init    (hw.knobs[Hothouse::KNOB_6],  0.f,    1.f,     Parameter::LINEAR);

  // ── DSP init ─────────────────────────────────────────────────────────────────

  dc_block.Init(sr);

  // ── DMM filter coefficients ───────────────────────────────────────────────────
  // 2-pole Butterworth LPF at 8 kHz — shared by anti-alias and anti-image filters.
  // Bilinear transform of analogue prototype: ωc = 2π·8000/sr, Q = 1/√2.
  {
    const float wc  = kTwoPi * 8000.f / sr;
    const float q   = 0.7071f;  // Butterworth Q = 1/√2
    const float k   = tanf(wc * 0.5f);
    const float k2  = k * k;
    const float norm = 1.f / (k2 + k / q + 1.f);
    aa_b0 =  k2 * norm;
    aa_b1 =  2.f * k2 * norm;
    aa_b2 =  aa_b0;
    aa_a1 =  2.f * (k2 - 1.f) * norm;
    aa_a2 =  (k2 - k / q + 1.f) * norm;
  }

  // One-pole feedback LPF at 5 kHz — darkens successive delay repeats.
  fb_lpf_c = 1.f - expf(-kTwoPi * 5000.f / sr);

  tone_filter.Init(sr);
  tone_filter.SetFreq(8000.f);
  tone_filter.SetRes(0.f);

  delay_line.Init();

  mod_lfo.Init(sr);
  mod_lfo.SetWaveform(Oscillator::WAVE_SIN);
  mod_lfo.SetFreq(1.f);
  mod_lfo.SetAmp(0.5f);

  reverb.Init(sr);
  reverb.SetFeedback(0.80f);
  reverb.SetLpFreq(8000.f);

  // Shimmer: octave up. PitchShifter takes integer semitone values.
  // TODO: consider a perfect 5th (+7) option selectable via Depth knob range
  //       when shimmer toggle is active — a natural future extension.
  pitch.Init(sr);
  pitch.SetTransposition(12.f);

  // ── LEDs ─────────────────────────────────────────────────────────────────────
  // Init with 1000 Hz update rate for smooth 8-bit PWM brightness.
  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false, 1000.f);
  led_freeze.Init(hw.seed.GetPin(Hothouse::LED_1), false, 1000.f);

  // ── Footswitch callbacks ──────────────────────────────────────────────────────
  static Hothouse::FootswitchCallbacks cbs = {
      OnNormalPress,
      OnDoublePress,
      OnLongPress,
  };
  hw.RegisterFootswitchCallbacks(&cbs);

  // ── Start audio ───────────────────────────────────────────────────────────────
  hw.StartAdc();
  hw.StartAudio(AudioCallback);

  // ── Main loop ─────────────────────────────────────────────────────────────────
  // LED updates run here at 1 kHz. All other logic is in AudioCallback.

  uint32_t last_led_ms  = 0;
  float    led_lfo_phase = 0.f;  // Normalized phase 0–1 for LED pulsing

  while (true) {
    const uint32_t now = System::GetNow();

    if (now - last_led_ms >= 1) {  // 1 ms tick → 1 kHz update
      last_led_ms = now;

      // LED_2: bypass indicator — solid on when effect is active
      led_bypass.Set(bypass ? 0.f : 1.f);
      led_bypass.Update();

      // LED_1: tap / freeze / mod indicator
      float led1_brightness;
      const bool tap_blink = (now - tap_blink_ms) < 80;

      if (tap_blink) {
        led1_brightness = 1.f;  // 80 ms flash on each tap
      } else if (bypass) {
        led1_brightness = 0.f;
        led_lfo_phase   = 0.f;
      } else if (fs1_is_freeze) {
        led1_brightness = 1.f;  // Solid on while frozen
        led_lfo_phase   = 0.f;
      } else {
        // Sync LED pulse to mod LFO: read current knob values directly
        // (safe from main loop; ADC is running and these are last-sampled values)
        const float cur_morph = hw.GetKnobValue(Hothouse::KNOB_1);
        const float cur_depth = hw.GetKnobValue(Hothouse::KNOB_4);
        const MorphParams mp  = ComputeMorph(cur_morph);
        const float vis_depth = cur_depth * mp.mod_depth_scale;

        // Approximate LFO rate: mirrors the Wow/Flutter branch as a baseline
        const float led_rate  = 0.3f + vis_depth * 2.0f;  // 0.3–2.3 Hz
        led_lfo_phase += led_rate * 0.001f;
        if (led_lfo_phase >= 1.f) led_lfo_phase -= 1.f;

        led1_brightness = vis_depth * (0.5f + 0.5f * sinf(led_lfo_phase * kTwoPi));
      }

      led_freeze.Set(led1_brightness);
      led_freeze.Update();
    }

    // DFU bootloader entry — 10 second hold on FS1 with secret combo:
    // All three toggles DOWN + mix knob at 0 (fully CCW).
    // This prevents accidental DFU entry during normal playing.
    static uint32_t fs1_hold_start = 0;
    static bool     fs1_was_held   = false;
    const bool fs1_held = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();

    if (fs1_held && !fs1_was_held)
      fs1_hold_start = now;  // rising edge — start counting

    const bool toggles_down =
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3) == Hothouse::TOGGLESWITCH_DOWN;
    const bool mix_zero = hw.GetKnobValue(Hothouse::KNOB_6) < 0.02f;

    if (fs1_held && toggles_down && mix_zero && (now - fs1_hold_start) >= 10000) {
      hw.StopAudio();
      hw.StopAdc();
      daisy::Led l1, l2;
      l1.Init(hw.seed.GetPin(Hothouse::LED_1), false);
      l2.Init(hw.seed.GetPin(Hothouse::LED_2), false);
      for (int i = 0; i < 3; i++) {
        l1.Set(1); l2.Set(0); l1.Update(); l2.Update();
        System::Delay(100);
        l1.Set(0); l2.Set(1); l1.Update(); l2.Update();
        System::Delay(100);
      }
      System::ResetToBootloader();
    }
    fs1_was_held = fs1_held;
  }
}
