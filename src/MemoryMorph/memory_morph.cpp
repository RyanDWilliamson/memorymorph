// Memory Morph
// Chase Bliss–inspired morphable tape saturation / Memory Man delay /
// ambient shimmer reverb for the Cleveland Music Co. HotHouse.
//
// Hardware: Daisy Seed (STM32H750 ARM Cortex-M7)
// Sample rate: 48 kHz  |  Boost mode: 480 MHz  |  Block size: 48
//
// ── Three instrument presets ─────────────────────────────────────────────────
//   DMM               (boot default) — EHX Deluxe Memory Man (delay/shimmer)
//   SDD-555 Delay                   — SRE-555 Chorus Echo: drive → echo → mod/verb
//   SDD-555 Chorus Verb             — SRE-555 chain without echo: drive → mod/verb
//
//   Mode switch combos (each is a direct toggle with DMM):
//     SDD-555 Delay      : FS1+FS2 + SW1 UP   / SW2 DOWN / SW3 DOWN + Mix dry, 3 s
//     SDD-555 Chorus Verb: FS1+FS2 + SW1 DOWN / SW2 UP   / SW3 DOWN + Mix dry, 3 s
//   Both LEDs blink alternating 3× to confirm. From either SDD-555 preset,
//   the matching combo returns to DMM. (The all-DOWN toggle pattern is
//   reserved for the 10 s DFU bootloader hold with FS1 only.)
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
// ── SDD-555 Delay: MORPH three-zone sweep (KNOB_1) ───────────────────────────
//   0.0  Drive   — NE570 compander dirt only, no echo, no chorus, no verb
//   0.5  Echo    — tape echo on, light chorus motion, light verb
//   1.0  Ambient — echo + full chorus depth + full verb wash
//
// ── SDD-555 Chorus Verb: MORPH three-zone sweep (KNOB_1) ─────────────────────
//   0.0  Drive   — NE570 compander dirt only, no chorus, no verb
//   0.5  Chorus  — full chorus, no verb yet
//   1.0  Verb    — full chorus + full verb wash
//
// ── SDD-555 Delay: Controls ──────────────────────────────────────────────────
//   KNOB_1  Morph        — Drive → +Echo → +Chorus/Verb
//   KNOB_2  Echo time    — tape echo 50–500 ms log (tap-synced)
//   KNOB_3  Echo feedback — tape echo repeats 0 – 0.95
//   KNOB_4  Verb decay   — whichever verb SW3 selects (capped at 0.85)
//   KNOB_5  Mech age     — HF rolloff + breathing LFO
//   KNOB_6  Mix          — dry/wet blend
//   FOOTSWITCH_1  Tap tempo (syncs echo time) / Freeze
//
// ── SDD-555 Chorus Verb: Controls ────────────────────────────────────────────
//   KNOB_1  Morph        — Drive → +Chorus → +Verb
//   KNOB_2  Chorus rate  — 0.1–3 Hz log (tap-synced via FS1)
//   KNOB_3  Chorus depth — direct 0–1 swing intensity
//   KNOB_4  Verb decay   — whichever verb SW3 selects (capped at 0.85)
//   KNOB_5  Mech age     — HF rolloff + breathing LFO
//   KNOB_6  Mix          — dry/wet blend
//   FOOTSWITCH_1  Tap tempo (syncs chorus rate) / Freeze
//
// ── SDD-555 shared toggle map ────────────────────────────────────────────────
//   TOGGLESWITCH_1  NE570 drive (LINEAR, no preamp clip): UP=Hot MID=Warm DOWN=Clean
//   TOGGLESWITCH_2  Chorus: UP=CE-1 BBD  MID=H910 Micropitch  DOWN=Dimension D
//   TOGGLESWITCH_3  Verb: UP=AMS Non-Lin  MID=Wildcard Resonator  DOWN=Spring only
//   FOOTSWITCH_2    Bypass toggle    (LED_2 on = active)
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

// Fuzz drive override — DMM only. Toggled by FS1 double-press. When true AND
// SW1 is UP, the preamp gain ramps up to ~30× with a lower post_gain so the
// JRC4558 tanhf model saturates well past Hot territory into bona-fide fuzz.
// Other SW1 positions are unaffected. State is RAM-only (not persisted across
// reboots) — power-cycle returns to Hot.
static volatile bool fuzz_enabled = false;

// Three presets share one binary:
//   DMM                — boot default. NJM4558 preamp → SA571 → BBD → plate/shimmer.
//   SDD555_DELAY       — NE570 → tape echo → chorus → spring → AMS/Wildcard verb.
//   SDD555_CHORUSVERB  — NE570 → chorus → spring → AMS/Wildcard verb (NO tape echo).
enum class ActiveMode : uint8_t { DMM, SDD555_DELAY, SDD555_CHORUSVERB };
static volatile ActiveMode active_mode = ActiveMode::DMM;

static inline bool IsSdd555(ActiveMode m) {
  return m == ActiveMode::SDD555_DELAY || m == ActiveMode::SDD555_CHORUSVERB;
}

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

// ── DMM BBD clock drift state ────────────────────────────────────────────────
// Slow random walk on the BBD delay time. Differentiates the long-time "Echo"
// zone from the short-time "Tape" zone perceptually — at long delays the
// drift makes the trailing repeats wander like an old transport, at short
// delays it's inaudible. Block-rate update is plenty for ~0.1 Hz red noise.
static float    dmm_drift_z    = 0.f;
static uint32_t dmm_drift_seed = 0x12345678u;

// ── SRE-555 multi-rate flutter state (tape transport realism) ────────────────
// Real tape has at least two distinct pitch-modulation components: a fast
// capstan/scrape flutter around 5–8 Hz and a slow random drift from belt
// stretch and motor speed. The single mech-wow LFO covers the slow side; this
// adds a faster sine flutter and a noise-driven drift integrator on top, both
// applied to the multi-tap echo read offsets so each repeat wobbles
// authentically. Block-rate update is plenty for these slow signals.
static float    flutter_phase = 0.f;
static float    drift_z       = 0.f;
static uint32_t drift_seed    = 0xCAFEF00Du;

// ── SRE-555 tape playback head bump (low-mid emphasis) ───────────────────────
// Real tape heads have a characteristic +3–5 dB peak around 80–120 Hz from
// gap-loss compensation EQ in the playback preamp. Cheap two-LPF-difference
// trick — 200 Hz minus 50 Hz produces a band-pass centred around 100 Hz; we
// add a scaled copy of that to the dry signal to lift the peak. Mono filter
// because it sits on the pre-chorus multi-head sum. Coefficients are literals
// at 48 kHz (OnePoleCoeff isn't constexpr).
static constexpr float kHeadC1 = 0.02584f;   // 1 - exp(-2π·200/48000)
static constexpr float kHeadC2 = 0.006524f;  // 1 - exp(-2π· 50/48000)
static float head_lp1_z = 0.f;
static float head_lp2_z = 0.f;

// ── SRE-555 tape record-head asymmetric saturation ───────────────────────────
// Mirrors DmmChain::AsymSat — adds 2nd-harmonic skew on top of the symmetric
// tanh feedback clip. Models the tape record stage's level-dependent
// asymmetric saturation, which is what gives heavily-driven tape its "fat"
// character vs. transistor clipping's brittleness. Slow ~20 Hz HPF removes
// the DC offset that x² accumulates so the feedback loop stays bounded.
static constexpr float kTapeAsymA = 0.06f;
static constexpr float kTapeAsymC = 0.002618f;  // 1 - exp(-2π·20/48000)
static float tape_asym_z = 0.f;

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

// ── SDD-555 MORPH lerp tables ────────────────────────────────────────────────
// Both SDD-555 presets share this parameter struct and the DMM-style three-zone
// shape (NE570 preamp always-on; MORPH layers in the rest). The two presets
// differ only in which fields move:
//
//   SDD555_DELAY      (tape_echo=true)  — Drive → +Echo → +Chorus/Verb
//   SDD555_CHORUSVERB (tape_echo=false) — Drive → +Chorus → +Verb
//
// `chorus_wet` crossfades between the dry NE570 mono signal and the chorus
// stereo output — at chorus_wet=0 the chorus is bypassed entirely (no EQ pair,
// no algorithmic processing audible), so the Drive zone is truly clean dirt.
struct SddMorphParams {
  float echo_send;
  float chorus_wet;          // 0 = bypass chorus block, 1 = full chorus output
  float chorus_depth_scale;  // LFO swing intensity (BBD / Dimension D)
  float verb_send;
};

// Delay preset anchors: Drive → Echo (with light chorus/verb) → Ambient wash.
static inline SddMorphParams ComputeDelayMorph(float m) {
  static constexpr SddMorphParams a0 = { 0.0f, 0.0f, 0.0f, 0.0f };
  static constexpr SddMorphParams a1 = { 1.0f, 1.0f, 0.5f, 0.3f };
  static constexpr SddMorphParams a2 = { 1.0f, 1.0f, 1.0f, 1.0f };
  if (m < 0.5f) {
    const float t = m * 2.f;
    return { lerpf(a0.echo_send,          a1.echo_send,          t),
             lerpf(a0.chorus_wet,         a1.chorus_wet,         t),
             lerpf(a0.chorus_depth_scale, a1.chorus_depth_scale, t),
             lerpf(a0.verb_send,          a1.verb_send,          t) };
  }
  const float t = (m - 0.5f) * 2.f;
  return { lerpf(a1.echo_send,          a2.echo_send,          t),
           lerpf(a1.chorus_wet,         a2.chorus_wet,         t),
           lerpf(a1.chorus_depth_scale, a2.chorus_depth_scale, t),
           lerpf(a1.verb_send,          a2.verb_send,          t) };
}

// ChorusVerb preset anchors: Drive → +Chorus (no verb) → +Verb. Echo is always
// disabled in this preset — echo_send stays 0 across the entire sweep.
static inline SddMorphParams ComputeChorusVerbMorph(float m) {
  static constexpr SddMorphParams a0 = { 0.0f, 0.0f, 0.0f, 0.0f };
  static constexpr SddMorphParams a1 = { 0.0f, 1.0f, 1.0f, 0.0f };
  static constexpr SddMorphParams a2 = { 0.0f, 1.0f, 1.0f, 1.0f };
  if (m < 0.5f) {
    const float t = m * 2.f;
    return { 0.f,
             lerpf(a0.chorus_wet,         a1.chorus_wet,         t),
             lerpf(a0.chorus_depth_scale, a1.chorus_depth_scale, t),
             lerpf(a0.verb_send,          a1.verb_send,          t) };
  }
  const float t = (m - 0.5f) * 2.f;
  return { 0.f,
           lerpf(a1.chorus_wet,         a2.chorus_wet,         t),
           lerpf(a1.chorus_depth_scale, a2.chorus_depth_scale, t),
           lerpf(a1.verb_send,          a2.verb_send,          t) };
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
                         float                     bypass_target,
                         bool                      tape_echo);

// ── Footswitch callbacks ───────────────────────────────────────────────────────

static void OnNormalPress(Hothouse::Switches fsw) {
  if (fsw == Hothouse::FOOTSWITCH_2) bypass = !bypass;
}

static void OnDoublePress(Hothouse::Switches fsw) {
  // FS1 double-press toggles fuzz drive. Works in every mode — DMM uses it as
  // a "+12× preamp tanh" override on SW1=UP; SRE-555 modes use it as a "drive
  // the NE570 polynomial harder" override on SW1=UP. The "clean JRC4558 input"
  // constraint for SRE-555 still holds at every other position; fuzz is a
  // deliberate creative override, not an authenticity claim. Tap-tempo would
  // otherwise see the two rising edges as a too-fast tap pair, so cancel it.
  if (fsw == Hothouse::FOOTSWITCH_1) {
    fuzz_enabled = !fuzz_enabled;
    tap.active   = false;
  }
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
  // One-shot Reset latch: zero DSP state exactly once on bypass entry instead
  // of every block. The repeated Reset() bursts (hundreds of float stores at
  // 1 kHz) were drawing current pulses on the rail that coupled into the audio
  // input as a low-level periodic noise during bypass.
  const float bypass_target = bypass ? 0.f : 1.f;
  static bool bypass_state_reset = false;
  if (bypass && bypass_ramp < 0.0001f) {
    if (!bypass_state_reset) {
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
      bypass_state_reset = true;
    }
    // Snap to zero so the early-return reliably trips next block.
    bypass_ramp = 0.f;
    for (size_t i = 0; i < size; ++i) {
      out[0][i] = in[0][i];
      out[1][i] = in[0][i];
    }
    return;
  }
  // Re-arm the latch any time we're active or still ramping.
  bypass_state_reset = false;

  // ── Mode dispatch ───────────────────────────────────────────────────────────
  // The two SDD-555 presets share Sdd555Block — Delay enables the tape echo
  // stage, ChorusVerb skips it. DMM is its own block.
  if (active_mode == ActiveMode::SDD555_DELAY)
    Sdd555Block(in, out, size, p, bypass_target, /*tape_echo=*/true);
  else if (active_mode == ActiveMode::SDD555_CHORUSVERB)
    Sdd555Block(in, out, size, p, bypass_target, /*tape_echo=*/false);
  else
    DmmBlock   (in, out, size, p, bypass_target);
}

// ── Sdd555Block ──────────────────────────────────────────────────────────────
// SDD-555 per-sample loop, shared by both SDD-555 presets:
//   tape_echo = true  → SDD555_DELAY     — drive → echo → chorus → verb
//   tape_echo = false → SDD555_CHORUSVERB — drive → chorus → verb (no echo)
// See AGENTS.md "SDD-555 signal chain" for the full path.
static void Sdd555Block(AudioHandle::InputBuffer  in,
                         AudioHandle::OutputBuffer out,
                         size_t                    size,
                         const BlockInputs&        p,
                         float                     bypass_target,
                         bool                      tape_echo) {
    // SW1 input drive — LINEAR gain into the NE570 (no preamp clip).
    // The real SRE-555 input was a clean JRC4558 op-amp buffer with ~24× headroom
    // at guitar levels; it didn't clip. All program-dependent dirt comes from
    // the NE570's polynomial + the compressor's RMS pumping in Ne570::Compress().
    // Higher drive just pushes the NE570 harder — more polynomial coloration and
    // more compressor pumping, never square-wave saturation.
    //
    // FUZZ override (FS1 double-press): when SW1 is UP and fuzz_enabled, drive
    // is cranked to 5× so the NE570 polynomial dominates and the final tanh
    // output clip squares off — deliberate non-authentic creative mode.
    float sdd_drive;
    if (p.sw1 == Hothouse::TOGGLESWITCH_UP) {
      sdd_drive = fuzz_enabled ? 5.0f : 2.0f;                  // Hot or Fuzz
    } else if (p.sw1 == Hothouse::TOGGLESWITCH_MIDDLE) {
      sdd_drive = 1.0f;                                          // Warm
    } else {
      sdd_drive = 0.5f;                                          // Clean
    }

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

    const SddMorphParams sm = tape_echo
                              ? ComputeDelayMorph     (morph_knob)
                              : ComputeChorusVerbMorph(morph_knob);
    const float echo_fb     = echo_fb_knob * 0.95f;  // hard cap below self-oscillation

    // Echo-related state is only meaningful when tape_echo is enabled. The
    // ChorusVerb preset skips the entire echo stage — buffer is left alone,
    // KNOB_2 and KNOB_3 become inert (they would have been echo time/feedback).
    static constexpr float kSddMaxEchoS = 0.5f;
    static constexpr float kSddMinEchoS = 0.05f;
    float sdd_fb_lpf_c = 0.f;
    if (tape_echo) {
      // Log mapping 50 → 500 ms; tap-tempo override clamped to 500 ms. Tap
      // cancellation is handled by the GetTimeS() call at the top of
      // AudioCallback (its prev_knob_time tracks KNOB_2 raw → seconds), so
      // turning KNOB_2 still clears tap.active here without extra plumbing.
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
      sdd_fb_lpf_c = OnePoleCoeff(6000.f, static_cast<float>(kSampleRate));
    }

    // Chorus rate & depth — preset-dependent. The Delay preset uses the
    // canonical hardware rates fixed per algorithm; ChorusVerb hands rate
    // and depth control to the user via KNOB_2 / KNOB_3 (which are inert
    // otherwise since the echo stage is skipped) and accepts a tap-tempo
    // override on the rate via FS1.
    float chorus_rate_hz;
    float chorus_depth_value;
    if (tape_echo) {
      // Canonical hardware rates per Boss CE-1 / Eventide H910 / Roland SDD-320.
      if      (p.sw2 == Hothouse::TOGGLESWITCH_UP)     chorus_rate_hz = 0.5f;  // CE-1
      else if (p.sw2 == Hothouse::TOGGLESWITCH_MIDDLE) chorus_rate_hz = 0.5f;  // H910 (LFO inert)
      else                                              chorus_rate_hz = 0.3f;  // SDD-320
      chorus_depth_value = sm.chorus_depth_scale;
    } else {
      // ChorusVerb: KNOB_2 = rate 0.1–3 Hz log; KNOB_3 = depth 0–1 (direct).
      // The MORPH-driven chorus_wet still gates the chorus block in/out, so
      // KNOB_3 is the "depth" once chorus is audible — independent of MORPH.
      static constexpr float kCvRateMin = 0.1f;
      static constexpr float kCvRateMax = 3.0f;
      float rate_hz = kCvRateMin
                    * expf(echo_time_knob * logf(kCvRateMax / kCvRateMin));
      if (tap.IsActive() && tap.tempo_s > 0.f) {
        // Tap-tempo override — clamp 1/tap_period into the chorus rate window.
        rate_hz = fmaxf(kCvRateMin, fminf(kCvRateMax, 1.f / tap.tempo_s));
      }
      chorus_rate_hz     = rate_hz;
      chorus_depth_value = echo_fb_knob;   // KNOB_3 raw, 0–1
    }
    // ── Transport drift (advances every block in both SDD-555 presets) ─────
    // Drift integrator runs in both presets because the chorus uses it for
    // rate wandering; the 5 Hz flutter LFO only matters when there's tape, so
    // its phase advance is gated on tape_echo below.
    static constexpr float kDriftLpfC  = 0.000628f;   // ~0.1 Hz at 1 kHz block rate
    static constexpr float kFlutterAmp = 0.0008f;
    static constexpr float kDriftAmp   = 0.002f;
    drift_seed = drift_seed * 1664525u + 1013904223u;
    const float drift_white = static_cast<int32_t>(drift_seed) * (1.f / 2147483648.f);
    drift_z += kDriftLpfC * (drift_white - drift_z);

    float wf_mult = 1.f;
    if (tape_echo) {
      static constexpr float kFlutterHz = 5.f;
      flutter_phase += kFlutterHz * static_cast<float>(size)
                                  / static_cast<float>(kSampleRate);
      if (flutter_phase >= 1.f) flutter_phase -= 1.f;
      wf_mult = 1.f
              + kFlutterAmp * sinf(flutter_phase * kTwoPi)
              + kDriftAmp   * drift_z;
    }

    // Chorus BBD clock drift — ±4% slow rate wander. Audible at the long-rate
    // CE-1 / SDD-320 settings as a "this thing has been on for years" wobble
    // that doesn't lock perfectly to the LFO oscillator.
    const float chorus_drift_mult = 1.f + 0.04f * drift_z;

    chorus.SetRate(chorus_rate_hz * chorus_drift_mult, static_cast<float>(kSampleRate));
    chorus.SetDepth(chorus_depth_value);

    // Verb decay caps. nl_verb (AMS gated / Wildcard resonator) is stable up
    // to 0.85. The cross-coupled 3-spring tank starts feeding back well below
    // that — the four-stage dispersive input chain + 3 springs × 3 allpasses
    // give a long composite loop, so we cap spring more conservatively at 0.65
    // (still feels endless musically, no self-oscillation at KNOB_4=1).
    spring.SetDecay (verb_decay_knob * 0.65f);
    nl_verb.SetDecay(verb_decay_knob * 0.85f);

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

      // ── Tape echo (SRE-555 multi-head echo section) ─────────────────────
      // Real SRE-555 had three (some configurations four) playback heads at
      // fixed positions along the tape path. The user's KNOB_2 sets the
      // longest-head delay; the shorter heads play earlier in time at
      // fractional positions of that delay, producing the unit's signature
      // multi-tap rhythmic pattern that then recirculates as a unit when
      // the longest head feeds back. Levels mirror typical SRE-555 mixer
      // settings — primary head fullest, earlier heads progressively softer.
      // Feedback samples only the longest head so the recursion stays clean.
      // ChorusVerb skips this stage entirely to save 3 SDRAM reads per sample.
      if (tape_echo) {
        const float main_smp = sdd_smooth_echo_smp * wf_mult;
        const float tap1 = delay_line.Read(main_smp * 0.33f);  // head 1 (shortest)
        const float tap2 = delay_line.Read(main_smp * 0.66f);  // head 2 (middle)
        const float tap3 = delay_line.Read(main_smp);          // head 3 (longest, primary)
        sdd_fb_lpf_z += sdd_fb_lpf_c * (tap3 - sdd_fb_lpf_z);
        const float fb_sat = tanhf(sdd_fb_lpf_z * echo_fb);
        // Tape record stage with asymmetric soft saturation. 2nd-harmonic
        // emerges as the input is driven hard; embedded DC blocker keeps the
        // x² term from accumulating in the feedback loop.
        const float record_in   = sig + fb_sat;
        const float record_pre  = record_in + kTapeAsymA * record_in * record_in;
        tape_asym_z += kTapeAsymC * (record_pre - tape_asym_z);
        const float record_sat  = tanhf(record_pre - tape_asym_z);
        delay_line.Write(record_sat);
        const float multi_head = tap3 + 0.6f * tap2 + 0.5f * tap1;
        // Tape playback head bump: +4 dB peak around 100 Hz via two-LPF
        // difference. kHead*Coeff are derived in main() from 200 Hz / 50 Hz.
        head_lp1_z += kHeadC1 * (multi_head - head_lp1_z);
        head_lp2_z += kHeadC2 * (multi_head - head_lp2_z);
        const float head_bumped = multi_head + 0.6f * (head_lp1_z - head_lp2_z);
        sig = sig + head_bumped * sm.echo_send;
      }

      // Pre-chorus mono signal — used as the dry-drive reference when MORPH
      // is in the Drive zone (chorus_wet=0), so the chorus is bypassed
      // entirely and only NE570 dirt is heard.
      const float drive_mono = sig;

      // Chorus algorithm (SW2). Branch on block-rate constant — predictor handles cleanly.
      float wetL, wetR;
      if      (p.sw2 == Hothouse::TOGGLESWITCH_UP)     chorus.ProcessBbd       (sig, wetL, wetR);
      else if (p.sw2 == Hothouse::TOGGLESWITCH_MIDDLE) chorus.ProcessEventide  (sig, wetL, wetR);
      else                                              chorus.ProcessDimensionD(sig, wetL, wetR);

      // Crossfade chorus output toward dry NE570 mono as MORPH approaches the
      // Drive zone. At chorus_wet=0 the chorus block is effectively bypassed.
      wetL = lerpf(drive_mono, wetL, sm.chorus_wet);
      wetR = lerpf(drive_mono, wetR, sm.chorus_wet);

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

      // Emergency runaway latch — should never fire with decay capped at 0.85,
      // but if the verb structure ever diverges (e.g. NaN injection from input),
      // Reset within the same block instead of locking the audio path.
      if (fabsf(verb_l) > 4.f || fabsf(verb_r) > 4.f) {
        if      (p.sw3 == Hothouse::TOGGLESWITCH_DOWN) spring .Reset();
        else                                            nl_verb.Reset();
        verb_l = verb_r = 0.f;
      }

      // MORPH-scaled verb send — at morph=0 the chorus path is dry, at
      // morph=1 the verb is at full return level.
      float wet_l = aged_l + verb_l * sm.verb_send;
      float wet_r = aged_r + verb_r * sm.verb_send;

      // NaN/inf guard mirrors DMM's pattern — a runaway verb structure can
      // propagate non-finite values that would otherwise lock the audio path.
      if (!std::isfinite(wet_l)) wet_l = 0.f;
      if (!std::isfinite(wet_r)) wet_r = 0.f;

      // Soft tanh saturation on the wet sum. The pre/post gain (0.85 / 1.176)
      // gives unity in the linear region — musical settings sound identical
      // to today; only signals above ~−2 dBFS get a 2nd/3rd-harmonic compression
      // instead of digital clip. Mirrors the DMM output-stage philosophy.
      wet_l = tanhf(wet_l * 0.85f) * 1.176f;
      wet_r = tanhf(wet_r * 0.85f) * 1.176f;

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
  // When SW1 is UP and fuzz_enabled is set (FS1 double-press toggle), the Hot
  // position turns into a fourth "Fuzz" level — preamp gain pushed deep into
  // tanh saturation for the squared-off corners of a fuzz pedal.
  float preamp_gain, post_gain;
  if      (p.sw1 == Hothouse::TOGGLESWITCH_UP) {
    if (fuzz_enabled) { preamp_gain = 30.0f; post_gain = 0.55f; }
    else              { preamp_gain = 12.0f; post_gain = 1.0f;  }
  }
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

  // Slow random drift on the BBD clock — ±0.3% red noise at block rate.
  // Same xorshift-flavored LCG + one-pole as the SRE-555 flutter integrator.
  // Multiplied into base_delay_smp below so the drift scales proportionally
  // (audible at long times, vanishingly small at short).
  dmm_drift_seed = dmm_drift_seed * 1664525u + 1013904223u;
  const float dmm_drift_white = static_cast<int32_t>(dmm_drift_seed) * (1.f / 2147483648.f);
  dmm_drift_z += 0.000628f * (dmm_drift_white - dmm_drift_z);
  const float dmm_drift_mult = 1.f + 0.003f * dmm_drift_z;

  const float base_delay_smp = p.time_s * static_cast<float>(kSampleRate) * dmm_drift_mult;

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
    // Pre-emphasis sits between the tone filter and the BBD bandwidth LPF so
    // the BBD sees a treble-lifted signal above its noise floor; de-emphasis
    // after AiFilter restores flat response and pulls the BBD noise back down.
    // AsymSat after the BBD bandwidth LPF adds 2nd-harmonic asymmetry to the
    // already band-limited signal — models the BBD chip's program-dependent
    // soft compression on top of the symmetric tanh feedback clip. Noise()
    // injects the BBD chip's stationary noise floor (-65 dBFS) so the expander
    // downstream has something to duck during quiet passages → "breathing".
    const float pre_emp_in = dmm.PreEmph(delay_input);
    const float bbd_lim    = dmm.BbdFilter(pre_emp_in, bbd_c);
    const float bbd_sat    = dmm.AsymSat(bbd_lim) + dmm.Noise();
    delay_line.Write(bbd_sat + fb_sat);

    const float ai_out   = dmm.AiFilter(delay_out);
    const float expanded = dmm.Expand(dmm.DeEmph(ai_out));

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
  // PWM frequency 8 kHz — above the input LPF and the chorus de-emphasis
  // shelf at 3 kHz. Keeping the LED GPIO PWM out of the audio band prevents
  // capacitive coupling into the analog input from being audible.
  led_bypass.Init(hw.seed.GetPin(Hothouse::LED_2), false, 8000.f);
  led_freeze.Init(hw.seed.GetPin(Hothouse::LED_1), false, 8000.f);

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
    // Two combo patterns target different SDD-555 presets. Each combo is a
    // direct toggle with DMM — entering the same combo a second time from the
    // target preset returns to DMM. Switching directly between the two SDD-555
    // presets requires routing through DMM (hit either combo, then the other).
    static uint32_t   fs1_hold_start    = 0;
    static bool       fs1_was_held      = false;
    static uint32_t   mode_combo_start  = 0;
    static bool       mode_combo_fired  = false;
    static ActiveMode mode_combo_target = ActiveMode::DMM;  // sentinel: no combo

    const bool fs1_held = hw.switches[Hothouse::FOOTSWITCH_1].Pressed();
    const bool fs2_held = hw.switches[Hothouse::FOOTSWITCH_2].Pressed();

    if (fs1_held && !fs1_was_held)
      fs1_hold_start = now;

    const auto sw1 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_1);
    const auto sw2 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_2);
    const auto sw3 = hw.GetToggleswitchPosition(Hothouse::TOGGLESWITCH_3);
    // Each SDD-555 preset has its own unique toggle pattern. The all-DOWN
    // pattern is now reserved for the 10 s DFU bootloader hold (FS1 only).
    const bool toggles_all_down =
        sw1 == Hothouse::TOGGLESWITCH_DOWN &&
        sw2 == Hothouse::TOGGLESWITCH_DOWN &&
        sw3 == Hothouse::TOGGLESWITCH_DOWN;
    const bool toggles_delay =
        sw1 == Hothouse::TOGGLESWITCH_UP   &&
        sw2 == Hothouse::TOGGLESWITCH_DOWN &&
        sw3 == Hothouse::TOGGLESWITCH_DOWN;
    const bool toggles_chorusverb =
        sw1 == Hothouse::TOGGLESWITCH_DOWN &&
        sw2 == Hothouse::TOGGLESWITCH_UP   &&
        sw3 == Hothouse::TOGGLESWITCH_DOWN;
    const bool mix_zero = hw.GetKnobValue(Hothouse::KNOB_6) < 0.02f;

    // Which (if any) preset does the current toggle pattern target?
    ActiveMode combo_target = ActiveMode::DMM;  // sentinel "no valid combo"
    bool       combo_valid  = false;
    if (mix_zero && fs1_held && fs2_held) {
      if      (toggles_delay)      { combo_target = ActiveMode::SDD555_DELAY;     combo_valid = true; }
      else if (toggles_chorusverb) { combo_target = ActiveMode::SDD555_CHORUSVERB; combo_valid = true; }
    }

    // Restart the 3 s timer whenever the combo breaks OR the target changes
    // mid-hold (so a toggle slide between patterns can't accumulate time).
    if (!combo_valid || combo_target != mode_combo_target) {
      mode_combo_start  = now;
      mode_combo_fired  = false;
      mode_combo_target = combo_target;
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
      // Direct toggle with DMM — hit the target combo twice to round-trip back.
      active_mode   = (active_mode == combo_target) ? ActiveMode::DMM : combo_target;
      // Zero the shared SDRAM delay line — DMM's BBD content and SDD-555's
      // tape echo content are both stored here, and neither mode should hear
      // the other's residue when it takes over. ~384 KB SDRAM write — a few
      // milliseconds during the LED blink, imperceptible.
      delay_line.Init();
      sdd_fb_lpf_z = 0.f;
      smooth_delay_smp     = 2400.f;
      sdd_smooth_echo_smp  = 2400.f;
      // PitchShifter is shared between DMM shimmer (+12 st octave, fun=0.3)
      // and SDD-555 Eventide H910 micro-pitch (+0.20 st ≈ +12 cents, fun=0).
      // The H910 manual reference is ±7c dual-shifter; with a single shared
      // shifter and the +0.07 st value, the effect was inaudible on guitar
      // — bumped to +0.20 st to register as a clearly audible micro-pitch
      // widening while still being subtler than a chorus.
      if (IsSdd555(active_mode)) {
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
    if (fs1_held && !fs2_held && toggles_all_down && mix_zero && (now - fs1_hold_start) >= 10000) {
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
