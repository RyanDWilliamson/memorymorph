//! TIME engine — the Footswitch-1 side of Driftwood.
//!
//! One engine, three behaviours selected by TOGGLE_2 ([`TimeMode`]):
//! - **Delay** — BBD-modeled feedback delay; sweeping the time knob glides pitch
//!   (varispeed) via a slewed fractional read.
//! - **Tape-slip** — the same delay with high sustain and a slow wandering drift.
//! - **Looper** — hands-free record / play / overdub over the SDRAM buffer; its
//!   transport is driven from the control loop ([`looper_transport`]).
//!
//! FOOTSWITCH_1 hold = momentary **freeze / havoc** in the delay modes (input
//! muted, feedback pushed past unity into a soft-clipped self-oscillating bloom).
//!
//! [`looper_transport`]: TimeEngine::looper_transport

use dsp::bbd::Bbd;
use dsp::fastmath;
use dsp::looper::{Looper, LooperAction, LooperInput};
use dsp::warble::Warble;
use dsp::one_pole_coeff;
use libm::powf;

use super::params::{Params, TimeMode};
use crate::delay::DelayLine;

const MIN_DELAY_S: f32 = 0.05;
const MAX_DELAY_S: f32 = 0.90;
/// Freeze/havoc feedback — just past unity so it blooms, held in check by the
/// BBD soft clip rather than running away to infinity.
const HAVOC_FB: f32 = 1.015;
const SLIP_DRIFT_HZ: f32 = 0.13;
/// Overdub layer decay, so stacked passes don't build up without bound.
const OVERDUB_DECAY: f32 = 0.96;

pub struct TimeEngine {
    fs: f32,
    delay: DelayLine,
    bbd: Bbd,
    warble: Warble,

    mode: TimeMode,
    freeze: bool,

    // Varispeed: slewed fractional delay length (samples).
    target_delay: f32,
    cur_delay: f32,
    delay_slew: f32,

    // Smoothed feedback.
    feedback: f32,
    fb_target: f32,

    drive: f32,
    mix: f32,
    level: f32,

    slip_phase: f32,

    // Looper.
    looper: Looper,
    loop_len: usize,
    loop_pos: usize,
    rec_pos: usize,
}

impl TimeEngine {
    pub fn new(fs: f32, buf: &'static mut [f32]) -> Self {
        let mut warble = Warble::new(fs);
        warble.set_rates(0.8, 8.0, fs);
        Self {
            fs,
            delay: DelayLine::new(buf),
            bbd: Bbd::new(fs),
            warble,
            mode: TimeMode::Delay,
            freeze: false,
            target_delay: 0.3 * fs,
            cur_delay: 0.3 * fs,
            delay_slew: one_pole_coeff(5.0, fs),
            feedback: 0.3,
            fb_target: 0.3,
            drive: 0.3,
            mix: 0.4,
            level: 0.8,
            slip_phase: 0.0,
            looper: Looper::new(),
            loop_len: 0,
            loop_pos: 0,
            rec_pos: 0,
        }
    }

    /// Map parameters once per audio block.
    pub fn set_params(&mut self, p: &Params) {
        self.mode = p.time_mode;
        self.freeze = p.freeze;

        // Tap tempo overrides the knob until the knob is next moved.
        let ds = if p.tap_delay_s > 0.0 {
            p.tap_delay_s.clamp(MIN_DELAY_S, MAX_DELAY_S)
        } else {
            MIN_DELAY_S * powf(MAX_DELAY_S / MIN_DELAY_S, p.time_time())
        };
        self.target_delay = (ds * self.fs).clamp(1.0, (self.delay.len() - 2) as f32);

        self.fb_target = match self.mode {
            TimeMode::TapeSlip => 0.86 + 0.12 * p.time_repeats(),
            _ => 0.95 * p.time_repeats(),
        };

        self.drive = p.time_drive();
        self.mix = p.time_mix();
        self.level = p.time_level();

        self.warble.set_amount(p.time_warble());
        self.bbd.set_noise(0.0005 + self.drive * 0.003);
        self.bbd
            .set_time(self.cur_delay / self.fs, self.fb_target.min(1.0));
    }

    /// Current looper transport state (for LED feedback).
    pub fn looper_state(&self) -> dsp::looper::LooperState {
        self.looper.state()
    }

    /// Apply a looper transport event (called from the control loop, under a
    /// critical section). Only meaningful in [`TimeMode::Looper`].
    pub fn looper_transport(&mut self, input: LooperInput) {
        match self.looper.step(input) {
            LooperAction::StartRecording => {
                self.rec_pos = 0;
                self.loop_len = 0;
            }
            LooperAction::CloseLoopAndPlay => self.close_loop(),
            LooperAction::Clear => self.clear_loop(),
            LooperAction::None => {}
        }
    }

    /// `vib` is an external vibrato fraction (MOVEMENT, time target); 0 when
    /// movement is off or targeting something else.
    #[inline]
    pub fn process(&mut self, x: f32, vib: f32) -> f32 {
        // Varispeed glide + feedback smoothing.
        self.cur_delay += self.delay_slew * (self.target_delay - self.cur_delay);
        self.feedback += 0.002 * (self.fb_target - self.feedback);

        match self.mode {
            TimeMode::Looper => self.process_looper(x),
            _ => self.process_delay(x, vib),
        }
    }

    #[inline]
    fn process_delay(&mut self, x: f32, vib: f32) -> f32 {
        let w = self.warble.process(); // [-1,1] × amount
        let mut mod_frac = w * 0.02 + vib; // ±2% wow/flutter + movement vibrato
        if self.mode == TimeMode::TapeSlip {
            self.slip_phase += SLIP_DRIFT_HZ / self.fs;
            if self.slip_phase >= 1.0 {
                self.slip_phase -= 1.0;
            }
            mod_frac += 0.03 * fastmath::sin_01(self.slip_phase); // slow wander
        }

        let read_samples = self.cur_delay * (1.0 + mod_frac);
        let read_raw = self.delay.read(read_samples);
        let wet = self.bbd.post(read_raw);

        let (fb, input) = if self.freeze {
            (HAVOC_FB, 0.0)
        } else {
            (self.feedback, x)
        };
        let drive_gain = 1.0 + 3.0 * self.drive;
        let write_in = self.bbd.pre(input * drive_gain + wet * fb);
        self.delay.write(write_in);

        (x * (1.0 - self.mix) + wet * self.mix) * self.level
    }

    #[inline]
    fn process_looper(&mut self, x: f32) -> f32 {
        let cap = self.delay.len();

        if self.loop_len == 0 {
            // Empty (or recording the first pass): monitor the input.
            if self.looper.is_recording() && self.rec_pos < cap {
                self.delay.poke(self.rec_pos, x);
                self.rec_pos += 1;
            }
            return x * self.level;
        }

        let play = self.delay.peek(self.loop_pos);
        if self.looper.is_overdubbing() {
            self.delay
                .poke(self.loop_pos, play * OVERDUB_DECAY + x);
        }
        self.loop_pos += 1;
        if self.loop_pos >= self.loop_len {
            self.loop_pos = 0;
        }

        (x * (1.0 - self.mix) + play * self.mix) * self.level
    }

    fn close_loop(&mut self) {
        self.loop_len = self.rec_pos.max(1);
        self.loop_pos = 0;
    }

    fn clear_loop(&mut self) {
        self.delay.clear_region(0, self.loop_len);
        self.loop_len = 0;
        self.loop_pos = 0;
        self.rec_pos = 0;
    }
}
