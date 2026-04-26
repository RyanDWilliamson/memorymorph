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
//   TOGGLESWITCH_1  Saturation character
//                   UP=Tape (soft clip)  MID=Warm (fold)  DOWN=Clean
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
using daisysp::Overdrive;
using daisysp::PitchShifter;
using daisysp::ReverbSc;
using daisysp::Svf;
using daisysp::Wavefolder;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kSampleRate  = 48000;
static constexpr uint32_t kMaxDelaySmp = kSampleRate * 2;  // 2 seconds
static constexpr float    kTwoPi       = 6.28318530718f;

// ── Large DSP buffers — MUST live in 64 MB external SDRAM ─────────────────────
// Omitting DSY_SDRAM_BSS causes a hard fault at Init().

static DelayLine<float, kMaxDelaySmp> DSY_SDRAM_BSS delay_line;
static ReverbSc                       DSY_SDRAM_BSS reverb;

// PitchShifter holds two 16384-float delay lines (~128 KB).
// At 96 kHz the grain window is ~171 ms (vs ~341 ms at 48 kHz) — slightly
// brighter shimmer character but functionally correct.
// Move to SDRAM with DSY_SDRAM_BSS if internal SRAM runs short.
static PitchShifter pitch;

// ── Other DSP objects ─────────────────────────────────────────────────────────

static Hothouse    hw;
static DcBlock     dc_block;
static Overdrive   drive;       // Tape / soft-clip saturation
static Wavefolder  folder;      // Warm / wavefolding saturation
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
  float sat_drive;        // 0–1  overdrive / fold drive amount
  float mod_depth_scale;  // 0–1  scales KNOB_4 raw depth value
  float reverb_decay;     // 0.6–0.999  reverb feedback / decay time
  float reverb_lpf_hz;    // Hz  reverb high-frequency damping
};

// Tape anchor: pure saturation, no delay, no reverb
static constexpr MorphParams kAnchorTape = {
    /*delay_send=*/0.00f,
    /*reverb_send=*/0.00f,
    /*sat_drive=*/0.50f,
    /*mod_depth_scale=*/0.00f,
    /*reverb_decay=*/0.75f,
    /*reverb_lpf_hz=*/9000.f,
};

// Echo anchor: Memory Man-style delay with moderate saturation
// reverb_send=0.30 lets the reverb start fading in before noon on the knob
static constexpr MorphParams kAnchorEcho = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/0.30f,
    /*sat_drive=*/0.25f,
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
    /*sat_drive=*/0.25f,
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
      lerpf(a.sat_drive,        b.sat_drive,        t),
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

// ── Audio callback ─────────────────────────────────────────────────────────────

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size) {
  // Always call ProcessAllControls() first.
  hw.ProcessAllControls();

  // ── FS1 tap tempo detection (rising edge = new tap) ────────────────────────
  // Uses RisingEdge() which fires once on press — doesn't conflict with
  // momentary freeze (Pressed()) or the callback system.
  if (hw.switches[Hothouse::FOOTSWITCH_1].RisingEdge()) {
    static uint32_t last_tap_ms = 0;
    const uint32_t now_ms = System::GetNow();
    const uint32_t delta  = now_ms - last_tap_ms;
    last_tap_ms  = now_ms;
    tap_blink_ms = now_ms;
    // Valid tap: 50 ms – 2000 ms (30–1200 BPM)
    if (delta >= 50 && delta <= 2000) {
      tap_tempo_s = static_cast<float>(delta) * 0.001f;
      tap_active  = true;
    }
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
  static float smooth_morph  = 0.f;
  static float smooth_mix    = 0.5f;
  static float smooth_time   = 0.3f;
  static float smooth_tone   = 4000.f;
  // Block-rate glide for delay time (tap tempo slide — slow enough to
  // avoid audible doppler chirp when the read head moves)
  smooth_time  += 0.01f * (time_target - smooth_time);
  const float time_s = smooth_time;

  // Momentary freeze: hold FS1 = freeze (infinite feedback + reverb decay)
  // Ramp freeze_blend over ~10 ms (coeff 0.002 at 48 kHz / 48-sample blocks)
  // to avoid pops from instant parameter jumps.
  const float freeze_target = hw.switches[Hothouse::FOOTSWITCH_1].Pressed() ? 1.f : 0.f;
  freeze_blend += 0.002f * (freeze_target - freeze_blend);
  const bool frozen_now = freeze_blend > 0.5f;

  // Freeze: smoothly ramp feedback toward near-infinite
  const float fb = lerpf(repeats, 0.999f, freeze_blend);

  // ── Toggle positions ─────────────────────────────────────────────────────────

  const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  // ── Update DSP module parameters ─────────────────────────────────────────────

  // Saturation drive (Overdrive and Wavefolder both updated; only one is used
  // per sample depending on sw1, but updating both is cheap and avoids pops).
  drive.SetDrive(0.5f);
  folder.SetGain(2.25f);

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
  mod_lfo.SetAmp(depth * mp_lfo.mod_depth_scale);

  // Delay time in samples
  const float base_delay_smp = time_s * static_cast<float>(kSampleRate);

  // ── Per-sample DSP loop ───────────────────────────────────────────────────────

  // Per-sample smoothing coefficients (~5 ms time constant at 48 kHz)
  static constexpr float kSmooth = 0.0002f;   // morph, mix
  static constexpr float kSmoothTone = 0.0002f;
  static float smooth_verb_lpf = 8500.f;

  for (size_t i = 0; i < size; ++i) {
    const float dry = in[0][i];

    // ── Per-sample parameter smoothing (eliminates 1 kHz staircase whine) ──
    smooth_morph += kSmooth * (morph_raw - smooth_morph);
    smooth_mix   += kSmooth * (mix_raw   - smooth_mix);
    smooth_tone  += kSmoothTone * (tone_hz - smooth_tone);
    const float morph = smooth_morph;
    const float mix   = smooth_mix;

    const MorphParams mp = ComputeMorph(morph);

    // Update tone filter frequency per-sample (smooth)
    tone_filter.SetFreq(smooth_tone);

    // Reverb parameters — smoothed via morph
    const float rev_decay = lerpf(mp.reverb_decay, 0.999f, freeze_blend);
    float eff_decay = rev_decay;
    if (short_plate)
      eff_decay = frozen_now ? 0.999f : fminf(rev_decay, 0.80f);
    const float shimmer_amt = shimmer_on ? 0.25f * mp.reverb_send : 0.f;
    if (shimmer_amt > 0.001f && !frozen_now)
      eff_decay = fminf(eff_decay, 0.93f);
    reverb.SetFeedback(eff_decay);
    smooth_verb_lpf += kSmooth * (mp.reverb_lpf_hz - smooth_verb_lpf);
    reverb.SetLpFreq(smooth_verb_lpf);

    if (bypass) {
      // Drain DSP buffers by feeding silence so they don't build up energy.
      // This prevents drone when re-engaging the effect.
      delay_line.SetDelay(base_delay_smp);
      delay_line.Read();
      delay_line.Write(0.f);
      float vL, vR;
      reverb.Process(0.f, 0.f, &vL, &vR);
      shimmer_buf = 0.f;
      out[0][i] = dry;
      out[1][i] = dry;
      continue;
    }

    // DC block (removes low-frequency offset from guitar pickups)
    float sig = dc_block.Process(dry);

    // ── Saturation stage (unity gain) ──────────────────────────────────────────
    // Overdrive/fold can add gain; tanhf after the blend keeps the peak at
    // ±1 so downstream reverb+shimmer see a consistent input level regardless
    // of saturation mode or morph position.
    float sat;
    if      (sw1 == Hothouse::TOGGLESWITCH_UP)     sat = tanhf(lerpf(sig, drive.Process(sig), mp.sat_drive));
    else if (sw1 == Hothouse::TOGGLESWITCH_MIDDLE)  sat = tanhf(lerpf(sig, folder.Process(sig), mp.sat_drive));
    else                                             sat = sig;

    // ── Tone filter ────────────────────────────────────────────────────────────
    tone_filter.Process(sat);
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
    const float delay_out = delay_line.Read();
    // During freeze, fade new input to zero so delay just recirculates.
    // freeze_blend ramps smoothly 0→1 to avoid pops.
    const float delay_input = tape_out * (1.f - freeze_blend);
    delay_line.Write(delay_input + delay_out * fb);

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

    // Reverb input: crossfade tape→delay (not sum!) so level stays ~unity.
    // shimmer_buf feeds back at shimmer_amt (max 0.25).
    const float verb_src = lerpf(tape_out, delay_out, mp.delay_send)
                         * (1.f - freeze_blend);
    float pre_verb_l = verb_src + shimmer_buf * shimmer_amt;

    // Mono-in, stereo-out reverb
    float verbL, verbR;
    reverb.Process(pre_verb_l, pre_verb_l, &verbL, &verbR);

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
    const float dry_path = lerpf(tape_out, delay_out, mp.delay_send);
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

  drive.Init();
  drive.SetDrive(0.5f);

  folder.Init();
  folder.SetGain(1.f);
  folder.SetOffset(0.f);

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
      } else if (hw.switches[Hothouse::FOOTSWITCH_1].Pressed()) {
        led_lfo_phase += 0.4f * 0.001f;  // 0.4 Hz at 1 kHz tick
        if (led_lfo_phase >= 1.f) led_lfo_phase -= 1.f;
        led1_brightness = 0.5f + 0.5f * sinf(led_lfo_phase * kTwoPi);
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
