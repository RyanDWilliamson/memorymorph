//! Bucket-brigade device (BBD) delay coloration — a behavioural model of the
//! analog delay chip's *character*, not its memory.
//!
//! The delay memory itself (and the varispeed fractional read that gives the
//! tape/BBD pitch glide) lives in the firmware engine; this module models what
//! the silicon does to the signal around that delay:
//!
//! 1. **Syllabic compander** — a 2:1 compress on the way in, 1:2 expand on the
//!    way out, exactly as the NE570-class noise reduction in real BBD pedals.
//!    The envelope time-constants are what produce the audible *pumping /
//!    breathing* as the delay regenerates.
//! 2. **Charge-transfer nonlinearity** — a gentle soft clip on the compressed
//!    (hot) signal.
//! 3. **Clock-dependent bandwidth** — anti-alias (pre) and reconstruction
//!    (post) low-passes whose cutoff tracks the BBD clock: longer delay = slower
//!    clock = darker. Feedback narrows it further (the classic regenerating
//!    BBD "bandwidth collapse").
//! 4. **Pumped noise floor** — a little hiss injected before the expander, so
//!    the compander breathes it up and down like the real thing.
//!
//! Wiring in the engine (per sample):
//! ```ignore
//! let c = bbd.pre(x);          // compress + soften + anti-alias
//! delay.write(c);
//! let d = delay.read(frac);    // firmware-owned varispeed delay
//! let y = bbd.post(d);         // reconstruction + noise + expand
//! ```
//!
//! Pure `libm` math; host-tested under `cargo test`.

use crate::fastmath::{self, soft_clip};
use crate::onepole::OnePole;

/// Effective BBD stage count — sets how the clock (and therefore bandwidth)
/// scales with delay time. ~4096 gives a musically dark long delay and a bright
/// short one.
const STAGES: f32 = 4096.0;

/// Compander reference level (linear). Signals near this pass at unity; quieter
/// signals are boosted (and their noise floor with them).
const REF: f32 = 0.06;

const ENV_EPS: f32 = 1.0e-5;

pub struct Bbd {
    fs: f32,
    pre_lp: OnePole,
    post_lp: OnePole,
    // Compander envelopes (one-pole rectified followers).
    comp_env: f32,
    exp_env: f32,
    env_coeff: f32,
    bandwidth: f32,
    // Cheap LCG noise.
    rng: u32,
    noise_amt: f32,
}

impl Bbd {
    pub fn new(fs: f32) -> Self {
        // ~8 ms syllabic time constant: slow enough to pump musically, fast
        // enough to follow notes.
        let env_coeff = crate::onepole::one_pole_coeff(20.0, fs);
        let mut b = Self {
            fs,
            pre_lp: OnePole::new(8_000.0, fs),
            post_lp: OnePole::new(8_000.0, fs),
            comp_env: REF,
            exp_env: REF,
            env_coeff,
            bandwidth: 8_000.0,
            rng: 0x1234_5678,
            noise_amt: 0.0008,
        };
        b.set_time(0.25, 0.0);
        b
    }

    /// Set the BBD clock from the current delay time (seconds) and feedback
    /// amount (0..1). Longer delay and more feedback both darken the voice.
    pub fn set_time(&mut self, delay_s: f32, feedback: f32) {
        let d = delay_s.max(0.001);
        let fclk = STAGES / (2.0 * d); // BBD clock for this delay
        let mut cutoff = 0.5 * fclk; // reconstruction bandwidth ≈ fclk/2
        cutoff *= 1.0 - 0.4 * feedback.clamp(0.0, 1.0); // regen collapse
        let cutoff = cutoff.clamp(1_200.0, (0.45 * self.fs).min(10_000.0));
        self.bandwidth = cutoff;
        self.pre_lp.set_cutoff(cutoff, self.fs);
        self.post_lp.set_cutoff(cutoff, self.fs);
    }

    /// Amount of injected hiss (linear, before the expander). Default ~0.0008.
    pub fn set_noise(&mut self, amt: f32) {
        self.noise_amt = amt.max(0.0);
    }

    /// Current modeled bandwidth in Hz (for inspection/tests).
    #[inline]
    pub fn bandwidth_hz(&self) -> f32 {
        self.bandwidth
    }

    /// Input stage: 2:1 compress → charge-transfer soft clip → anti-alias LPF.
    #[inline]
    pub fn pre(&mut self, x: f32) -> f32 {
        self.pre_mix(x, 0.0)
    }

    /// Input stage with recirculation. The dry input is compressed, but the
    /// feedback re-enters in the **compressed domain** (`read·fb`, exactly as
    /// stored). It must NOT be expanded and re-compressed around the loop: an
    /// ideal 2:1 compander inside a feedback loop has a stable non-zero fixed
    /// point (A* = fb²·REF) — repeats that never decay, converging to a
    /// self-sustaining platform. Compressed-domain feedback makes the loop
    /// linear in `fb` (true exponential decay) while still passing the
    /// charge-transfer clip and anti-alias filter every pass, so per-pass
    /// darkening and the freeze/havoc clip-limit are preserved.
    #[inline]
    pub fn pre_mix(&mut self, x: f32, feedback_compressed: f32) -> f32 {
        self.comp_env += self.env_coeff * (x.abs() - self.comp_env);
        let gain = fastmath::sqrt(REF / (self.comp_env + ENV_EPS)).clamp(0.5, 6.0);
        // Gain staging: cap the dry injection so hot playing squashes
        // (tape-like) instead of railing the loop's clip headroom — otherwise
        // the buffer parks at the clip and the repeats tower over the dry
        // ("feedback" while playing hot). The recirculation keeps its own
        // headroom above the cap.
        let driven = (x * gain).clamp(-0.6, 0.6);
        let compressed = soft_clip(driven + feedback_compressed);
        self.pre_lp.process(compressed)
    }

    /// Output stage: reconstruction LPF → pumped noise → 1:2 expand.
    #[inline]
    pub fn post(&mut self, x: f32) -> f32 {
        let filtered = self.post_lp.process(x);
        let noisy = filtered + self.white() * self.noise_amt;
        self.exp_env += self.env_coeff * (noisy.abs() - self.exp_env);
        let gain = fastmath::sqrt((self.exp_env + ENV_EPS) / REF).clamp(0.1, 2.0);
        noisy * gain
    }

    #[inline]
    fn white(&mut self) -> f32 {
        // Xorshift-ish LCG → [-1, 1].
        self.rng = self.rng.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        (self.rng >> 9) as f32 / (1 << 23) as f32 * 2.0 - 1.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FS: f32 = 96_000.0;

    /// Run one sample through the full pre→post path (no delay in between).
    fn thru(b: &mut Bbd, x: f32) -> f32 {
        let c = b.pre(x);
        b.post(c)
    }

    #[test]
    fn zero_in_zero_out() {
        let mut b = Bbd::new(FS);
        let mut y = 0.0;
        for _ in 0..2000 {
            y = thru(&mut b, 0.0);
        }
        assert!(y.abs() < 1e-3, "silence should stay near silence, got {y}");
    }

    #[test]
    fn longer_delay_is_darker() {
        let mut b = Bbd::new(FS);
        b.set_time(0.05, 0.0);
        let short = b.bandwidth_hz();
        b.set_time(0.6, 0.0);
        let long = b.bandwidth_hz();
        assert!(
            long < short,
            "longer delay must lower bandwidth: short={short} long={long}"
        );
    }

    #[test]
    fn feedback_collapses_bandwidth() {
        let mut b = Bbd::new(FS);
        b.set_time(0.3, 0.0);
        let dry = b.bandwidth_hz();
        b.set_time(0.3, 0.9);
        let regen = b.bandwidth_hz();
        assert!(regen < dry, "feedback should narrow bandwidth: {regen} < {dry}");
    }

    #[test]
    fn compander_roundtrip_is_roughly_unity() {
        // Feed a steady mid-level sine through pre→post (no delay between) and
        // check the output level is in the same ballpark as the input (the
        // 2:1/1:2 companding cancels in steady state).
        let mut b = Bbd::new(FS);
        let f = 220.0;
        let mut peak_in = 0.0f32;
        let mut peak_out = 0.0f32;
        for n in 0..8000 {
            let x = 0.3 * libm::sinf(core::f32::consts::TAU * f * n as f32 / FS);
            let y = thru(&mut b, x);
            if n > 4000 {
                peak_in = peak_in.max(x.abs());
                peak_out = peak_out.max(y.abs());
            }
        }
        let ratio = peak_out / peak_in;
        assert!(
            (0.5..2.0).contains(&ratio),
            "companding should roughly preserve level, ratio={ratio}"
        );
    }

    #[test]
    fn feedback_loop_repeats_decay_to_silence() {
        // Regression for the "eternal loop" bench bug: expanding + re-
        // compressing the recirculation gives the compander a stable non-zero
        // fixed point (A* = fb²·REF) — repeats sustain forever. With
        // compressed-domain feedback (pre_mix) the loop must decay. Simulates
        // the TimeEngine wiring with a 10 ms loop at fb = 0.9.
        let mut b = Bbd::new(FS);
        b.set_noise(0.0); // isolate the loop math from the hiss floor
        b.set_time(0.01, 0.9);
        let n = 960;
        let mut delay = vec![0.0f32; n];
        let mut w = 0usize;
        let fb = 0.9f32;
        let mut late_peak = 0.0f32;
        for i in 0..(FS as usize * 4) {
            // 50 ms input burst, then silence.
            let x = if i < 4800 {
                0.5 * libm::sinf(core::f32::consts::TAU * 220.0 * i as f32 / FS)
            } else {
                0.0
            };
            let read = delay[w];
            let wet = b.post(read);
            delay[w] = b.pre_mix(x, read * fb);
            w = (w + 1) % n;
            if i > FS as usize * 3 {
                late_peak = late_peak.max(wet.abs());
            }
        }
        assert!(
            late_peak < 1.0e-3,
            "repeats must decay to silence, late peak {late_peak}"
        );
    }

    #[test]
    fn output_is_bounded() {
        let mut b = Bbd::new(FS);
        b.set_time(0.3, 0.8);
        let mut maxy = 0.0f32;
        for n in 0..20000 {
            let x = libm::sinf(core::f32::consts::TAU * 440.0 * n as f32 / FS);
            let y = thru(&mut b, x);
            maxy = maxy.max(y.abs());
            assert!(y.is_finite(), "non-finite output");
        }
        assert!(maxy < 8.0, "output should stay bounded, peak={maxy}");
    }
}
