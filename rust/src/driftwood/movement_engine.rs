//! MOVEMENT engine — routes the internal LFO to one of three targets (MASTER
//! page): amplitude (tremolo / harmonic tremolo), time (vibrato into the TIME
//! delay), or space (swell into the SPACE reverb send).
//!
//! Advance the LFO once per audio frame with [`tick`], then read [`vib`] /
//! [`send_gain`] and apply [`apply_amp`].
//!
//! [`tick`]: MovementEngine::tick
//! [`vib`]: MovementEngine::vib
//! [`send_gain`]: MovementEngine::send_gain
//! [`apply_amp`]: MovementEngine::apply_amp

use dsp::movement::{HarmonicTrem, Lfo, Shape};
use libm::powf;

use super::params::{MoveTarget, Params};

const MIN_RATE_HZ: f32 = 0.1;
const MAX_RATE_HZ: f32 = 12.0;
/// Vibrato depth (fraction of delay time) at full movement depth.
const VIB_DEPTH: f32 = 0.03;

pub struct MovementEngine {
    lfo: Lfo,
    htrem: HarmonicTrem,
    target: MoveTarget,
    shape: Shape,
    /// Depth target (knob) and per-sample smoothed value (zipper-free K2).
    depth: f32,
    depth_s: f32,
    cur: f32, // last unipolar LFO value [0,1]
    /// Rate-knob value the LFO rate was last computed for (powf memoization).
    last_rate_knob: f32,
}

impl MovementEngine {
    pub fn new(fs: f32) -> Self {
        Self {
            lfo: Lfo::new(fs),
            htrem: HarmonicTrem::new(fs),
            target: MoveTarget::Amplitude,
            shape: Shape::Sine,
            depth: 0.0,
            depth_s: 0.0,
            cur: 0.5,
            last_rate_knob: f32::NAN, // force first computation
        }
    }

    pub fn set_params(&mut self, p: &Params) {
        let rate_knob = p.movement_rate();
        if rate_knob != self.last_rate_knob {
            self.last_rate_knob = rate_knob;
            let rate = MIN_RATE_HZ * powf(MAX_RATE_HZ / MIN_RATE_HZ, rate_knob);
            self.lfo.set_rate(rate);
        }
        self.shape = Shape::from_knob(p.movement_shape());
        self.lfo.set_shape(self.shape);
        self.depth = p.movement_depth();
        self.target = p.movement_target();
    }

    /// Restart the LFO cycle (tap-sync / re-trigger).
    pub fn reset(&mut self) {
        self.lfo.reset();
    }

    /// Advance the LFO one sample. Call once per frame before the readers below.
    #[inline]
    pub fn tick(&mut self) {
        self.cur = self.lfo.tick();
        self.depth_s += 0.001 * (self.depth - self.depth_s); // zipper-free depth
    }

    /// Vibrato fraction for the TIME delay read (±); 0 unless target == Time.
    #[inline]
    pub fn vib(&self) -> f32 {
        if self.target == MoveTarget::Time {
            (self.cur * 2.0 - 1.0) * self.depth_s * VIB_DEPTH
        } else {
            0.0
        }
    }

    /// Reverb send gain for the swell; 1.0 unless target == Space.
    #[inline]
    pub fn send_gain(&self) -> f32 {
        if self.target == MoveTarget::Space {
            1.0 - self.depth_s + self.depth_s * self.cur
        } else {
            1.0
        }
    }

    /// Apply amplitude movement (plain or harmonic tremolo) to the final signal.
    #[inline]
    pub fn apply_amp(&mut self, x: f32) -> f32 {
        if self.target != MoveTarget::Amplitude || self.depth_s <= 0.0 {
            return x;
        }
        if self.shape == Shape::Harmonic {
            self.htrem.process(x, self.cur, self.depth_s)
        } else {
            x * (1.0 - self.depth_s + self.depth_s * self.cur)
        }
    }
}
