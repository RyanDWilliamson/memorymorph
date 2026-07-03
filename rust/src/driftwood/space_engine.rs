//! SPACE engine — the Footswitch-2 side: lo-fi PT2399 reverb, voiced by
//! TOGGLE_3 ([`SpaceMode`]).
//!
//! - **Dark** — murky, dub/cave (tone pulled down, light modulation).
//! - **Modulated** — seasick wash (deep comb modulation).
//! - **Shimmer** — an octave-up tail folded into the reverb feedback.
//!
//! An outer regeneration loop (SPACE "regen" knob) feeds the wet tail back into
//! the tank; FS1-hold **bloom** pushes decay and regen toward self-oscillation,
//! held in check by the reverb's internal soft clip and the engine's output
//! limiter.

use dsp::pitch::OctaveUp;
use dsp::pt2399_reverb::Pt2399Reverb;

use super::params::{Params, SpaceMode};

/// Outer-loop regeneration when bloom (FS1-hold) is engaged.
const BLOOM_REGEN: f32 = 0.85;

pub struct SpaceEngine {
    reverb: Pt2399Reverb,
    shimmer: OctaveUp,
    mode: SpaceMode,
    mix: f32,
    regen: f32,
    shimmer_amt: f32,
    last_tail: f32,
}

impl SpaceEngine {
    /// SDRAM the reverb network needs at sample rate `fs`.
    pub fn reverb_len(fs: f32) -> usize {
        Pt2399Reverb::required_len(fs)
    }

    pub fn new(fs: f32, reverb_buf: &'static mut [f32], shimmer_buf: &'static mut [f32]) -> Self {
        Self {
            reverb: Pt2399Reverb::new(fs, reverb_buf),
            shimmer: OctaveUp::new(shimmer_buf),
            mode: SpaceMode::Modulated,
            mix: 0.3,
            regen: 0.0,
            shimmer_amt: 0.0,
            last_tail: 0.0,
        }
    }

    /// Map parameters once per block; `bloom` = FS1-hold.
    pub fn set_params(&mut self, p: &Params, bloom: bool) {
        self.mode = p.space_mode;
        self.mix = p.space_mix();

        self.reverb.set_decay(if bloom { 1.0 } else { p.space_decay() });
        self.reverb.set_age(p.space_age());

        // Character toggle shapes tone/modulation and enables shimmer.
        let (tone_scale, mod_scale, shimmer_on) = match self.mode {
            SpaceMode::Dark => (0.45, 0.4, false),
            SpaceMode::Modulated => (0.8, 1.0, false),
            SpaceMode::Shimmer => (0.9, 0.6, true),
        };
        self.reverb
            .set_tone((p.space_tone() * tone_scale).clamp(0.0, 1.0));
        self.reverb
            .set_mod((p.space_mod() * mod_scale).clamp(0.0, 1.0));
        self.reverb.set_rate(0.3 + p.space_mod() * 0.8);

        // Audit: the outer regen loop multiplies with the reverb's internal
        // comb feedback; near decay resonance the product exceeds unity and the
        // reverb self-sustains at moderate knob settings. Bound the outer loop
        // decay-aware — less outer regen headroom as the internal tail grows.
        let regen = if bloom {
            BLOOM_REGEN
        } else {
            0.5 * p.space_regen() * (1.0 - 0.6 * p.space_decay())
        };
        self.regen = regen.clamp(0.0, BLOOM_REGEN);
        self.shimmer_amt = if shimmer_on {
            0.5 * (0.4 + 0.6 * p.space_regen())
        } else {
            0.0
        };
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
        let send = x * send_gain + self.last_tail * self.regen;
        let wet = self.reverb.process(send, inject);
        self.last_tail = wet;
        x * (1.0 - self.mix) + wet * self.mix
    }
}
