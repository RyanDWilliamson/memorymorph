//! Cassette LoFi Junky signal chain: ties the `dsp` modules to an SDRAM delay
//! line and the control-map parameters.
//!
//! Chain (mono, per sample):
//!   x → TapeSat → [write to delay, read with wow/flutter offset] → CassetteTone
//!     → + Hiss → × Dropout gate → dry/wet mix → × Level
//!
//! The short (~12–30 ms) modulated delay is the wow/flutter pitch wobble, not an
//! echo. `Params` is produced in the control loop and consumed in the audio ISR.

use dsp::lofi::{Dropout, Hiss};
use dsp::saturation::TapeSat;
use dsp::tone::CassetteTone;
use dsp::warble::Warble;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TapeSpeed {
    Fast,
    Normal,
    Slow,
}

/// Control-map parameters. `Copy` so it can live in a `Mutex<Cell<Params>>`
/// shared from the control loop to the audio interrupt.
#[derive(Clone, Copy)]
pub struct Params {
    pub drive: f32,  // KNOB_1
    pub warble: f32, // KNOB_2
    pub tone: f32,   // KNOB_3
    pub junk: f32,   // KNOB_4
    pub mix: f32,    // KNOB_5
    pub level: f32,  // KNOB_6
    pub speed: TapeSpeed,
    pub bypass: bool,
    pub slam: bool, // FOOTSWITCH_2 momentary
}

impl Params {
    /// Const default usable as a `static` initializer (the `Default` trait is
    /// not usable in const context).
    pub const DEFAULT: Self = Self {
        drive: 0.3,
        warble: 0.2,
        tone: 0.6,
        junk: 0.2,
        mix: 1.0,
        level: 0.8,
        speed: TapeSpeed::Normal,
        bypass: true,
        slam: false,
    };
}

impl Default for Params {
    fn default() -> Self {
        Self::DEFAULT
    }
}

/// Circular delay line over an SDRAM-backed buffer with linear-interpolated
/// fractional read (for the wow/flutter modulation).
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
    pub fn write(&mut self, x: f32) {
        self.buf[self.write] = x;
        self.write += 1;
        if self.write >= self.buf.len() {
            self.write = 0;
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

pub struct CassetteEngine {
    sat: TapeSat,
    tone: CassetteTone,
    warble: Warble,
    hiss: Hiss,
    dropout: Dropout,
    delay: DelayLine,
    fs: f32,
    base_delay: f32, // samples
}

impl CassetteEngine {
    pub fn new(fs: f32, buf: &'static mut [f32]) -> Self {
        Self {
            sat: TapeSat::new(fs),
            tone: CassetteTone::new(fs),
            warble: Warble::new(fs),
            hiss: Hiss::new(),
            dropout: Dropout::new(fs),
            delay: DelayLine::new(buf),
            fs,
            base_delay: 0.020 * fs,
        }
    }

    /// Re-map parameters (called once per audio block). FOOTSWITCH_2 "slam"
    /// pushes drive / warble / junk toward the wild end.
    pub fn set_params(&mut self, p: &Params) {
        let boost = if p.slam { 0.4 } else { 0.0 };
        self.sat.set_drive((p.drive + boost).min(1.0));
        self.tone.set_bandwidth(p.tone);
        self.warble.set_amount((p.warble + boost).min(1.0));
        let junk = (p.junk + boost).min(1.0);
        self.hiss.set_amount(junk * 0.006);
        self.dropout.set_age(junk, self.fs);

        let (wow, flutter, base_ms) = match p.speed {
            TapeSpeed::Fast => (1.2, 11.0, 0.012),
            TapeSpeed::Normal => (0.8, 8.0, 0.020),
            TapeSpeed::Slow => (0.5, 6.0, 0.030),
        };
        self.warble.set_rates(wow, flutter, self.fs);
        self.base_delay = base_ms * self.fs;
    }

    #[inline]
    pub fn process(&mut self, x: f32, p: &Params) -> f32 {
        if p.bypass {
            return x;
        }
        let sat = self.sat.process(x);
        self.delay.write(sat);

        let m = self.warble.process(); // ~[-1,1] × amount
        let d = self.base_delay * (1.0 + m * 0.02); // ±2% wow/flutter
        let delayed = self.delay.read(d);

        let toned = self.tone.process(delayed);
        let dirty = toned + self.hiss.process();
        let gated = dirty * self.dropout.process();

        let out = x * (1.0 - p.mix) + gated * p.mix;
        out * p.level
    }
}
