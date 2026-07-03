//! Wow + flutter modulation source for the cassette delay line.
//!
//! Real tape transport instability is a slow "wow" (~0.5–1.5 Hz) plus a faster
//! "flutter" (~6–11 Hz). This produces a combined modulation value in roughly
//! [-1, 1] scaled by `amount`; the firmware multiplies it by the base delay to
//! get a fractional delay-time offset (Memory Morph modulates ±1.5% — we widen
//! that as `amount` rises for the "wild" end). This is the modulation *source*;
//! the delay line itself lives in the firmware (SDRAM).

use crate::fastmath;

pub struct Warble {
    wow_phase: f32,
    wow_inc: f32,
    flutter_phase: f32,
    flutter_inc: f32,
    amount: f32,
}

impl Warble {
    pub fn new(fs: f32) -> Self {
        Self {
            wow_phase: 0.0,
            wow_inc: 0.8 / fs, // 0.8 Hz wow
            flutter_phase: 0.37, // offset so wow & flutter don't start aligned
            flutter_inc: 8.0 / fs, // 8 Hz flutter
            amount: 0.0,
        }
    }

    /// 0..1 modulation depth (the Warble knob). 0 = rock-steady pitch.
    pub fn set_amount(&mut self, amount: f32) {
        self.amount = amount.clamp(0.0, 1.0);
    }

    /// Optionally retune the two rates (Hz), e.g. for the tape-speed toggle.
    pub fn set_rates(&mut self, wow_hz: f32, flutter_hz: f32, fs: f32) {
        self.wow_inc = wow_hz / fs;
        self.flutter_inc = flutter_hz / fs;
    }

    /// Advance one sample; returns the modulation value, ~[-1, 1] × amount.
    /// Wow dominates; flutter adds shimmer, and its share grows with amount so
    /// the high end of the knob gets genuinely unstable.
    #[inline]
    pub fn process(&mut self) -> f32 {
        let wow = fastmath::sin_01(self.wow_phase);
        let flutter = fastmath::sin_01(self.flutter_phase);

        self.wow_phase += self.wow_inc;
        if self.wow_phase >= 1.0 {
            self.wow_phase -= 1.0;
        }
        self.flutter_phase += self.flutter_inc;
        if self.flutter_phase >= 1.0 {
            self.flutter_phase -= 1.0;
        }

        let flutter_share = 0.15 + 0.35 * self.amount; // 0.15..0.50
        let m = wow * (1.0 - flutter_share) + flutter * flutter_share;
        m * self.amount
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const FS: f32 = 96_000.0;

    #[test]
    fn steady_at_zero_amount() {
        let mut w = Warble::new(FS);
        w.set_amount(0.0);
        for _ in 0..10_000 {
            assert_eq!(w.process(), 0.0);
        }
    }

    #[test]
    fn bounded_and_centred_when_active() {
        let mut w = Warble::new(FS);
        w.set_amount(1.0);
        let mut peak = 0.0_f32;
        let mut sum = 0.0_f32;
        let n = FS as usize * 4; // 4 s, several wow cycles
        for _ in 0..n {
            let m = w.process();
            peak = peak.max(m.abs());
            sum += m;
        }
        assert!(peak <= 1.0, "modulation must stay within ±1, got {peak}");
        assert!(peak > 0.3, "active warble should actually move, peak {peak}");
        let mean = sum / n as f32;
        assert!(mean.abs() < 0.05, "modulation should be ~zero-mean, got {mean}");
    }
}
