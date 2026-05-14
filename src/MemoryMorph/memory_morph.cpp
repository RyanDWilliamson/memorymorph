// Memory Morph
// Chase Bliss–inspired morphable tape saturation / Memory Man delay /
// ambient shimmer reverb for the Cleveland Music Co. HotHouse.
//
// Hardware: Daisy Seed (STM32H750 ARM Cortex-M7)
// Sample rate: 48 kHz  |  Boost mode: 480 MHz  |  Block size: 48
//
// ── Two instrument modes ──────────────────────────────────────────────────────
//   DMM     (boot default) — Electro-Harmonix Deluxe Memory Man circuit model
//   SDD-555                — Roland SRE-555 Chorus Echo + SDD-320 Dimension D
//
//   Mode switch: hold FS1 + FS2 + all toggles DOWN + mix fully dry for 3 s.
//   Both LEDs blink alternating 3× to confirm. Toggles back the same way.
//
// ── DMM mode: MORPH three-zone sweep (KNOB_1) ────────────────────────────────
//   0.0  Tape    — saturated tape tone, no delay, no reverb
//   0.5  Echo    — warm Memory Man-style echo with light saturation
//   1.0  Ambient — modulated shimmer reverb wash
//
// ── DMM mode: Controls ───────────────────────────────────────────────────────
//   KNOB_1  Morph        — sweeps all parameters across three zones
//   KNOB_2  Time         — delay time 50 ms – 2000 ms (log)
//   KNOB_3  Repeats      — delay feedback 0 – 97%
//   KNOB_4  Depth        — modulation intensity (scaled by Morph)
//   KNOB_5  Tone         — LPF cutoff 4000 Hz – 18 kHz (log)
//   KNOB_6  Mix          — dry/wet blend
//
//   TOGGLESWITCH_1  Input drive: UP=High  MID=Med  DOWN=Low
//   TOGGLESWITCH_2  Modulation type: UP=Chorus  MID=Vibrato  DOWN=Wow/Flutter
//   TOGGLESWITCH_3  Reverb tail: UP=Short plate  MID=Long  DOWN=Shimmer
//
//   FOOTSWITCH_2  Bypass toggle    (LED_2 on = active)
//   FOOTSWITCH_1  Tap tempo (short press) / Freeze (hold ≥ 1500 ms)
//                 DFU bootloader: FS1 only, hold 10 s + all toggles DOWN + mix 0
//
// ── SDD-555 mode: Controls ───────────────────────────────────────────────────
//   KNOB_1  Morph        — Echo only → +Chorus → +Verb
//   KNOB_2  Echo time    — tape echo 50–500 ms log (authentic SRE-555, tap-synced)
//   KNOB_3  Echo feedback — tape echo repeats 0 – 0.95
//   KNOB_4  Verb decay   — whichever verb SW3 selects
//   KNOB_5  Mech age     — HF rolloff + breathing LFO
//   KNOB_6  Mix          — dry/wet blend
//
//   TOGGLESWITCH_1  NE570 drive (LINEAR, no preamp clip): UP=Hot MID=Warm DOWN=Clean
//   TOGGLESWITCH_2  Chorus: UP=BBD (CE-1)  MID=Eventide pitch  DOWN=Dimension D
//   TOGGLESWITCH_3  Verb: UP=AMS Non-Lin  MID=Wildcard Resonator  DOWN=Spring only
//
//   FOOTSWITCH_2  Bypass toggle    (LED_2 on = active)
//   FOOTSWITCH_1  Tap tempo (syncs echo time) / Freeze
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>

#include "daisysp.h"
#include "hothouse.h"
#include "constants.h"
#include "morph.h"
#include "plate_reverb.h"
#include "dmm_chain.h"
#include "tap_tempo.h"
#include "shimmer.h"
#include "ne570.h"
#include "bbd_chorus.h"
#include "spring_reverb.h"
#include "nl_verb.h"

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
// kTwoPi now lives in constants.h (shared with the DSP headers).

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

enum class ActiveMode : uint8_t { DMM, SDD555 };
static volatile ActiveMode active_mode = ActiveMode::DMM;

static DmmChain      dmm;
static ShimmerVoice  shimmer;
static TapTempoState tap;

// ── SDD-555 mode DSP objects ─────────────────────────────────────────────────
// Each is built up in its own Phase. Phase 2: Ne570 compander (VCA dirt).
// Phase 3: BBD chorus + per-channel matched expanders.
//
// Three Ne570 instances because the SRE-555 uses one mono compressor before
// the BBD and one expander per stereo output of the BBD — each expander tracks
// its own channel's envelope to undo the compression accurately on stereo material.
static Ne570       ne570;        // pre-BBD compressor (mono)
static Ne570       ne570_exp_l;  // post-BBD expander, L channel
static Ne570       ne570_exp_r;  // post-BBD expander, R channel
static BbdChorus    chorus;
static SpringReverb spring;       // 3-spring Accutronics 8AB2D1A model
static NlVerb       nl_verb;      // AMS Non-Lin gated + Wildcard Resonator

// ── MechAge inline state (SDD-555) ───────────────────────────────────────────
// "Mechanical age" — KNOB_5 in SDD-555 mode. Simulates worn tape / aged tank:
// HF rolloff cutoff lerps from ~18 kHz (pristine) to ~4 kHz (worn), with a
// slow ~0.4 Hz "breathing" LFO modulating the cutoff by ±20% × age. No big
// delay line — under 100 bytes of state, kept inline per the plan.
static float mech_lpf_z_l   = 0.f;
static float mech_lpf_z_r   = 0.f;
static float mech_wow_phase = 0.f;

// ── SDD-555 tape echo state ──────────────────────────────────────────────────
// Reuses the existing SDRAM `delay_line` (mono, 2 s) — DMM and SDD-555 never
// run simultaneously, so the buffer is single-owner per mode. delay_line.Init()
// is called on every mode switch to zero out the previous mode's residue.
//
// One-pole 6 kHz LPF in the feedback path models tape's natural HF rolloff
// (each repeat gets progressively darker). `tanhf` on the feedback signal
// soft-clips at high amplitudes — safe here because the chorus PitchShifter
// (Eventide mode) is in a separate forward path, NOT this feedback loop.
static float sdd_fb_lpf_z       = 0.f;
static float sdd_smooth_echo_smp = 2400.f;

// ── SDD-555 MORPH lerp table ─────────────────────────────────────────────────
// Anchors define the three-zone sweep across KNOB_1 in SDD-555 mode. Tape
// echo runs at full level across all anchors — the morph controls only how
// much chorus motion and reverb get layered on top of the echo.
//   0.0  Echo only         : no chorus modulation, no verb
//   0.5  Echo + Chorus      : full chorus depth, light verb (the "Chorus Echo" sound)
//   1.0  Echo + Chorus + Verb : full ambient wash
struct SddMorphParams {
  float verb_send;
  float chorus_depth_scale;
};

static inline SddMorphParams ComputeSddMorph(float m) {
  static constexpr SddMorphParams a0 = { 0.0f, 0.0f };
  static constexpr SddMorphParams a1 = { 0.3f, 1.0f };
  static constexpr SddMorphParams a2 = { 1.0f, 1.0f };
  if (m < 0.5f) {
    const float t = m * 2.f;
    return { lerpf(a0.verb_send,          a1.verb_send,          t),
             lerpf(a0.chorus_depth_scale, a1.chorus_depth_scale, t) };
  }
  const float t = (m - 0.5f) * 2.f;
  return { lerpf(a1.verb_send,          a2.verb_send,          t),
           lerpf(a1.chorus_depth_scale, a2.chorus_depth_scale, t) };
}

// Smoothed delay time to avoid zipper artifacts when turning the knob
static float smooth_delay_smp = 2400.f;  // ~50 ms default

// Smoothed freeze blend (0=normal, 1=frozen) — ramps over ~10 ms to avoid pops
static float freeze_blend = 0.f;

// ── Bypass ramp + shared smoothing constants ─────────────────────────────────
// Linear ramp over ~5 ms (240 samples at 48 kHz) eliminates engage/disengage
// pops. 0.0 = fully bypassed, 1.0 = fully active. File-scope so DmmBlock and
// Sdd555Block (extracted from AudioCallback) can both advance it.
static float bypass_ramp = 0.f;
static constexpr float kBypassRampRate = 1.f / 240.f;
// Per-sample smoothing coefficient (~5 ms time constant at 48 kHz).
static constexpr float kSmooth = 0.0002f;

// ── BlockInputs ──────────────────────────────────────────────────────────────
// Shared parameter snapshot AudioCallback reads once per block and hands to
// the active mode's block function. DMM uses every field; SDD-555 uses only
// what its own dispatch needs (mix_raw, freeze info, toggle positions) and
// reads its remaining knobs via hw.GetKnobValue() since they map differently.
struct BlockInputs {
  float morph_raw;
  float time_s;       // already block-rate smoothed
  float repeats;
  float depth;
  float tone_hz;
  float mix_raw;
  float freeze_blend;
  bool  frozen_now;
  Hothouse::ToggleswitchPosition sw1, sw2, sw3;
};

// Forward declarations — definitions live after AudioCallback for readability.
static void DmmBlock   (AudioHandle::InputBuffer  in,
                         AudioHandle::OutputBuffer out,
                         size_t                    size,
                         const BlockInputs&        p,
                         float                     bypass_target);
static void Sdd555Block(AudioHandle::InputBuffer  in,
                         AudioHandle::OutputBuffer out,
                         size_t                    size,
                         const BlockInputs&        p,
                         float                     bypass_target);

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

  // ── Snapshot all per-block inputs into `p` ──────────────────────────────────
  BlockInputs p;
  p.morph_raw           = p_morph.Process();
  const float knob_time  = p_time.Process();              // seconds (DMM-mapped)
  p.repeats             = p_repeats.Process();
  const float time_target = tap.GetTimeS(knob_time);
  p.depth               = p_depth.Process();
  p.tone_hz             = p_tone.Process();
  p.mix_raw             = p_mix.Process();

  // Block-rate glide for delay time (tap-tempo slide). Used by DmmBlock;
  // Sdd555Block has its own knob mapping for echo time (50–500 ms log).
  static float smooth_time = 0.3f;
  smooth_time += 0.01f * (time_target - smooth_time);
  p.time_s = smooth_time;

  // Freeze ramp: asymmetric — engage 0.015 (τ≈67 ms) / release 0.03 (τ≈33 ms).
  const float freeze_target = tap.IsFreeze() ? 1.f : 0.f;
  const float freeze_rate   = (freeze_target > freeze_blend) ? 0.015f : 0.03f;
  freeze_blend += freeze_rate * (freeze_target - freeze_blend);
  p.freeze_blend = freeze_blend;
  p.frozen_now   = freeze_blend > 0.5f;

  p.sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
  p.sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
  p.sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);

  // ── Soft bypass settled early-return ────────────────────────────────────────
  const float bypass_target = bypass ? 0.f : 1.f;
  if (bypass && bypass_ramp < 0.0001f) {
    if (active_mode == ActiveMode::DMM) {
      dmm.Reset();
      shimmer.Reset();
    } else {
      ne570.Reset();
      ne570_exp_l.Reset();
      ne570_exp_r.Reset();
      chorus.Reset();
      spring.Reset();
      nl_verb.Reset();
      mech_lpf_z_l = mech_lpf_z_r = 0.f;
      sdd_fb_lpf_z = 0.f;
    }
    for (size_t i = 0; i < size; ++i) {
      out[0][i] = in[0][i];
      out[1][i] = in[0][i];
    }
    return;
  }

  // ── Mode dispatch ───────────────────────────────────────────────────────────
  if (active_mode == ActiveMode::SDD555)
    Sdd555Block(in, out, size, p, bypass_target);
  else
    DmmBlock   (in, out, size, p, bypass_target);
}

// ── Sdd555Block ──────────────────────────────────────────────────────────────
// SDD-555 mode per-sample loop. See AGENTS.md "SDD-555 signal chain" for the
// full path: drive → compress → tape echo → chorus → expand → MechAge → verb.
static void Sdd555Block(AudioHandle::InputBuffer  in,
                         AudioHandle::OutputBuffer out,
                         size_t                    size,
                         const BlockInputs&        p,
                         float                     bypass_target) {
    // SW1 input drive — LINEAR gain into the NE570 (no preamp clip).
    // The real SRE-555 input was a clean JRC4558 op-amp buffer with ~24× headroom
    // at guitar levels; it didn't clip. All program-dependent dirt comes from
    // the NE570's polynomial + the compressor's RMS pumping in Ne570::Compress().
    // Higher drive just pushes the NE570 harder — more polynomial coloration and
    // more compressor pumping, never square-wave saturation.
    float sdd_drive;
    if      (p.sw1 == Hothouse::TOGGLESWITCH_UP)     sdd_drive = 2.0f;  // Hot
    else if (p.sw1 == Hothouse::TOGGLESWITCH_MIDDLE) sdd_drive = 1.0f;  // Warm
    else                                              sdd_drive = 0.5f;  // Clean

    // Raw knob reads — SDD-555 mappings differ from DMM, so bypass Parameter::Process.
    // KNOB_2 has its own log curve here (50–500 ms — authentic SRE-555 range)
    // rather than reusing the DMM 50 ms – 2 s pipeline; the real unit topped out
    // around 320 ms single-head / 500 ms multi-head, never 2 s.
    // KNOB_3 → echo feedback. KNOB_4 → verb decay. KNOB_5 → mech age. KNOB_1 → MORPH.
    const float morph_knob      = hw.GetKnobValue(Hothouse::KNOB_1);
    const float echo_time_knob  = hw.GetKnobValue(Hothouse::KNOB_2);
    const float echo_fb_knob    = hw.GetKnobValue(Hothouse::KNOB_3);
    const float verb_decay_knob = hw.GetKnobValue(Hothouse::KNOB_4);
    const float mech_age_knob   = hw.GetKnobValue(Hothouse::KNOB_5);

    const SddMorphParams sm = ComputeSddMorph(morph_knob);
    const float echo_fb     = echo_fb_knob * 0.95f;  // hard cap below self-oscillation

    // Echo time. Log mapping 50 → 500 ms; tap-tempo override clamped to 500 ms.
    // tap cancellation is handled by the GetTimeS() call at the top of
    // AudioCallback (its prev_knob_time tracks KNOB_2 raw → seconds), so
    // turning KNOB_2 still clears tap.active here without extra plumbing.
    static constexpr float kSddMaxEchoS = 0.5f;
    static constexpr float kSddMinEchoS = 0.05f;
    const float sdd_knob_s = kSddMinEchoS
                           * expf(echo_time_knob * logf(kSddMaxEchoS / kSddMinEchoS));
    float echo_time_s;
    if (tap.IsActive() && tap.tempo_s > 0.f) {
      echo_time_s = fminf(kSddMaxEchoS, tap.tempo_s);
    } else {
      echo_time_s = sdd_knob_s;
    }

    // Glide the per-sample read pointer to avoid pitch chirp on knob moves.
    const float echo_target_smp = echo_time_s * static_cast<float>(kSampleRate);
    sdd_smooth_echo_smp += 0.01f * (echo_target_smp - sdd_smooth_echo_smp);
    sdd_smooth_echo_smp = fmaxf(48.f,
                                fminf(static_cast<float>(kMaxDelaySmp - 1),
                                      sdd_smooth_echo_smp));
    delay_line.SetDelay(sdd_smooth_echo_smp);

    // Feedback path LPF coefficient — tape darkens repeats at ~6 kHz cutoff.
    const float sdd_fb_lpf_c = OnePoleCoeff(6000.f, static_cast<float>(kSampleRate));

    // Chorus rate is no longer directly knob-controlled in SDD-555 mode (KNOB_2
    // is now echo time). Fixed at 0.6 Hz — a musical default for slow chorus.
    // SW2 selects character (BBD / Eventide / Dimension D); MORPH controls depth.
    chorus.SetRate(0.6f, static_cast<float>(kSampleRate));
    chorus.SetDepth(sm.chorus_depth_scale);

    spring.SetDecay (verb_decay_knob * 0.95f);
    nl_verb.SetDecay(verb_decay_knob * 0.95f);

    // MechAge: HF cutoff lerps 18 kHz → 4 kHz with age; a slow ~0.4 Hz LFO
    // modulates it ±20% × age for "breathing." Block-rate update is fine —
    // the LFO is slow, the LPF state advances per sample.
    static constexpr float kMechWowHz = 0.4f;
    mech_wow_phase += kMechWowHz * static_cast<float>(size)
                                 / static_cast<float>(kSampleRate);
    if (mech_wow_phase >= 1.f) mech_wow_phase -= 1.f;
    const float mech_lfo     = sinf(mech_wow_phase * kTwoPi);
    const float mech_base_hz = lerpf(18000.f, 4000.f, mech_age_knob);
    const float mech_hz      = mech_base_hz * (1.f + mech_lfo * mech_age_knob * 0.2f);
    const float mech_c       = OnePoleCoeff(mech_hz, static_cast<float>(kSampleRate));

    static float sdd_smooth_mix = 0.f;

    for (size_t i = 0; i < size; ++i) {
      const float dry = in[0][i];

      sdd_smooth_mix += kSmooth * (p.mix_raw - sdd_smooth_mix);
      const float mix = sdd_smooth_mix;

      if (bypass_ramp < bypass_target)
        bypass_ramp = fminf(bypass_ramp + kBypassRampRate, bypass_target);
      else if (bypass_ramp > bypass_target)
        bypass_ramp = fmaxf(bypass_ramp - kBypassRampRate, bypass_target);

      // dc_block → linear drive → NE570 compress (VCA polynomial dirt baked in).
      // No tanhf preamp here — that's DMM's NJM4558 op-amp model. The SRE-555
      // input was clean; the NE570 itself is the only intentional nonlinearity.
      float sig = dc_block.Process(dry);
      sig = sig * sdd_drive;
      sig = ne570.Compress(sig);

      // ── Tape echo (SRE-555 echo section) ────────────────────────────────
      // Single tap with tape-darkened, soft-clipped feedback. Output is the
      // mono dry+wet sum that feeds the chorus, so every repeat gets chorused
      // and verbed downstream — exactly like the real SRE-555 routing.
      const float echo_repeat = delay_line.Read();
      sdd_fb_lpf_z += sdd_fb_lpf_c * (echo_repeat - sdd_fb_lpf_z);
      const float fb_sat = tanhf(sdd_fb_lpf_z * echo_fb);
      delay_line.Write(sig + fb_sat);
      sig = sig + echo_repeat;

      // Chorus algorithm (SW2). Branch on block-rate constant — predictor handles cleanly.
      float wetL, wetR;
      if      (p.sw2 == Hothouse::TOGGLESWITCH_UP)     chorus.ProcessBbd       (sig, wetL, wetR);
      else if (p.sw2 == Hothouse::TOGGLESWITCH_MIDDLE) chorus.ProcessEventide  (sig, wetL, wetR);
      else                                              chorus.ProcessDimensionD(sig, wetL, wetR);

      // Matched stereo expanders.
      const float exp_l = ne570_exp_l.Expand(wetL);
      const float exp_r = ne570_exp_r.Expand(wetR);

      // MechAge LPF per channel — block-rate cutoff, per-sample state advance.
      mech_lpf_z_l += mech_c * (exp_l - mech_lpf_z_l);
      mech_lpf_z_r += mech_c * (exp_r - mech_lpf_z_r);
      const float aged_l = mech_lpf_z_l;
      const float aged_r = mech_lpf_z_r;

      // SW3 verb selection. Only one runs per sample; the others' state holds.
      const float verb_in = 0.5f * (aged_l + aged_r);
      float verb_l = 0.f, verb_r = 0.f;
      if      (p.sw3 == Hothouse::TOGGLESWITCH_UP)     nl_verb.ProcessAms     (verb_in, verb_l, verb_r);
      else if (p.sw3 == Hothouse::TOGGLESWITCH_MIDDLE) nl_verb.ProcessWildcard(verb_in, verb_l, verb_r);
      else                                              spring  .Process       (verb_in, verb_l, verb_r);

      // MORPH-scaled verb send — at morph=0 the chorus path is dry, at
      // morph=1 the verb is at full return level.
      const float wet_l = aged_l + verb_l * sm.verb_send;
      const float wet_r = aged_r + verb_r * sm.verb_send;

      out[0][i] = lerpf(dry, wet_l, mix * bypass_ramp);
      out[1][i] = lerpf(dry, wet_r, mix * bypass_ramp);
    }
    return;
  }

// ── DmmBlock ─────────────────────────────────────────────────────────────────
// DMM mode per-sample loop. See AGENTS.md "DMM signal chain" for the full path:
// drive → tanhf preamp (NJM4558 model) → SA571 compress → AA filter → tone →
// BBD delay with feedback soft-clip → AI filter → reverb + shimmer feedback loop.
static void DmmBlock(AudioHandle::InputBuffer  in,
                     AudioHandle::OutputBuffer out,
                     size_t                    size,
                     const BlockInputs&        p,
                     float                     bypass_target) {
  // SW1 drive — preamp_gain feeds tanhf clip then SA571 compress; post_gain
  // compensates output level. High runs intentionally hotter than Med/Low.
  float preamp_gain, post_gain;
  if      (p.sw1 == Hothouse::TOGGLESWITCH_UP)     { preamp_gain = 12.0f; post_gain = 1.0f;  }
  else if (p.sw1 == Hothouse::TOGGLESWITCH_MIDDLE) { preamp_gain = 4.0f;  post_gain = 0.80f; }
  else                                              { preamp_gain = 2.0f;  post_gain = 0.80f; }

  tone_filter.SetRes(0.f);

  const bool shimmer_on  = (p.sw3 == Hothouse::TOGGLESWITCH_DOWN);
  const bool short_plate = (p.sw3 == Hothouse::TOGGLESWITCH_UP);

  // LFO rate depends on SW2 mod type and depth knob.
  float lfo_hz;
  if      (p.sw2 == Hothouse::TOGGLESWITCH_UP)     lfo_hz = 0.5f + p.depth * 4.5f;
  else if (p.sw2 == Hothouse::TOGGLESWITCH_MIDDLE) lfo_hz = 1.5f + p.depth * 6.5f;
  else                                              lfo_hz = 0.1f + p.depth * 1.4f;
  mod_lfo.SetFreq(lfo_hz);

  // ── DMM-only smoothing state — static locals persist across calls ──────────
  static float smooth_morph    = 0.f;
  static float smooth_mix      = 0.5f;
  static float smooth_repeats  = 0.f;
  static float smooth_depth    = 0.f;
  static float smooth_tone     = 4000.f;
  static float smooth_verb_lpf = 8500.f;

  // Block-rate glide for LFO amplitude — avoids 1 kHz step clicks on depth knob.
  smooth_depth += 0.02f * (p.depth - smooth_depth);
  const MorphParams mp_lfo = ComputeMorph(smooth_morph);
  mod_lfo.SetAmp(smooth_depth * mp_lfo.mod_depth_scale);

  const float base_delay_smp = p.time_s * static_cast<float>(kSampleRate);

  // BBD bandwidth narrows with longer delay time — real MN3005 behaviour.
  const float bbd_cutoff = fminf(9000.f, fmaxf(2000.f, 4000.f / p.time_s));
  const float bbd_c      = OnePoleCoeff(bbd_cutoff, static_cast<float>(kSampleRate));

  // ── Block-rate reverb + tone config ────────────────────────────────────────
  {
    const MorphParams mp_b = ComputeMorph(smooth_morph);
    const float rev_decay_b = lerpf(mp_b.reverb_decay, 0.999f, p.freeze_blend);
    float eff_decay_b = rev_decay_b;
    if (short_plate)
      eff_decay_b = p.frozen_now ? 0.999f : fminf(rev_decay_b, 0.80f);
    const float shimmer_amt_b = shimmer_on ? 0.25f * mp_b.reverb_send : 0.f;
    if (shimmer_amt_b > 0.001f && !p.frozen_now)
      eff_decay_b = fminf(eff_decay_b, 0.93f);
    reverb.SetFeedback(eff_decay_b);
    smooth_verb_lpf += 0.01f * (mp_b.reverb_lpf_hz - smooth_verb_lpf);
    reverb.SetLpFreq(smooth_verb_lpf, static_cast<float>(kSampleRate));
  }

  smooth_tone += 0.01f * (p.tone_hz - smooth_tone);
  tone_filter.SetFreq(smooth_tone);

  for (size_t i = 0; i < size; ++i) {
    const float dry = in[0][i];

    // Per-sample smoothing eliminates 1 kHz staircase whine from block-rate updates.
    smooth_morph   += kSmooth * (p.morph_raw - smooth_morph);
    smooth_mix     += kSmooth * (p.mix_raw   - smooth_mix);
    smooth_repeats += kSmooth * (p.repeats   - smooth_repeats);
    const float morph = smooth_morph;
    const float mix   = smooth_mix;

    if (bypass_ramp < bypass_target)
      bypass_ramp = fminf(bypass_ramp + kBypassRampRate, bypass_target);
    else if (bypass_ramp > bypass_target)
      bypass_ramp = fmaxf(bypass_ramp - kBypassRampRate, bypass_target);

    // fb per-sample: eliminates ADC jitter on KNOB_3 → 1 kHz AM tone.
    const float fb = lerpf(smooth_repeats, 0.999f, p.freeze_blend);
    const MorphParams mp = ComputeMorph(morph);
    const float shimmer_amt = shimmer_on ? 0.25f * mp.reverb_send : 0.f;

    // dc_block → tanhf preamp (NJM4558 op-amp soft clip) → SA571 compress
    float sig = dc_block.Process(dry);
    const float preamp_out = tanhf(sig * preamp_gain);
    const float comp_out   = dmm.Compress(preamp_out) * post_gain;
    const float aa_out     = dmm.AaFilter(comp_out);

    tone_filter.Process(aa_out);
    const float tape_out = tone_filter.Low();

    // Delay with LFO modulation (±1.5% — wow/flutter range).
    const float lfo_val    = mod_lfo.Process();
    float       target_smp = base_delay_smp + lfo_val * base_delay_smp * 0.015f;
    target_smp = fmaxf(target_smp, 48.f);
    target_smp = fminf(target_smp, static_cast<float>(kMaxDelaySmp - 1));
    smooth_delay_smp += 0.0004f * (target_smp - smooth_delay_smp);

    delay_line.SetDelay(smooth_delay_smp);
    const float delay_out = delay_line.Read();

    const float fb_out      = dmm.FbFilter(delay_out);
    const float delay_input = tape_out * (1.f - p.freeze_blend);

    // Feedback soft-clip — BBD input op-amp model. Safe — PitchShifter lives
    // in the shimmer feedback loop, not this delay loop.
    const float fb_sig = fb_out * fb;
    const float fb_sat = tanhf(fb_sig * 2.f) * 0.5f;
    delay_line.Write(dmm.BbdFilter(delay_input, bbd_c) + fb_sat);

    const float ai_out   = dmm.AiFilter(delay_out);
    const float expanded = ai_out;  // no expander — digital has no noise floor

    // ── Reverb + shimmer feedback loop ───────────────────────────────────────
    // NO tanhf inside this loop — see AGENTS.md constraint #7.
    if (!std::isfinite(shimmer.buf)) shimmer.buf = 0.f;

    const float verb_src = lerpf(tape_out, expanded, mp.delay_send)
                         * (1.f - p.freeze_blend);
    float pre_verb_l = verb_src + shimmer.buf * shimmer_amt;

    float verbL, verbR;
    if (mp.reverb_send > 0.005f || shimmer_amt > 0.001f) {
      reverb.Process(pre_verb_l, &verbL, &verbR);
    } else {
      verbL = verbR = 0.f;
    }
    if (!std::isfinite(verbL)) verbL = 0.f;
    if (!std::isfinite(verbR)) verbR = 0.f;

    shimmer.Process(verbL, shimmer_amt);

    // Output mix — dry path crossfades tape→delay (same blend as reverb input).
    const float dry_path  = lerpf(tape_out, expanded, mp.delay_send);
    const float dry_level = fmaxf(0.15f, 1.f - mp.reverb_send * 0.7f);
    const float wetL = dry_path * dry_level + tanhf(verbL) * mp.reverb_send * 2.0f;
    const float wetR = dry_path * dry_level + tanhf(verbR) * mp.reverb_send * 2.0f;

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

  // SDD-555 mode DSP — Ne570 needs no Init (constants only); BbdChorus
  // computes filter coefficients and delay-tap distances from sr. The
  // PitchShifter is shared with DMM shimmer — chorus stores the pointer
  // and mode-switch code re-configures transposition on entry/exit.
  chorus.Init(sr, &pitch);
  spring.Init(sr);
  nl_verb.Init(sr);

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

    // ── Footswitch hold detection: mode switch + DFU ──────────────────────────────
    static uint32_t fs1_hold_start   = 0;
    static bool     fs1_was_held     = false;
    static uint32_t mode_combo_start = 0;
    static bool     mode_combo_fired = false;

    const bool fs1_held = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();
    const bool fs2_held = hw.switches[Hothouse::FOOTSWITCH_2].Pressed();

    if (fs1_held && !fs1_was_held)
      fs1_hold_start = now;

    const bool toggles_down =
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2) == Hothouse::TOGGLESWITCH_DOWN &&
        hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3) == Hothouse::TOGGLESWITCH_DOWN;
    const bool mix_zero = hw.GetKnobValue(Hothouse::KNOB_6) < 0.02f;

    // Mode switch: FS1 + FS2 + all toggles DOWN + mix dry, held 3 s.
    // Resets on any break in the combo so accidental partial holds don't accumulate.
    const bool mode_combo = fs1_held && fs2_held && toggles_down && mix_zero;
    if (!mode_combo) {
      mode_combo_start = now;
      mode_combo_fired = false;
    } else if (!mode_combo_fired && (now - mode_combo_start) >= 3000) {
      mode_combo_fired = true;
      hw.StopAudio();
      for (int bi = 0; bi < 3; bi++) {
        led_freeze.Set(1.f); led_bypass.Set(0.f);
        led_freeze.Update(); led_bypass.Update();
        System::Delay(150);
        led_freeze.Set(0.f); led_bypass.Set(1.f);
        led_freeze.Update(); led_bypass.Update();
        System::Delay(150);
      }
      led_freeze.Set(0.f); led_bypass.Set(0.f);
      led_freeze.Update(); led_bypass.Update();
      tap.is_freeze = false;
      tap.active    = false;
      active_mode   = (active_mode == ActiveMode::DMM) ? ActiveMode::SDD555 : ActiveMode::DMM;
      // Zero the shared SDRAM delay line — DMM's BBD content and SDD-555's
      // tape echo content are both stored here, and neither mode should hear
      // the other's residue when it takes over. ~384 KB SDRAM write — a few
      // milliseconds during the LED blink, imperceptible.
      delay_line.Init();
      sdd_fb_lpf_z = 0.f;
      smooth_delay_smp     = 2400.f;
      sdd_smooth_echo_smp  = 2400.f;
      // PitchShifter is shared between DMM shimmer (+12 st octave, fun=0.3)
      // and SDD-555 Eventide chorus (+0.20 st detune, fun=0). Reconfigure
      // here while audio is stopped so the next StartAudio block reads clean.
      if (active_mode == ActiveMode::SDD555) {
        pitch.SetTransposition(0.20f);
        pitch.SetFun(0.f);
      } else {
        pitch.SetTransposition(12.f);
        pitch.SetFun(0.3f);
      }
      hw.StartAudio(AudioCallback);
    }

    // DFU bootloader: FS1 only (FS2 not held) + all toggles DOWN + mix dry, held 10 s.
    // The !fs2_held guard prevents the mode-switch combo from also triggering DFU.
    if (fs1_held && !fs2_held && toggles_down && mix_zero && (now - fs1_hold_start) >= 10000) {
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
