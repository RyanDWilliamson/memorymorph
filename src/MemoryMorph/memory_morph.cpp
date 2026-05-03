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
//   KNOB_2  Time         — delay time 50 ms – 2000 ms (log)
//   KNOB_3  Repeats      — delay feedback 0 – 97%
//   KNOB_4  Depth        — modulation intensity (scaled by Morph)
//   KNOB_5  Tone         — LPF cutoff 4000 Hz – 18 kHz (log)
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
#include "morph.h"
#include "plate_reverb.h"
#include "dmm_chain.h"
#include "tap_tempo.h"
#include "shimmer.h"

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
using daisysp::Svf;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kSampleRate  = 48000;
static constexpr uint32_t kMaxDelaySmp = kSampleRate * 2;  // 2 seconds
static constexpr float    kTwoPi       = 6.28318530718f;

// ── Large DSP buffers — MUST live in 64 MB external SDRAM ─────────────────────
// Omitting DSY_SDRAM_BSS causes a hard fault at Init().

static DelayLine<float, kMaxDelaySmp> DSY_SDRAM_BSS delay_line;

// PitchShifter holds two DelayLine<float,16384> (~128 KB total).
// Sequential grain sweeps tolerate SDRAM burst-read latency well.
static PitchShifter DSY_SDRAM_BSS pitch;

static PlateReverb reverb;

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
static Parameter p_tone;     // KNOB_5 — 4000 – 18000 Hz (log)
static Parameter p_mix;      // KNOB_6 — 0.0 – 1.0

// ── LEDs ──────────────────────────────────────────────────────────────────────

static Led led_bypass;  // LED_2: solid on = active
static Led led_freeze;  // LED_1: pulsing = freeze or mod active

// ── Global state ──────────────────────────────────────────────────────────────

static volatile bool bypass = false;  // start active — LED_2 lights on boot

static DmmChain      dmm;
static ShimmerVoice  shimmer;
static TapTempoState tap;

// Smoothed delay time to avoid zipper artifacts when turning the knob
static float smooth_delay_smp = 2400.f;  // ~50 ms default

// Smoothed freeze blend (0=normal, 1=frozen) — ramps over ~10 ms to avoid pops
static float freeze_blend = 0.f;

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
  // FPSCR is part of the FPU exception frame on Cortex-M7 and is reset to 0
  // on every ISR entry — the FZ bit set in main() does NOT carry over here.
  // Must be set at the top of the callback so every block has flush-to-zero.
  __set_FPSCR(__get_FPSCR() | (1u << 24));
  // Always call ProcessAllControls() first.
  hw.ProcessAllControls();

  // ── FS1: short press = tap tempo; hold ≥ 1500 ms = momentary freeze ─────────
  tap.Update(hw.switches[Hothouse::FOOTSWITCH_1].Pressed(), System::GetNow());

  // ── Read all parameters once per block ──────────────────────────────────────

  const float morph_raw  = p_morph.Process();
  const float knob_time  = p_time.Process();      // seconds
  const float repeats    = p_repeats.Process();   // 0 – 0.97

  // Tap tempo: use tapped interval if active; turning the knob overrides
  const float time_target = tap.GetTimeS(knob_time);
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

  // Freeze ramp: asymmetric rates so engage feels immediate and release is clean.
  // Engage 0.015 → τ≈67 ms (snaps in within ~100 ms of the 1500 ms gate opening).
  // Release 0.03 → τ≈33 ms (back to live within ~50 ms of button lift, no ghost-loop).
  const float freeze_target = tap.IsFreeze() ? 1.f : 0.f;
  const float freeze_rate   = (freeze_target > freeze_blend) ? 0.015f : 0.03f;
  freeze_blend += freeze_rate * (freeze_target - freeze_blend);
  const bool frozen_now = freeze_blend > 0.5f;

  // NOTE: fb is computed per-sample inside the loop using smooth_repeats.

  // ── Toggle positions ─────────────────────────────────────────────────────────

  const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  // ── Soft bypass ───────────────────────────────────────────────────────────────
  // Linear ramp over ~5 ms (240 samples at 48 kHz) eliminates engage/disengage
  // pops. 0.0 = fully bypassed, 1.0 = fully active. The early return is only
  // taken once the ramp has fully settled at 0 — during the ramp the full DSP
  // chain runs and the output is crossfaded toward dry.
  static float bypass_ramp = 0.f;
  static constexpr float kBypassRampRate = 1.f / 240.f;
  const float bypass_target = bypass ? 0.f : 1.f;

  if (bypass && bypass_ramp < 0.0001f) {
    // Fully settled in bypass — zero filter states so re-engage starts clean,
    // then pass dry signal through without touching any DSP or SDRAM.
    dmm.Reset();
    shimmer.Reset();
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
  // post_gain is set equal across modes — the SA571 compressor targets 0.25 RMS
  // output in all three modes, so post_gain is the only level control.
  // High mode runs intentionally hotter than Med/Low — post_gain is higher to
  // reflect the extra drive character. Med and Low remain level-matched to each other.
  if (sw1 == Hothouse::TOGGLESWITCH_UP) {
    // High: very hard preamp clip, aggressive compressor pumping, maximum harmonic content
    preamp_gain = 12.0f;  post_gain = 1.0f;
  } else if (sw1 == Hothouse::TOGGLESWITCH_MIDDLE) {
    // Med: noticeable saturation and compression snap
    preamp_gain = 4.0f;  post_gain = 0.80f;
  } else {
    // Low: gentle grit, most transparent; guitar vol drives saturation depth
    preamp_gain = 2.0f;  post_gain = 0.80f;
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
  // reverb.SetFeedback / reverb.SetLpFreq each do one expf() — cheap, no gating needed.
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
    smooth_verb_lpf += 0.01f * (mp_b.reverb_lpf_hz - smooth_verb_lpf);
    reverb.SetLpFreq(smooth_verb_lpf, static_cast<float>(kSampleRate));
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

    // Advance bypass ramp (linear, 5 ms)
    if (bypass_ramp < bypass_target)
      bypass_ramp = fminf(bypass_ramp + kBypassRampRate, bypass_target);
    else if (bypass_ramp > bypass_target)
      bypass_ramp = fmaxf(bypass_ramp - kBypassRampRate, bypass_target);

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
    const float comp_out = dmm.Compress(preamp_out) * post_gain;

    // 3. Anti-alias LPF (2-pole Butterworth, 8 kHz) — before BBD write.
    //    Removes content the BBD can't reproduce; adds the slight HF rolloff
    //    present on all DMM dry tones even without delay.
    const float aa_out = dmm.AaFilter(comp_out);

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
    const float delay_out = delay_line.Read();

    // 4. Feedback path LPF: warms each successive repeat — one-pole at ~5 kHz.
    //    Feeds back through the BBD-bandwidth LPF too, so long echoes get
    //    progressively darker and thicker, exactly like the real DMM.
    const float fb_out = dmm.FbFilter(delay_out);

    // During freeze, fade new input to zero so delay just recirculates.
    const float delay_input = tape_out * (1.f - freeze_blend);

    // 5. BBD bandwidth LPF on the write path (block-rate bbd_c coefficient).
    //    At short times: near-transparent (~9 kHz).
    //    At long times: dark (~2 kHz) — matches real MN3005 characteristics.
    //
    //    Feedback soft-clip: tanhf only on the recycled signal, not the clean input.
    //    Models the BBD input op-amp clipping — repeats get progressively warmer/dirtier
    //    as they accumulate, exactly like the real DMM at higher feedback settings.
    //    Safe here: PitchShifter is in the shimmer loop, not the delay loop.
    const float fb_sig    = fb_out * fb;
    const float fb_sat    = tanhf(fb_sig * 2.f) * 0.5f;
    delay_line.Write(dmm.BbdFilter(delay_input, bbd_c) + fb_sat);

    // 6. Anti-image LPF after BBD (same 8 kHz Butterworth coefficients as aa).
    //    Reconstruction filter; also rounds off any BBD clock-noise edges.
    const float ai_out = dmm.AiFilter(delay_out);

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
    if (!std::isfinite(shimmer.buf)) shimmer.buf = 0.f;

    // Reverb input: crossfade tone-filtered signal → BBD-expanded output.
    // expanded carries the full DMM delay character; tape_out is the dry tone.
    const float verb_src = lerpf(tape_out, expanded, mp.delay_send)
                         * (1.f - freeze_blend);
    float pre_verb_l = verb_src + shimmer.buf * shimmer_amt;

    // Gate reverb when its contribution to the mix would be inaudible.
    // reverb_send < 0.005 → output scaling < 0.5% → silent in the mix.
    float verbL, verbR;
    if(mp.reverb_send > 0.005f || shimmer_amt > 0.001f)
    {
      reverb.Process(pre_verb_l, &verbL, &verbR);
    }
    else
    {
        verbL = verbR = 0.f;
    }

    // Guard against NaN (can propagate from reverb internal state)
    if (!std::isfinite(verbL)) verbL = 0.f;
    if (!std::isfinite(verbR)) verbR = 0.f;

    shimmer.Process(verbL, shimmer_amt);

    // ── Output mix ────────────────────────────────────────────────────────────
    //
    // Dry path: crossfade tape→delay (same blend as reverb input).
    // Reverb blends in on top, scaled by reverb_send.
    //
    // tanhf on verbL/verbR before the 2.0× scale: at typical levels (verbL ≤ 0.7)
    // the reduction is <10% and the scale factor compensates. At extreme shimmer
    // build-up (verbL = 2+) it prevents the final tanhf from hitting near-unity
    // (≈0.999) which sounds like a harsh brick-wall clip.
    // Safe: shimmer path already consumed raw verbL above this point.
    const float dry_path = lerpf(tape_out, expanded, mp.delay_send);
    const float dry_level = fmaxf(0.15f, 1.f - mp.reverb_send * 0.7f);
    const float wetL = dry_path * dry_level
                     + tanhf(verbL) * mp.reverb_send * 2.0f;
    const float wetR = dry_path * dry_level
                     + tanhf(verbR) * mp.reverb_send * 2.0f;

    // Single tanhf at the output — NOT in a feedback loop, so no
    // waveform flattening. Just smooth, warm analog-style limiting.
    out[0][i] = lerpf(dry, tanhf(wetL), mix * bypass_ramp);
    out[1][i] = lerpf(dry, tanhf(wetR), mix * bypass_ramp);
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
  p_tone.Init   (hw.knobs[Hothouse::KNOB_5],  4000.f, 18000.f, Parameter::LOGARITHMIC);
  p_mix.Init    (hw.knobs[Hothouse::KNOB_6],  0.f,    1.f,     Parameter::LINEAR);

  // ── DSP init ─────────────────────────────────────────────────────────────────

  dc_block.Init(sr);

  dmm.Init(sr);  // 8 kHz anti-alias/anti-image Butterworth + 5 kHz feedback LPF

  tone_filter.Init(sr);
  tone_filter.SetFreq(8000.f);
  tone_filter.SetRes(0.f);

  delay_line.Init();

  mod_lfo.Init(sr);
  mod_lfo.SetWaveform(Oscillator::WAVE_SIN);
  mod_lfo.SetFreq(1.f);
  mod_lfo.SetAmp(0.5f);

  reverb.SetFeedback(0.80f);
  reverb.SetLpFreq(8000.f, sr);

  // Shimmer: octave up (+12 st). TODO: perfect 5th (+7) as a future extension.
  shimmer.Init(sr, &pitch);

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
      const bool tap_blink = (now - tap.GetBlinkMs()) < 80;

      if (tap_blink) {
        led1_brightness = 1.f;  // 80 ms flash on each tap
      } else if (bypass) {
        led1_brightness = 0.f;
        led_lfo_phase   = 0.f;
      } else if (tap.IsFreeze()) {
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
