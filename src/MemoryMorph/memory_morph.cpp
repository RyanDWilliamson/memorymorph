// Memory Morph
// Chase Bliss–inspired morphable tape saturation / Memory Man delay /
// ambient shimmer reverb for the Cleveland Music Co. HotHouse.
//
// Hardware: Daisy Seed (STM32H750 ARM Cortex-M7)
// Sample rate: 96 kHz  |  Boost mode: 480 MHz  |  Block size: 48
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
//   FOOTSWITCH_1  Freeze (single)  (LED_1 pulses at LFO rate when active)
//                 DFU bootloader   (hold 2 s)
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
using daisysp::DCBlock;
using daisysp::DelayLine;
using daisysp::Oscillator;
using daisysp::Overdrive;
using daisysp::PitchShifter;
using daisysp::ReverbSc;
using daisysp::Svf;
using daisysp::Wavefolder;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t kSampleRate  = 96000;
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
static DCBlock     dc_block;
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

static volatile bool bypass = true;
static volatile bool frozen = false;

// Shimmer feedback sample (read and written each audio sample)
static float shimmer_buf = 0.f;

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
    /*sat_drive=*/0.90f,
    /*mod_depth_scale=*/0.00f,
    /*reverb_decay=*/0.75f,
    /*reverb_lpf_hz=*/9000.f,
};

// Echo anchor: Memory Man-style delay with moderate saturation
static constexpr MorphParams kAnchorEcho = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/0.00f,
    /*sat_drive=*/0.45f,
    /*mod_depth_scale=*/0.50f,
    /*reverb_decay=*/0.78f,
    /*reverb_lpf_hz=*/8500.f,
};

// Ambient anchor: full reverb wash with shimmer and deep modulation
static constexpr MorphParams kAnchorAmbient = {
    /*delay_send=*/1.00f,
    /*reverb_send=*/1.00f,
    /*sat_drive=*/0.10f,
    /*mod_depth_scale=*/1.00f,
    /*reverb_decay=*/0.95f,
    /*reverb_lpf_hz=*/3500.f,
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
  if (fsw == Hothouse::FOOTSWITCH_1) frozen = !frozen;
}

static void OnDoublePress(Hothouse::Switches /*fsw*/) {
  // Reserved for future use
}

static void OnLongPress(Hothouse::Switches /*fsw*/) {
  // FOOTSWITCH_1 long press is handled by hw.CheckResetToBootloader()
  // in the main loop — no action needed here.
}

// ── Audio callback ─────────────────────────────────────────────────────────────

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size) {
  // Always call ProcessAllControls() first.
  hw.ProcessAllControls();

  // ── Read all parameters once per block ──────────────────────────────────────

  const float morph   = p_morph.Process();
  const float time_s  = p_time.Process();      // seconds
  const float repeats = p_repeats.Process();   // 0 – 0.97
  const float depth   = p_depth.Process();     // 0 – 1
  const float tone_hz = p_tone.Process();      // Hz
  const float mix     = p_mix.Process();       // 0 – 1

  const MorphParams mp = ComputeMorph(morph);

  // Freeze: clamp feedback and reverb decay to near-infinite
  const float fb        = frozen ? 0.999f : repeats;
  const float rev_decay = frozen ? 0.999f : mp.reverb_decay;

  // ── Toggle positions ─────────────────────────────────────────────────────────

  const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  // ── Update DSP module parameters ─────────────────────────────────────────────

  // Saturation drive (Overdrive and Wavefolder both updated; only one is used
  // per sample depending on sw1, but updating both is cheap and avoids pops).
  drive.SetDrive(mp.sat_drive);
  folder.SetGain(1.f + mp.sat_drive * 2.5f);

  // Tone filter: LPF on the saturated + delayed signal
  tone_filter.SetFreq(tone_hz);
  tone_filter.SetRes(0.f);

  // Reverb — short plate caps decay; shimmer and long plate use full morph decay
  float eff_decay = rev_decay;
  if (sw3 == Hothouse::TOGGLESWITCH_UP)
    eff_decay = frozen ? 0.999f : fminf(rev_decay, 0.80f);

  reverb.SetFeedback(eff_decay);
  reverb.SetLpFreq(mp.reverb_lpf_hz);

  // Shimmer amount — only audible when TOGGLESWITCH_3 is DOWN
  const float shimmer_amt = (sw3 == Hothouse::TOGGLESWITCH_DOWN) ? 0.35f : 0.f;

  // LFO rate depends on modulation type (sw2) and depth knob
  float lfo_hz;
  if      (sw2 == Hothouse::TOGGLESWITCH_UP)     lfo_hz = 0.5f + depth * 4.5f;  // Chorus:     0.5–5 Hz
  else if (sw2 == Hothouse::TOGGLESWITCH_MIDDLE)  lfo_hz = 1.5f + depth * 6.5f;  // Vibrato:    1.5–8 Hz
  else                                             lfo_hz = 0.1f + depth * 1.4f;  // Wow/Flutter: 0.1–1.5 Hz

  mod_lfo.SetFreq(lfo_hz);
  // Depth is scaled by MORPH so at Tape anchor (morph=0) there is no modulation
  mod_lfo.SetAmp(depth * mp.mod_depth_scale);

  // Delay time in samples
  const float base_delay_smp = time_s * static_cast<float>(kSampleRate);

  // ── Per-sample DSP loop ───────────────────────────────────────────────────────

  for (size_t i = 0; i < size; ++i) {
    const float dry = in[0][i];

    // DC block (removes low-frequency offset from guitar pickups)
    float sig = dc_block.Process(dry);

    // ── Saturation stage ───────────────────────────────────────────────────────
    float sat;
    if      (sw1 == Hothouse::TOGGLESWITCH_UP)     sat = drive.Process(sig);    // Tape: soft clip
    else if (sw1 == Hothouse::TOGGLESWITCH_MIDDLE)  sat = folder.Process(sig);   // Warm: wavefold
    else                                             sat = sig;                   // Clean: bypass

    // ── Tone filter ────────────────────────────────────────────────────────────
    tone_filter.Process(sat);
    const float tape_out = tone_filter.Low();  // Low-pass output

    // ── Delay with LFO modulation ──────────────────────────────────────────────
    // LFO modulates delay time by ±1.5% (wow/flutter range).
    // Chorus/Vibrato modes use the same LFO but at higher rates.
    const float lfo_val    = mod_lfo.Process();
    float       delay_smp  = base_delay_smp + lfo_val * base_delay_smp * 0.015f;
    delay_smp = fmaxf(delay_smp, 48.f);
    delay_smp = fminf(delay_smp, static_cast<float>(kMaxDelaySmp - 1));

    delay_line.SetDelay(delay_smp);
    const float delay_out = delay_line.Read();
    delay_line.Write(tape_out + delay_out * fb);

    // ── Reverb + shimmer feedback loop ────────────────────────────────────────
    // shimmer_buf holds the pitch-shifted (one octave up) reverb output from
    // the previous sample. It is mixed back into the reverb input to create the
    // ascending shimmer effect when TOGGLESWITCH_3 is DOWN.
    const float pre_verb_l = tape_out
                           + delay_out * mp.delay_send
                           + shimmer_buf * shimmer_amt;
    // Mono-in, stereo-out reverb (spread for ambient width on output)
    float verbL, verbR;
    reverb.Process(pre_verb_l, pre_verb_l, &verbL, &verbR);

    // Update shimmer buffer for the next sample
    shimmer_buf = pitch.Process(verbL);

    // ── Output mix ────────────────────────────────────────────────────────────
    // At Morph=0 (Tape):    only tape_out is heard (delay+reverb sends = 0)
    // At Morph=0.5 (Echo):  delay heard, no reverb
    // At Morph=1 (Ambient): delay + reverb wash
    const float wetL = tape_out
                     + delay_out * mp.delay_send
                     + verbL     * mp.reverb_send;
    const float wetR = tape_out
                     + delay_out * mp.delay_send
                     + verbR     * mp.reverb_send;

    if (bypass) {
      out[0][i] = dry;
      out[1][i] = dry;
    } else {
      out[0][i] = lerpf(dry, wetL, mix);
      out[1][i] = lerpf(dry, wetR, mix);
    }
  }
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main() {
  // boost=true: 480 MHz overclock — required for this DSP chain at 96 kHz.
  hw.Init(true);
  hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);
  hw.SetAudioBlockSize(48);
  const float sr = hw.AudioSampleRate();  // 96000.f

  // ── Parameters ───────────────────────────────────────────────────────────────

  p_morph.Init  (hw.knobs[Hothouse::KNOB_1],  0.f,    1.f,     Parameter::LINEAR);
  p_time.Init   (hw.knobs[Hothouse::KNOB_2],  0.05f,  1.6f,    Parameter::LOGARITHMIC);
  p_repeats.Init(hw.knobs[Hothouse::KNOB_3],  0.f,    0.97f,   Parameter::LINEAR);
  p_depth.Init  (hw.knobs[Hothouse::KNOB_4],  0.f,    1.f,     Parameter::LINEAR);
  p_tone.Init   (hw.knobs[Hothouse::KNOB_5],  800.f,  18000.f, Parameter::LOGARITHMIC);
  p_mix.Init    (hw.knobs[Hothouse::KNOB_6],  0.f,    1.f,     Parameter::LINEAR);

  // ── DSP init ─────────────────────────────────────────────────────────────────

  dc_block.Init();

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

      // LED_1: freeze / mod activity indicator
      // - Bypassed:      off
      // - Frozen:        slow 0.4 Hz pulse (dreamy hold)
      // - Active + mod:  pulse at LFO rate, brightness proportional to depth
      float led1_brightness;

      if (bypass) {
        led1_brightness = 0.f;
        led_lfo_phase   = 0.f;
      } else if (frozen) {
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

    // FOOTSWITCH_1 held for 2 s → LEDs flash three times → DFU bootloader mode.
    // This is the standard HotHouse reflash gesture; no extra code needed.
    hw.CheckResetToBootloader();
  }
}
