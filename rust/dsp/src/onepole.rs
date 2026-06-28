//! One-pole low-pass primitive, matching the C++ `OnePoleCoeff` /
//! `z += c*(x - z)` convention used throughout the Echorec/Memory Morph DSP.

use crate::TWO_PI;
use libm::expf;

/// One-pole smoothing coefficient for cutoff `fc` Hz at sample rate `fs` Hz.
/// `c = 1 - e^(-2π·fc/fs)` — identical to the C++ `OnePoleCoeff`.
#[inline]
pub fn one_pole_coeff(fc: f32, fs: f32) -> f32 {
    1.0 - expf(-TWO_PI * fc / fs)
}

/// One-pole low-pass filter using the leaky-integrator form `z += c*(x - z)`.
#[derive(Clone, Copy)]
pub struct OnePole {
    c: f32,
    z: f32,
}

impl OnePole {
    #[inline]
    pub fn new(fc: f32, fs: f32) -> Self {
        Self {
            c: one_pole_coeff(fc, fs),
            z: 0.0,
        }
    }

    /// Change the cutoff in place (preserves state).
    #[inline]
    pub fn set_cutoff(&mut self, fc: f32, fs: f32) {
        self.c = one_pole_coeff(fc, fs);
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        self.z += self.c * (x - self.z);
        self.z
    }

    #[inline]
    pub fn reset(&mut self) {
        self.z = 0.0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coeff_in_unit_range_and_monotonic() {
        let fs = 96_000.0;
        let lo = one_pole_coeff(100.0, fs);
        let hi = one_pole_coeff(10_000.0, fs);
        assert!(lo > 0.0 && lo < 1.0);
        assert!(hi > 0.0 && hi < 1.0);
        assert!(hi > lo, "higher cutoff => larger coefficient");
    }

    #[test]
    fn settles_to_dc_input() {
        let mut lp = OnePole::new(1_000.0, 96_000.0);
        let mut y = 0.0;
        for _ in 0..20_000 {
            y = lp.process(1.0);
        }
        assert!((y - 1.0).abs() < 1e-3, "LPF should settle to DC input, got {y}");
    }
}
