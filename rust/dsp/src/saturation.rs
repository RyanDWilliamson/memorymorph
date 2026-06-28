//! Tape/valve saturation, ported from Memory Morph's `TapeSatProcess`
//! (`../../src/MemoryMorph/memory_morph.cpp`) and the Echorec `TubeStage`
//! (`tube.h` on the echorec-refinements branch).

use crate::{OnePole, TWO_PI};
use libm::{expf, tanhf};

/// Three-stage magnetic-tape saturator:
///   1. HF pre-emphasis (one-pole LPF gives the low shelf; HF = x − shelf,
///      boosted 1.6×) — ferric oxide saturates highs first.
///   2. Asymmetric `tanh` with a DC bias for even-harmonic "tape bias" warmth.
///   3. Post-saturation rolloff (~8 kHz) for head-gap loss.
///
/// Output is bounded within ±1 by the internal `tanh`. `set_drive` scales the
/// pre-clip gain so a Drive knob can sweep clean → wild.
pub struct TapeSat {
    /// Pre-emphasis LPF state and pole (`a = e^(-2π·3000/fs)`), matching the
    /// C++ `z = a·z + (1-a)·x` form (note: a, not 1−a).
    hpf_a: f32,
    hpf_z: f32,
    post: OnePole,
    pre_gain: f32,
    bias_comp: f32,
}

// Asymmetry constant from the C++. The bias shifts the tanh operating point to
// add even harmonics; `bias_comp = tanh(BIAS)` is subtracted so idle output is
// zero. (The C++ hardcoded 0.17928 ≈ tanh(0.18); we compute it exactly, which
// removes the original's ~1 mV idle DC offset.)
const BIAS: f32 = 0.18;
/// Memory Morph's fixed pre-gain (kPreGain); the centre of our Drive sweep.
const NOMINAL_PRE_GAIN: f32 = 18.0;

impl TapeSat {
    pub fn new(fs: f32) -> Self {
        Self {
            hpf_a: expf(-TWO_PI * 3000.0 / fs),
            hpf_z: 0.0,
            post: OnePole::new(8000.0, fs),
            pre_gain: NOMINAL_PRE_GAIN,
            bias_comp: tanhf(BIAS),
        }
    }

    /// Drive 0..1 → pre-clip gain ~4..28 (nominal 18 near 0.6). Higher = wilder.
    pub fn set_drive(&mut self, drive: f32) {
        let d = drive.clamp(0.0, 1.0);
        self.pre_gain = 4.0 + d * 24.0;
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        // 1. Pre-emphasis.
        self.hpf_z = self.hpf_a * self.hpf_z + (1.0 - self.hpf_a) * x;
        let hf = x - self.hpf_z;
        let boosted = x + hf * 0.6;

        // 2. Asymmetric saturation (self-limiting), DC removed by kBiasComp.
        let clipped = tanhf(boosted * self.pre_gain + BIAS) - self.bias_comp;

        // 3. Post-saturation rolloff.
        self.post.process(clipped)
    }

    pub fn reset(&mut self) {
        self.hpf_z = 0.0;
        self.post.reset();
    }
}

/// Valve stage: asymmetric soft-clip `y = tanh(x + a·x²)`, DC-blocked (the x²
/// term introduces DC). Even-harmonic triode warmth. Ported from `TubeStage`.
pub struct TubeStage {
    a: f32,
    dc: OnePole,
}

impl TubeStage {
    pub fn new(fs: f32) -> Self {
        Self {
            a: 0.05,
            dc: OnePole::new(20.0, fs),
        }
    }

    /// 2nd-harmonic skew (asymmetry); the Echorec ages this up with the Age knob.
    pub fn set_skew(&mut self, a: f32) {
        self.a = a;
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        let y = tanhf(x + self.a * x * x);
        y - self.dc.process(y) // remove the DC the x² term introduced
    }

    pub fn reset(&mut self) {
        self.dc.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const FS: f32 = 96_000.0;

    /// Silence in → silence out (the bias compensation must zero idle DC).
    #[test]
    fn tape_sat_zero_in_zero_out() {
        let mut s = TapeSat::new(FS);
        let mut y = 0.0;
        for _ in 0..2000 {
            y = s.process(0.0);
        }
        assert!(y.abs() < 1e-3, "idle output should be ~0, got {y}");
    }

    /// Output stays bounded well within ±1 even when hammered hard.
    #[test]
    fn tape_sat_bounded_when_driven() {
        let mut s = TapeSat::new(FS);
        s.set_drive(1.0);
        let mut peak = 0.0_f32;
        for n in 0..4800 {
            let x = (n as f32 * 0.05).sin() * 2.0; // hot input
            peak = peak.max(s.process(x).abs());
        }
        // The asymmetric −bias_comp shift lets the negative excursion reach
        // ~−(1 + tanh(0.18)) ≈ −1.18, so "bounded" means ±1.2, not ±1.
        assert!(peak < 1.2, "saturated output should stay bounded, got {peak}");
        assert!(peak > 0.05, "driven output should be audible, got {peak}");
    }

    /// More drive => more harmonic energy (RMS rises) for the same input.
    #[test]
    fn tape_sat_drive_increases_level() {
        let rms = |drive: f32| {
            let mut s = TapeSat::new(FS);
            s.set_drive(drive);
            let mut acc = 0.0_f32;
            let n = 4800;
            for i in 0..n {
                let x = (i as f32 * 0.02).sin() * 0.2;
                let y = s.process(x);
                acc += y * y;
            }
            (acc / n as f32).sqrt()
        };
        assert!(rms(1.0) > rms(0.1), "higher drive should raise output RMS");
    }

    #[test]
    fn tube_stage_zero_in_zero_out_and_bounded() {
        let mut t = TubeStage::new(FS);
        t.set_skew(0.2);
        let mut peak = 0.0_f32;
        let mut last = 0.0;
        for n in 0..4800 {
            let x = (n as f32 * 0.03).sin() * 1.5;
            last = t.process(x);
            peak = peak.max(last.abs());
        }
        assert!(peak < 1.5 && peak > 0.05);
        // Settle on silence and confirm DC blocker pulls it back toward zero.
        for _ in 0..8000 {
            last = t.process(0.0);
        }
        assert!(last.abs() < 1e-3, "DC-blocked idle should be ~0, got {last}");
    }
}
