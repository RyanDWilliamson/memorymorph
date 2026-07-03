//! MOVEMENT — the internal modulation source that animates Driftwood (Mood's
//! "movement" without an expression jack).
//!
//! Two pieces:
//! - [`Lfo`] — a phase oscillator with selectable [`Shape`], producing a
//!   unipolar `[0,1]` value (for amplitude/swell depth) and a bipolar `[-1,1]`
//!   value (for pitch/time vibrato). Rate is settable and the phase can be
//!   reset for tap-sync.
//! - [`HarmonicTrem`] — a modeled harmonic (phase-shift) tremolo: it splits the
//!   signal at a crossover and amplitude-modulates the low and high bands in
//!   antiphase, the hollow vowel-y sound of an optical/harmonic trem, rather
//!   than a plain amplitude wobble.
//!
//! The engine decides what the movement *targets* (amplitude = tremolo, time =
//! vibrato, space = swell); this module only generates the modulation.
//!
//! Pure `libm` math; host-tested under `cargo test`.

use crate::fastmath;
use crate::onepole::OnePole;

/// Movement waveform. `Harmonic` selects the harmonic-tremolo voice (driven by a
/// sine); the others are plain shapes the engine applies to its target.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Shape {
    Sine,
    Harmonic,
    Square,
    Ramp,
}

impl Shape {
    /// Map a 0..1 knob to a shape (MASTER K3).
    pub fn from_knob(k: f32) -> Self {
        match (k.clamp(0.0, 1.0) * 4.0) as u32 {
            0 => Shape::Sine,
            1 => Shape::Harmonic,
            2 => Shape::Square,
            _ => Shape::Ramp,
        }
    }
}

pub struct Lfo {
    fs: f32,
    phase: f32, // [0,1)
    inc: f32,
    shape: Shape,
}

impl Lfo {
    pub fn new(fs: f32) -> Self {
        Self {
            fs,
            phase: 0.0,
            inc: 1.0 / fs,
            shape: Shape::Sine,
        }
    }

    pub fn set_rate(&mut self, hz: f32) {
        self.inc = hz.max(0.0) / self.fs;
    }

    pub fn set_shape(&mut self, shape: Shape) {
        self.shape = shape;
    }

    /// Restart the cycle (tap-sync / re-trigger).
    pub fn reset(&mut self) {
        self.phase = 0.0;
    }

    /// Advance one sample and return the unipolar value in `[0,1]`.
    #[inline]
    pub fn tick(&mut self) -> f32 {
        let v = self.shape_value(self.phase);
        self.phase += self.inc;
        if self.phase >= 1.0 {
            self.phase -= 1.0;
        }
        v
    }

    /// Same advance, but bipolar `[-1,1]` (for vibrato/pitch targets).
    #[inline]
    pub fn tick_bipolar(&mut self) -> f32 {
        self.tick() * 2.0 - 1.0
    }

    #[inline]
    fn shape_value(&self, phase: f32) -> f32 {
        match self.shape {
            // Harmonic uses a sine carrier; the band-split lives in HarmonicTrem.
            Shape::Sine | Shape::Harmonic => 0.5 + 0.5 * fastmath::sin_01(phase),
            Shape::Square => {
                if phase < 0.5 {
                    1.0
                } else {
                    0.0
                }
            }
            Shape::Ramp => phase, // rising sawtooth 0..1
        }
    }
}

/// Harmonic (phase-shift) tremolo: low and high bands modulated in antiphase.
pub struct HarmonicTrem {
    crossover: OnePole,
}

impl HarmonicTrem {
    pub fn new(fs: f32) -> Self {
        Self {
            // ~800 Hz split, the classic harmonic-trem crossover region.
            crossover: OnePole::new(800.0, fs),
        }
    }

    /// `lfo` is a unipolar `[0,1]` carrier; `depth` `[0,1]` blends dry→full.
    /// Low band gets gain `lfo`, high band gets `1-lfo` (antiphase).
    #[inline]
    pub fn process(&mut self, x: f32, lfo: f32, depth: f32) -> f32 {
        let low = self.crossover.process(x);
        let high = x - low;
        let g = lfo.clamp(0.0, 1.0);
        let wet = low * g + high * (1.0 - g);
        let d = depth.clamp(0.0, 1.0);
        x * (1.0 - d) + wet * d
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FS: f32 = 96_000.0;

    #[test]
    fn sine_is_unipolar_and_centred() {
        let mut lfo = Lfo::new(FS);
        lfo.set_rate(5.0);
        let mut sum = 0.0f64;
        let mut lo = f32::MAX;
        let mut hi = f32::MIN;
        let n = (FS / 5.0) as usize; // one period
        for _ in 0..n {
            let v = lfo.tick();
            assert!((0.0..=1.0).contains(&v));
            sum += v as f64;
            lo = lo.min(v);
            hi = hi.max(v);
        }
        let mean = sum / n as f64;
        assert!((mean - 0.5).abs() < 0.02, "sine mean ~0.5, got {mean}");
        assert!(lo < 0.05 && hi > 0.95, "sine should span [0,1]");
    }

    #[test]
    fn square_is_two_valued() {
        let mut lfo = Lfo::new(FS);
        lfo.set_rate(2.0);
        lfo.set_shape(Shape::Square);
        for _ in 0..5000 {
            let v = lfo.tick();
            assert!(v == 0.0 || v == 1.0, "square must be 0/1, got {v}");
        }
    }

    #[test]
    fn ramp_rises_then_wraps() {
        let mut lfo = Lfo::new(FS);
        lfo.set_rate(1.0);
        lfo.set_shape(Shape::Ramp);
        let mut prev = lfo.tick();
        let mut wraps = 0;
        for _ in 0..(FS as usize * 2) {
            let v = lfo.tick();
            if v + 1e-6 < prev {
                wraps += 1; // sawtooth reset
            }
            prev = v;
        }
        assert!(wraps >= 1, "ramp should wrap at least once over 2 s");
    }

    #[test]
    fn rate_sets_frequency() {
        // ~5 Hz → ~5 full cycles per second; count rising zero-crossings of the
        // bipolar sine.
        let mut lfo = Lfo::new(FS);
        lfo.set_rate(5.0);
        let mut prev = lfo.tick_bipolar();
        let mut rising = 0;
        for _ in 0..(FS as usize) {
            let v = lfo.tick_bipolar();
            if prev < 0.0 && v >= 0.0 {
                rising += 1;
            }
            prev = v;
        }
        assert!((4..=6).contains(&rising), "expected ~5 cycles, got {rising}");
    }

    #[test]
    fn harmonic_trem_is_dry_at_zero_depth_and_bounded() {
        let mut ht = HarmonicTrem::new(FS);
        let mut maxy = 0.0f32;
        for n in 0..4000 {
            let x = libm::sinf(core::f32::consts::TAU * 440.0 * n as f32 / FS);
            let dry = ht.process(x, 0.3, 0.0);
            assert!((dry - x).abs() < 1e-5, "depth=0 must be dry");
            let lfo = libm::fabsf(libm::sinf(n as f32 * 0.01));
            let wet = ht.process(x, lfo, 1.0);
            assert!(wet.is_finite(), "non-finite harmonic-trem output");
            maxy = maxy.max(wet.abs());
        }
        // Antiphase band modulation can exceed the input peak; just stay sane.
        assert!(maxy < 2.5, "harmonic trem should stay bounded, peak={maxy}");
    }
}
