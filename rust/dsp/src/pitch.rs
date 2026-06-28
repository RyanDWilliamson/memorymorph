//! Octave-up pitch shifter (delay-line, two-grain crossfade) for SHIMMER.
//!
//! Classic time-domain shifter: a write head fills a ring buffer at 1×; a read
//! head sweeps toward the write head at 2× so the delay shrinks one sample per
//! sample, raising pitch an octave. The sweep wraps discontinuously, so a second
//! grain offset by half the window crossfades with the first (triangular windows
//! that sum to unity) to hide the seam.
//!
//! Buffer is caller-supplied (SDRAM on hardware, `Vec` in tests); window length
//! = buffer length. Pure `libm`; host-tested.

const RATIO: f32 = 2.0; // +12 semitones

pub struct OctaveUp {
    buf: &'static mut [f32],
    wp: usize,
    phase: f32, // [0,1): position of grain 1 within the window
}

impl OctaveUp {
    pub fn new(buf: &'static mut [f32]) -> Self {
        for s in buf.iter_mut() {
            *s = 0.0;
        }
        Self {
            buf,
            wp: 0,
            phase: 0.0,
        }
    }

    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        let n = self.buf.len();
        self.buf[self.wp] = x;
        self.wp += 1;
        if self.wp >= n {
            self.wp = 0;
        }

        // Grain phase decreases so the read head approaches the write head.
        self.phase -= (RATIO - 1.0) / n as f32;
        if self.phase < 0.0 {
            self.phase += 1.0;
        }
        let p2 = if self.phase + 0.5 >= 1.0 {
            self.phase - 0.5
        } else {
            self.phase + 0.5
        };

        let nf = n as f32;
        let g1 = self.read_behind(self.phase * nf);
        let g2 = self.read_behind(p2 * nf);
        // Triangular windows offset by half a window sum to 1.
        g1 * tri(self.phase) + g2 * tri(p2)
    }

    /// Interpolated read `behind` samples before the write head.
    #[inline]
    fn read_behind(&self, behind: f32) -> f32 {
        let n = self.buf.len();
        let mut rp = self.wp as f32 - behind;
        while rp < 0.0 {
            rp += n as f32;
        }
        while rp >= n as f32 {
            rp -= n as f32;
        }
        let i0 = rp as usize;
        let frac = rp - i0 as f32;
        let i1 = if i0 + 1 >= n { 0 } else { i0 + 1 };
        self.buf[i0] + frac * (self.buf[i1] - self.buf[i0])
    }
}

/// Triangular window: 0 at the ends, 1 at the middle.
#[inline]
fn tri(p: f32) -> f32 {
    1.0 - (2.0 * p - 1.0).abs()
}

#[cfg(test)]
mod tests {
    use super::*;

    const FS: f32 = 96_000.0;

    fn mk(n: usize) -> OctaveUp {
        let buf: &'static mut [f32] = Box::leak(vec![0.0f32; n].into_boxed_slice());
        OctaveUp::new(buf)
    }

    #[test]
    fn silence_stays_silent() {
        let mut p = mk(2048);
        let mut y = 0.0;
        for _ in 0..5000 {
            y = p.process(0.0);
        }
        assert!(y.abs() < 1e-4, "silence stayed silent? got {y}");
    }

    #[test]
    fn output_is_bounded() {
        let mut p = mk(2048);
        let mut maxy = 0.0f32;
        for n in 0..20000 {
            let x = libm::sinf(core::f32::consts::TAU * 300.0 * n as f32 / FS);
            let y = p.process(x);
            assert!(y.is_finite());
            maxy = maxy.max(y.abs());
        }
        assert!(maxy < 1.5, "octave-up should stay bounded, peak={maxy}");
    }

    #[test]
    fn raises_pitch_roughly_an_octave() {
        // Rising zero-crossings should roughly double for a +1 octave shift.
        let mut p = mk(2048);
        let f = 250.0;
        let count_rising = |sig: &dyn Fn(usize) -> f32| {
            let mut prev = 0.0f32;
            let mut c = 0;
            for n in 0..(FS as usize) {
                let v = sig(n);
                if prev < 0.0 && v >= 0.0 {
                    c += 1;
                }
                prev = v;
            }
            c
        };
        let input_rising = count_rising(&|n| libm::sinf(core::f32::consts::TAU * f * n as f32 / FS));

        // Prime then measure output zero-crossings.
        let mut prev = 0.0f32;
        let mut out_rising = 0;
        for n in 0..(FS as usize) {
            let x = libm::sinf(core::f32::consts::TAU * f * n as f32 / FS);
            let y = p.process(x);
            if n > 4000 {
                if prev < 0.0 && y >= 0.0 {
                    out_rising += 1;
                }
                prev = y;
            }
        }
        // Allow generous tolerance — grain crossfades smear the count.
        let ratio = out_rising as f32 / input_rising as f32;
        assert!(
            (1.6..2.4).contains(&ratio),
            "expected ~2× pitch, ratio={ratio} (in={input_rising} out={out_rising})"
        );
    }
}
