//! Lo-fi "junk" — tape hiss and random amplitude dropouts. Ported from the
//! Echorec `Noise()` / `Dropout()` / `SetAgeLofi()` (echorec-refinements branch).
//!
//! Both use the same LCG (`x = x·1664525 + 1013904223`) as the C++ — a cheap,
//! deterministic PRNG that gives the firmware reproducible grit.

use crate::one_pole_coeff;

const LCG_MUL: u32 = 1_664_525;
const LCG_ADD: u32 = 1_013_904_223;

/// Tape hiss: scaled full-range white noise from the LCG.
pub struct Hiss {
    seed: u32,
    amt: f32,
}

impl Default for Hiss {
    fn default() -> Self {
        Self::new()
    }
}

impl Hiss {
    pub const fn new() -> Self {
        Self {
            seed: 0x1B0C_A7ED,
            amt: 0.0,
        }
    }

    /// Hiss level (the C++ uses ~0.005 at full Age — keep it small).
    pub fn set_amount(&mut self, amt: f32) {
        self.amt = amt;
    }

    #[inline]
    pub fn process(&mut self) -> f32 {
        self.seed = self.seed.wrapping_mul(LCG_MUL).wrapping_add(LCG_ADD);
        (self.seed as i32 as f32) * (1.0 / 2_147_483_648.0) * self.amt
    }
}

/// Random amplitude dropouts: occasionally gates the signal to zero with a
/// smoothed (~2 ms) gate, simulating worn tape. `process` returns a 0..1 gain
/// to multiply the output by.
pub struct Dropout {
    gate_c: f32,
    env: f32,
    prob: f32, // per-sample trip probability (Age²)
    len: i32,  // dropout length in samples
    count: i32,
    dropping: bool,
    seed: u32,
}

impl Dropout {
    pub fn new(fs: f32) -> Self {
        Self {
            gate_c: one_pole_coeff(80.0, fs), // ~2 ms gate slew
            env: 1.0,
            prob: 0.0,
            len: 0,
            count: 0,
            dropping: false,
            seed: 0xBEEF_1234,
        }
    }

    /// Age 0..1 → dropout rate and length. Age² so low Age stays clean.
    /// At Age=1: ~3.8 trips/s, ~22–45 ms each (matches the C++ `SetAgeLofi`).
    pub fn set_age(&mut self, age: f32, fs: f32) {
        let a = age.clamp(0.0, 1.0);
        self.prob = a * a * 0.000_04;
        self.len = (0.030 * fs * (0.5 + a)) as i32;
    }

    #[inline]
    pub fn process(&mut self) -> f32 {
        if self.prob > 0.0 {
            self.seed = self.seed.wrapping_mul(LCG_MUL).wrapping_add(LCG_ADD);
            let r = (self.seed >> 8) as f32 * (1.0 / 16_777_216.0); // 0..1
            if !self.dropping && r < self.prob {
                self.dropping = true;
                self.count = self.len;
            }
            if self.dropping {
                self.count -= 1;
                if self.count <= 0 {
                    self.dropping = false;
                }
            }
        }
        let target = if self.dropping { 0.0 } else { 1.0 };
        self.env += self.gate_c * (target - self.env);
        self.env
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const FS: f32 = 96_000.0;

    #[test]
    fn hiss_silent_at_zero_amount() {
        let mut h = Hiss::new();
        for _ in 0..1000 {
            assert_eq!(h.process(), 0.0);
        }
    }

    #[test]
    fn hiss_bounded_by_amount() {
        let mut h = Hiss::new();
        h.set_amount(0.01);
        let mut peak = 0.0_f32;
        let mut nonzero = false;
        for _ in 0..100_000 {
            let v = h.process();
            peak = peak.max(v.abs());
            nonzero |= v != 0.0;
        }
        assert!(nonzero, "hiss should produce signal");
        assert!(peak <= 0.01, "hiss must stay within amount, got {peak}");
    }

    #[test]
    fn dropout_open_when_clean() {
        let mut d = Dropout::new(FS);
        d.set_age(0.0, FS);
        for _ in 0..96_000 {
            assert_eq!(d.process(), 1.0, "Age=0 must never drop out");
        }
    }

    #[test]
    fn dropout_dips_when_aged() {
        let mut d = Dropout::new(FS);
        d.set_age(1.0, FS);
        let mut min_gain = 1.0_f32;
        // ~10 s of audio — plenty of time for several trips.
        for _ in 0..960_000 {
            min_gain = min_gain.min(d.process());
        }
        assert!(
            min_gain < 0.5,
            "Age=1 should produce audible dropouts, min gain was {min_gain}"
        );
    }
}
