//! Lo-fi reverb voiced after the PT2399-based Caroline Météore.
//!
//! A Schroeder network — four modulated feedback combs into two allpasses —
//! coloured to sound like a string of PT2399 echo chips pressed into reverb
//! duty:
//!
//! - **Limited bandwidth** — a dark input/feedback low-pass; the PT2399 is
//!   murky and gets murkier as it regenerates.
//! - **Quantisation grit** — coarse amplitude quantisation models the chip's
//!   ~10-bit converter; more "age" = fewer levels = more grain.
//! - **Dirty long decays** — a soft clip in the feedback path, so big washes
//!   distort the way real regenerating PT2399s do.
//! - **Modulation** — a slow LFO detunes the comb taps for the seasick
//!   "Modulated" character.
//!
//! The four combs + two allpasses share one caller-supplied buffer (SDRAM on
//! hardware, a `Vec` in tests) partitioned internally, so there is no
//! aliasing of multiple `&mut` slices. A `feedback_inject` hook lets a later
//! phase fold a pitch-shifted signal into the tank for SHIMMER.
//!
//! Pure `libm` math; host-tested under `cargo test`.

use crate::fastmath::{self, soft_clip};
use crate::onepole::OnePole;

/// Comb delay tunings (samples @ 44.1 kHz, Freeverb-derived); scaled to `fs`.
const COMB_TUNING: [usize; 4] = [1116, 1277, 1422, 1617];
/// Allpass tunings (samples @ 44.1 kHz); scaled to `fs`.
const ALLPASS_TUNING: [usize; 2] = [556, 441];
const REF_FS: f32 = 44_100.0;
const ALLPASS_FB: f32 = 0.5;
/// Max modulation depth in samples (applied to comb read positions).
const MOD_DEPTH_MAX: f32 = 28.0;
/// Refresh the (expensive) comb-modulation sine only every N samples. The LFO is
/// slow (< a few Hz), so block-rate refresh is inaudible but saves 3 of every 4
/// `sinf` calls per comb — real CPU headroom in the 96 kHz audio ISR.
const MOD_UPDATE: u32 = 16;

struct Comb {
    offset: usize,
    len: usize,
    idx: usize,
    filt: f32, // damping low-pass state
    phase: f32,
    mod_cached: f32, // modulation offset (samples), refreshed every MOD_UPDATE
}

struct Allpass {
    offset: usize,
    len: usize,
    idx: usize,
}

pub struct Pt2399Reverb {
    fs: f32,
    mem: &'static mut [f32],
    combs: [Comb; 4],
    allpasses: [Allpass; 2],
    feedback: f32,  // comb feedback (decay/size)
    damp: f32,      // comb damping (0=bright .. 1=dark)
    band: OnePole,  // PT2399 input/output bandwidth
    quant_levels: f32,
    drive: f32,     // feedback soft-clip drive
    lfo_inc: f32,
    mod_depth: f32, // samples
    mod_tick: u32,  // counts samples; sinf refreshed when it wraps
}

impl Pt2399Reverb {
    /// Samples of buffer needed at sample rate `fs` (give this much SDRAM).
    pub fn required_len(fs: f32) -> usize {
        let scale = fs / REF_FS;
        let combs: usize = COMB_TUNING.iter().map(|&t| scaled(t, scale)).sum();
        let aps: usize = ALLPASS_TUNING.iter().map(|&t| scaled(t, scale)).sum();
        combs + aps
    }

    /// Build over a caller-supplied buffer (must be at least `required_len`).
    pub fn new(fs: f32, mem: &'static mut [f32]) -> Self {
        for s in mem.iter_mut() {
            *s = 0.0;
        }
        let scale = fs / REF_FS;
        let mut cursor = 0usize;
        let mut alloc = |len: usize| {
            let off = cursor;
            cursor += len;
            (off, len)
        };
        let combs = [
            mk_comb(alloc(scaled(COMB_TUNING[0], scale)), 0.00),
            mk_comb(alloc(scaled(COMB_TUNING[1], scale)), 0.25),
            mk_comb(alloc(scaled(COMB_TUNING[2], scale)), 0.50),
            mk_comb(alloc(scaled(COMB_TUNING[3], scale)), 0.75),
        ];
        let allpasses = [
            mk_allpass(alloc(scaled(ALLPASS_TUNING[0], scale))),
            mk_allpass(alloc(scaled(ALLPASS_TUNING[1], scale))),
        ];
        debug_assert!(cursor <= mem.len(), "reverb buffer too small");

        let mut r = Self {
            fs,
            mem,
            combs,
            allpasses,
            feedback: 0.84,
            damp: 0.5,
            band: OnePole::new(4_000.0, fs),
            quant_levels: 2048.0,
            drive: 0.0,
            lfo_inc: 0.0,
            mod_depth: 0.0,
            mod_tick: 0,
        };
        r.set_rate(0.6);
        r
    }

    /// Decay/size (0..1) → comb feedback (longer tail near 1).
    pub fn set_decay(&mut self, decay: f32) {
        self.feedback = 0.70 + 0.29 * decay.clamp(0.0, 1.0);
    }

    /// Set the comb feedback directly. Values < 1 are unconditionally stable
    /// (damped, and the feedback path passes the soft clip + quantiser);
    /// slightly > 1 gives a **bounded** self-oscillating bloom — the clip in
    /// the comb feedback path limits it. Clamped to ≤ 1.02.
    pub fn set_feedback(&mut self, fb: f32) {
        self.feedback = fb.clamp(0.0, 1.02);
    }

    /// Tone (0..1): 0 = very dark, 1 = as bright as the PT2399 gets (~8 kHz).
    pub fn set_tone(&mut self, tone: f32) {
        let t = tone.clamp(0.0, 1.0);
        self.band.set_cutoff(900.0 + t * 7_100.0, self.fs);
        self.damp = 0.8 - 0.6 * t;
    }

    /// Age/grit (0..1): coarser quantisation + dirtier feedback.
    pub fn set_age(&mut self, age: f32) {
        let a = age.clamp(0.0, 1.0);
        self.quant_levels = (4096.0 * (1.0 - a) + 96.0 * a).max(32.0);
        self.drive = a;
    }

    /// Modulation amount (0..1) → comb-tap detune depth.
    pub fn set_mod(&mut self, amount: f32) {
        self.mod_depth = MOD_DEPTH_MAX * amount.clamp(0.0, 1.0);
    }

    /// Modulation LFO rate in Hz.
    pub fn set_rate(&mut self, hz: f32) {
        self.lfo_inc = hz.max(0.0) / self.fs;
    }

    /// Process one mono sample. `feedback_inject` is summed with `x` **before**
    /// the chip's input bandwidth filter (used by SHIMMER to fold a
    /// pitch-shifted tail back in); pass 0.0 when unused. Filtering the
    /// injection is both faithful (everything entering a PT2399 passes its
    /// input bandwidth) and what keeps shimmer stable: the rising octave
    /// ladder (+12 → +24 → …) dies at the bandwidth ceiling instead of
    /// accumulating into a scream.
    #[inline]
    pub fn process(&mut self, x: f32, feedback_inject: f32) -> f32 {
        let input = self.band.process(x + feedback_inject);

        let refresh_mod = self.mod_tick == 0;
        self.mod_tick += 1;
        if self.mod_tick >= MOD_UPDATE {
            self.mod_tick = 0;
        }

        let mut acc = 0.0f32;
        for c in 0..self.combs.len() {
            acc += self.comb_tick(c, input, refresh_mod);
        }
        let mut y = acc * 0.25;

        for a in 0..self.allpasses.len() {
            y = self.allpass_tick(a, y);
        }
        y
    }

    #[inline]
    fn comb_tick(&mut self, c: usize, input: f32, refresh_mod: bool) -> f32 {
        let (offset, len) = (self.combs[c].offset, self.combs[c].len);

        // Modulated fractional read position, just behind the write index. The
        // sine is only recomputed every MOD_UPDATE samples (see process()).
        if refresh_mod {
            let phase = self.combs[c].phase;
            self.combs[c].mod_cached =
                self.mod_depth * 0.5 * (1.0 + fastmath::sin_01(phase));
        }
        // Reading at `idx` (just before writing it) is the full comb delay
        // `len`; adding the modulation offset shortens it to `len - mod_samp`.
        // (`idx - mod_samp` would be a delay of only `mod_samp` samples — the
        // combs collapse to sub-millisecond metallic resonators.)
        let mod_samp = self.combs[c].mod_cached;
        let read = self.combs[c].idx as f32 + mod_samp;
        let out = self.read_frac(offset, len, read);

        // Damping low-pass in the feedback, then PT2399 grit.
        let filt = out * (1.0 - self.damp) + self.combs[c].filt * self.damp;
        self.combs[c].filt = filt;
        let fed = quantize(soft_clip(filt * (1.0 + self.drive)), self.quant_levels);

        let idx = self.combs[c].idx;
        self.mem[offset + idx] = input + fed * self.feedback;
        // Compare-wrap instead of `%` — len is not a power of two, so the
        // modulo costs a hardware divide per comb per sample.
        let next = idx + 1;
        self.combs[c].idx = if next == len { 0 } else { next };

        // Advance the LFO phase every sample (cheap); the sine itself is only
        // evaluated on refresh ticks.
        let mut phase = self.combs[c].phase + self.lfo_inc;
        if phase >= 1.0 {
            phase -= 1.0;
        }
        self.combs[c].phase = phase;

        out
    }

    #[inline]
    fn allpass_tick(&mut self, a: usize, x: f32) -> f32 {
        let (offset, len, idx) = (
            self.allpasses[a].offset,
            self.allpasses[a].len,
            self.allpasses[a].idx,
        );
        let buf = self.mem[offset + idx];
        let out = -x + buf;
        self.mem[offset + idx] = x + buf * ALLPASS_FB;
        let next = idx + 1;
        self.allpasses[a].idx = if next == len { 0 } else { next };
        out
    }

    /// Linear-interpolated read of `pos` (in samples, fractional, in
    /// `[0, 2·len)` — i.e. at most one wrap past the end) within the comb
    /// region `[offset, offset+len)`. Callers guarantee the bound: the only
    /// out-of-range excursion is `idx + mod_samp` with `mod_samp ≤ 28 ≪ len`.
    #[inline]
    fn read_frac(&self, offset: usize, len: usize, pos: f32) -> f32 {
        let lenf = len as f32;
        let p = if pos >= lenf { pos - lenf } else { pos };
        debug_assert!((0.0..lenf).contains(&p));
        // p ∈ [0, len), so integer truncation == floor.
        let i0 = p as usize;
        let frac = p - i0 as f32;
        let i1 = if i0 + 1 >= len { 0 } else { i0 + 1 };
        let a = self.mem[offset + i0];
        let b = self.mem[offset + i1];
        a + frac * (b - a)
    }
}

#[inline]
fn scaled(tuning: usize, scale: f32) -> usize {
    ((tuning as f32) * scale) as usize
}

fn mk_comb((offset, len): (usize, usize), phase: f32) -> Comb {
    Comb {
        offset,
        len,
        idx: 0,
        filt: 0.0,
        phase,
        mod_cached: 0.0,
    }
}

fn mk_allpass((offset, len): (usize, usize)) -> Allpass {
    Allpass {
        offset,
        len,
        idx: 0,
    }
}

/// Coarse amplitude quantisation (PT2399 converter grain).
#[inline]
fn quantize(x: f32, levels: f32) -> f32 {
    fastmath::round(x * levels) / levels
}

#[cfg(test)]
mod tests {
    use super::*;

    const FS: f32 = 96_000.0;

    fn mk() -> Pt2399Reverb {
        let n = Pt2399Reverb::required_len(FS);
        let buf: &'static mut [f32] = Box::leak(vec![0.0f32; n].into_boxed_slice());
        Pt2399Reverb::new(FS, buf)
    }

    #[test]
    fn required_len_is_sane() {
        let n = Pt2399Reverb::required_len(FS);
        assert!(n > 8_000 && n < 40_000, "unexpected buffer size {n}");
    }

    #[test]
    fn silence_stays_silent() {
        let mut r = mk();
        let mut y = 0.0;
        for _ in 0..4000 {
            y = r.process(0.0, 0.0);
        }
        assert!(y.abs() < 1e-4, "silence should stay silent, got {y}");
    }

    #[test]
    fn impulse_produces_a_decaying_tail() {
        let mut r = mk();
        r.set_decay(0.7);
        r.set_tone(0.6);
        let mut early = 0.0f32;
        let mut late = 0.0f32;
        let _ = r.process(1.0, 0.0); // impulse
        for n in 0..FS as usize {
            let y = r.process(0.0, 0.0).abs();
            if n < 4_000 {
                early = early.max(y);
            }
            if (40_000..44_000).contains(&n) {
                late = late.max(y);
            }
        }
        assert!(early > 1e-3, "expected an audible early tail, got {early}");
        assert!(late < early, "tail should decay: late={late} early={early}");
    }

    #[test]
    fn longer_decay_rings_longer() {
        let energy = |decay: f32| {
            let mut r = mk();
            r.set_decay(decay);
            r.set_tone(0.6);
            let _ = r.process(1.0, 0.0);
            let mut e = 0.0f64;
            for _ in 0..FS as usize {
                let y = r.process(0.0, 0.0) as f64;
                e += y * y;
            }
            e
        };
        assert!(
            energy(0.9) > energy(0.3),
            "more decay should ring longer"
        );
    }

    #[test]
    fn modulated_combs_keep_their_delay() {
        // Regression: the modulated tap read `idx - mod_samp`, collapsing every
        // comb to a `mod_samp`-sample (≤28) delay whenever modulation was
        // active. Correct combs cannot produce output before the shortest comb
        // delay (~2429 samples @96k, minus the ≤28-sample modulation).
        let mut r = mk();
        r.set_decay(0.6);
        r.set_tone(0.6);
        r.set_mod(1.0);
        r.set_rate(1.0);
        let _ = r.process(1.0, 0.0); // impulse
        let min_comb = (1116.0 * (FS / 44_100.0)) as usize; // shortest comb, scaled
        let mut early_peak = 0.0f32;
        for _ in 0..(min_comb - 64) {
            early_peak = early_peak.max(r.process(0.0, 0.0).abs());
        }
        assert!(
            early_peak < 1.0e-4,
            "output before the shortest comb delay means the modulated tap \
             reads the wrong side of the write head, early peak {early_peak}"
        );
        // …and the tail must still arrive after it.
        let mut tail_peak = 0.0f32;
        for _ in 0..(FS as usize / 4) {
            tail_peak = tail_peak.max(r.process(0.0, 0.0).abs());
        }
        assert!(tail_peak > 1.0e-3, "reverb tail missing, peak {tail_peak}");
    }

    #[test]
    fn shimmer_injection_stays_bounded_across_the_regen_range() {
        // Regression for the bench "massive feedback": the tank amplifies any
        // injection by its resonant gain ~1/(1-fb), so the SpaceEngine scales
        // the shimmer budget with the remaining headroom:
        //   amt = (0.3 + 0.7·regen) · 2 · (1 - fb), clamped to 0.3
        // Verify the whole law stays bounded and decaying at the least-damped
        // tone, sweeping fb across the regen range (worst case is mid fb where
        // the budget is largest).
        use crate::pitch::OctaveUp;
        for &fb in &[0.80f32, 0.90, 0.95, 0.995] {
            let mut r = mk();
            r.set_feedback(fb);
            r.set_tone(1.0); // least damping = worst case
            r.set_mod(0.5);
            let amt = (1.0 * (1.0 - fb)).clamp(0.0, 0.15); // regen knob = 1.0
            let shim_buf: &'static mut [f32] =
                Box::leak(vec![0.0f32; 2048].into_boxed_slice());
            let mut shim = OctaveUp::new(shim_buf);
            let mut tail = 0.0f32;
            let mut late_peak = 0.0f32;
            for n in 0..(FS as usize * 3) {
                let x = if n < 4800 { 0.5 } else { 0.0 }; // 50 ms burst
                let inject = shim.process(tail) * amt;
                tail = r.process(x, inject);
                assert!(tail.is_finite(), "non-finite tail at fb={fb} n={n}");
                if n > FS as usize * 2 {
                    late_peak = late_peak.max(tail.abs());
                }
            }
            assert!(
                late_peak < 0.5,
                "tail must decay, fb={fb} late peak {late_peak}"
            );
        }
    }

    #[test]
    fn stays_finite_and_bounded_under_max_settings() {
        let mut r = mk();
        r.set_decay(1.0);
        r.set_tone(1.0);
        r.set_age(1.0);
        r.set_mod(1.0);
        r.set_rate(1.5);
        let mut maxy = 0.0f32;
        for n in 0..(FS as usize) {
            let x = libm::sinf(core::f32::consts::TAU * 330.0 * n as f32 / FS);
            let y = r.process(x, 0.0);
            assert!(y.is_finite(), "non-finite at n={n}");
            maxy = maxy.max(y.abs());
        }
        assert!(maxy < 16.0, "reverb should stay bounded, peak={maxy}");
    }
}
