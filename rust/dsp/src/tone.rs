//! Cassette bandwidth limiting — a band-pass voicing (one-pole HPF + LPF) whose
//! width is set by a single Tone knob. Narrow = boxy/old-tape; wide = fuller.
//! Conceptually the same low-shelf/high-rolloff shaping as the Echorec `tone.h`,
//! collapsed into one user control for the cassette voice.

use crate::OnePole;

pub struct CassetteTone {
    hp: OnePole, // tracks lows; subtract to high-pass
    lp: OnePole, // high rolloff
    fs: f32,
}

impl CassetteTone {
    pub fn new(fs: f32) -> Self {
        let mut t = Self {
            hp: OnePole::new(120.0, fs),
            lp: OnePole::new(10_000.0, fs),
            fs,
        };
        t.set_bandwidth(0.6);
        t
    }

    /// Bandwidth 0..1: narrow lo-fi → wide. LPF sweeps ~2.5–14 kHz, HPF corner
    /// ~320 → 60 Hz, so turning down both darkens and thins toward a cassette box.
    pub fn set_bandwidth(&mut self, b: f32) {
        let b = b.clamp(0.0, 1.0);
        let lpf = 2_500.0 + b * 11_500.0;
        let hpf = 320.0 - b * 260.0;
        self.lp.set_cutoff(lpf, self.fs);
        self.hp.set_cutoff(hpf, self.fs);
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        let lows = self.hp.process(x);
        let high_passed = x - lows; // remove content below the HPF corner
        self.lp.process(high_passed) // roll off the highs
    }

    pub fn reset(&mut self) {
        self.hp.reset();
        self.lp.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const FS: f32 = 96_000.0;

    fn rms_of_broadband(bandwidth: f32) -> f32 {
        let mut t = CassetteTone::new(FS);
        t.set_bandwidth(bandwidth);
        // Sum of a low, mid and high tone to probe the passband width.
        let mut acc = 0.0_f32;
        let n = 9600;
        for i in 0..n {
            let p = i as f32;
            let x = (p * crate::TWO_PI * 80.0 / FS).sin()
                + (p * crate::TWO_PI * 2_000.0 / FS).sin()
                + (p * crate::TWO_PI * 13_000.0 / FS).sin();
            let y = t.process(x * 0.3);
            acc += y * y;
        }
        (acc / n as f32).sqrt()
    }

    #[test]
    fn wider_bandwidth_passes_more_energy() {
        assert!(
            rms_of_broadband(1.0) > rms_of_broadband(0.0),
            "wide setting should pass more broadband energy than narrow"
        );
    }

    #[test]
    fn blocks_dc() {
        let mut t = CassetteTone::new(FS);
        let mut y = 0.0;
        for _ in 0..20_000 {
            y = t.process(1.0);
        }
        assert!(y.abs() < 1e-2, "HPF should reject DC, got {y}");
    }
}
