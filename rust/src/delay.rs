//! Circular delay line over an SDRAM-backed buffer, with linear-interpolated
//! fractional reads. Shared by every pedal (the fractional read is what gives
//! tape/BBD varispeed pitch glide and wow/flutter modulation).

pub struct DelayLine {
    buf: &'static mut [f32],
    write: usize,
}

impl DelayLine {
    pub fn new(buf: &'static mut [f32]) -> Self {
        for s in buf.iter_mut() {
            *s = 0.0;
        }
        Self { buf, write: 0 }
    }

    #[inline]
    pub fn len(&self) -> usize {
        self.buf.len()
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    #[inline]
    pub fn write(&mut self, x: f32) {
        self.buf[self.write] = x;
        self.write += 1;
        if self.write >= self.buf.len() {
            self.write = 0;
        }
    }

    /// Absolute-index read (looper playback over a captured region).
    #[inline]
    pub fn peek(&self, i: usize) -> f32 {
        self.buf[i]
    }

    /// Absolute-index write (looper record/overdub).
    #[inline]
    pub fn poke(&mut self, i: usize, v: f32) {
        self.buf[i] = v;
    }

    /// Zero a `[start, start+len)` region (looper clear).
    pub fn clear_region(&mut self, start: usize, len: usize) {
        let end = (start + len).min(self.buf.len());
        for s in &mut self.buf[start..end] {
            *s = 0.0;
        }
    }

    /// Read `delay` samples in the past, linearly interpolated.
    #[inline]
    pub fn read(&self, delay: f32) -> f32 {
        let n = self.buf.len();
        let d = delay.clamp(1.0, (n - 2) as f32);
        let mut rp = self.write as f32 - d;
        if rp < 0.0 {
            rp += n as f32;
        }
        let i0 = rp as usize;
        let frac = rp - i0 as f32;
        let i1 = if i0 + 1 >= n { 0 } else { i0 + 1 };
        self.buf[i0] + frac * (self.buf[i1] - self.buf[i0])
    }
}
