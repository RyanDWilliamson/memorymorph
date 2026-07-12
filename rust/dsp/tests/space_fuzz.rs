//! Crash fuzz for the SPACE signal path (bench: "space mode crashes the
//! pedal — tone + both LEDs strobing" = panic/fault on hardware).
//!
//! Replicates the firmware's usage pattern: `set_*` called every block with
//! arbitrary knob combinations (including the SpaceEngine's derived laws),
//! audio + shimmer injection every sample. Any indexing/arithmetic panic in
//! the dsp layer reproduces here; if this stays green, the crash lives in the
//! firmware-only layer.

use dsp::pitch::OctaveUp;
use dsp::pt2399_reverb::Pt2399Reverb;

struct Lcg(u32);
impl Lcg {
    fn next(&mut self) -> f32 {
        self.0 = self.0.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        (self.0 >> 9) as f32 / (1 << 23) as f32 // [0,1)
    }
}

#[test]
fn space_path_survives_randomized_parameter_abuse() {
    const FS: f32 = 96_000.0;
    const BLOCK: usize = 32;
    let mut rng = Lcg(0xD1F7_0001);

    for cfg in 0..64 {
        let n = Pt2399Reverb::required_len(FS);
        let buf: &'static mut [f32] = Box::leak(vec![0.0f32; n].into_boxed_slice());
        let mut r = Pt2399Reverb::new(FS, buf);
        let shim_buf: &'static mut [f32] = Box::leak(vec![0.0f32; 2048].into_boxed_slice());
        let mut shim = OctaveUp::new(shim_buf);
        let mut tail = 0.0f32;

        // ~1 s of audio per config, params re-randomized every block —
        // including step changes as violent as a page flip with soft-takeover
        // pickup, and out-of-range values (the firmware clamps, but the dsp
        // layer must survive garbage too).
        for blk in 0..(FS as usize / BLOCK) {
            let decay = rng.next() * 1.2 - 0.1; // deliberately out of range
            let regen = rng.next() * 1.2 - 0.1;
            let base = 0.70 + 0.25 * decay.clamp(0.0, 1.0);
            let fb = if blk % 97 == 0 {
                1.01 // bloom
            } else {
                base + regen.clamp(0.0, 1.0) * (0.95 - base)
            };
            r.set_feedback(fb);
            r.set_tone(rng.next() * 1.4 - 0.2);
            r.set_age(rng.next() * 1.4 - 0.2);
            r.set_mod(rng.next() * 1.4 - 0.2);
            r.set_rate(rng.next() * 4.0);
            let shimmer_amt = ((0.3 + 0.7 * regen.clamp(0.0, 1.0))
                * 0.5
                * (1.0 - fb))
                .clamp(0.0, 0.08);
            let norm = Pt2399Reverb::output_norm_for(fb);

            for s in 0..BLOCK {
                let x = if (blk + s) % 3 == 0 {
                    (rng.next() * 2.0 - 1.0) * 1.5 // hot, clipped upstream IRL
                } else {
                    0.0
                };
                let inject = shim.process(tail) * shimmer_amt;
                let wet = r.process(x, inject) * norm;
                assert!(
                    wet.is_finite(),
                    "non-finite output: cfg={cfg} blk={blk} s={s}"
                );
                tail = wet;
            }
        }
    }
}
