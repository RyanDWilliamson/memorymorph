//! SPACE engine — the Footswitch-2 side: lo-fi PT2399 reverb, voiced by
//! TOGGLE_3 ([`SpaceMode`]).
//!
//! - **Dark** — murky, dub/cave (tone pulled down, light modulation).
//! - **Modulated** — seasick wash (deep comb modulation).
//! - **Shimmer** — an octave-up tail folded into the reverb feedback.
//!
//! Regeneration is **internal**: the regen knob raises the reverb's own comb
//! feedback toward (but never past) unity, which is unconditionally stable —
//! the comb feedback path is damped and passes a soft clip. FS1-hold **bloom**
//! pushes it just past unity into a bounded, musical self-oscillation.
//!
//! An earlier design recirculated the wet tail through an *outer* loop around
//! the whole network. That topology is inherently unstable: its true loop gain
//! is `regen × the network's resonant gain` (5–30× near decay resonance), so it
//! either ran away to inf → NaN-latched silence (unclipped) or a permanent
//! full-scale scream (clipped). Bench-confirmed both. Don't reintroduce it.

use dsp::fastmath::knee_clip;
use dsp::pitch::OctaveUp;
use dsp::pt2399_reverb::Pt2399Reverb;

use super::params::{Params, SpaceMode};

/// Internal comb feedback during bloom (FS1-hold): just past unity, bounded by
/// the clip inside the comb feedback path.
const BLOOM_FB: f32 = 1.01;
/// Comb feedback ceiling reachable with the regen knob (strictly stable).
const REGEN_FB_MAX: f32 = 0.995;

pub struct SpaceEngine {
    reverb: Pt2399Reverb,
    shimmer: OctaveUp,
    /// Mix target (knob) and its per-sample smoothed value (zipper-free K5).
    mix: f32,
    mix_s: f32,
    shimmer_amt: f32,
    last_tail: f32,
}

impl SpaceEngine {
    /// Buffer length the reverb network needs at sample rate `fs`.
    pub fn reverb_len(fs: f32) -> usize {
        Pt2399Reverb::required_len(fs)
    }

    pub fn new(fs: f32, reverb_buf: &'static mut [f32], shimmer_buf: &'static mut [f32]) -> Self {
        Self {
            reverb: Pt2399Reverb::new(fs, reverb_buf),
            shimmer: OctaveUp::new(shimmer_buf),
            mix: 0.3,
            mix_s: 0.3,
            shimmer_amt: 0.0,
            last_tail: 0.0,
        }
    }

    /// Map parameters once per block. `p.freeze` (FS1-hold) blooms the reverb —
    /// read directly from `Params` so TIME-freeze and SPACE-bloom can never be
    /// driven apart by a caller.
    pub fn set_params(&mut self, p: &Params) {
        let bloom = p.freeze;
        let mode = p.space_mode;
        self.mix = p.space_mix();
        self.reverb.set_age(p.space_age());

        // Regeneration = internal comb feedback. K1 (decay) sets the base tail;
        // K2 (regen) closes the remaining gap toward REGEN_FB_MAX, never past.
        let base = 0.70 + 0.29 * p.space_decay();
        let fb = if bloom {
            BLOOM_FB
        } else {
            base + p.space_regen() * (REGEN_FB_MAX - base)
        };
        self.reverb.set_feedback(fb);

        // Character toggle shapes tone/modulation and enables shimmer.
        let (tone_scale, mod_scale, shimmer_on) = match mode {
            SpaceMode::Dark => (0.45, 0.4, false),
            SpaceMode::Modulated => (0.8, 1.0, false),
            SpaceMode::Shimmer => (0.9, 0.6, true),
        };
        self.reverb
            .set_tone((p.space_tone() * tone_scale).clamp(0.0, 1.0));
        self.reverb
            .set_mod((p.space_mod() * mod_scale).clamp(0.0, 1.0));
        self.reverb.set_rate(0.3 + p.space_mod() * 0.8);

        // Shimmer feeds the pitch-shifted tail back through the chip input.
        // The tank amplifies any injection by its resonant gain ~1/(1-fb), so
        // the injection budget must scale with the remaining headroom (1-fb):
        // loop gain stays roughly constant and below unity across the whole
        // regen range (host regression test covers the worst case). At bloom
        // (fb > 1) the headroom is zero — bloom is pure tank self-oscillation.
        self.shimmer_amt = if shimmer_on {
            ((0.3 + 0.7 * p.space_regen()) * 0.5 * (1.0 - fb)).clamp(0.0, 0.08)
        } else {
            0.0
        };
        // Belt-and-braces: if the tail state was ever poisoned, recover instead
        // of latching silent.
        if !self.last_tail.is_finite() {
            self.last_tail = 0.0;
        }
    }

    /// `send_gain` scales the dry signal into the tank (MOVEMENT swell, space
    /// target); 1.0 when movement is off or targeting something else.
    #[inline]
    pub fn process(&mut self, x: f32, send_gain: f32) -> f32 {
        let inject = if self.shimmer_amt > 0.0 {
            self.shimmer.process(self.last_tail) * self.shimmer_amt
        } else {
            0.0
        };
        // Soft-clip the send so a hot TIME stage can't slam the comb inputs.
        // There is no recirculation here — regeneration lives inside the
        // reverb's comb feedback (see module docs).
        let send = knee_clip(x * send_gain, 0.75);
        let wet = self.reverb.process(send, inject);
        self.last_tail = wet;
        self.mix_s += 0.001 * (self.mix - self.mix_s); // zipper-free mix knob
        x * (1.0 - self.mix_s) + wet * self.mix_s
    }
}
