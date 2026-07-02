//! Driftwood signal chain: `IN → [TIME engine] → [SPACE engine] → OUT`.
//!
//! Both modeled engines run in series. Gain staging follows the plan: MASTER
//! input-gain at the front, MASTER output-level at the back, and a
//! waveform-preserving soft limiter protecting the codec. FS1-hold freezes the
//! TIME engine and blooms the SPACE reverb together.

use dsp::looper::LooperInput;

use super::movement_engine::MovementEngine;
use super::params::Params;
use super::space_engine::SpaceEngine;
use super::time_engine::TimeEngine;

pub struct DriftwoodEngine {
    time: TimeEngine,
    space: SpaceEngine,
    movement: MovementEngine,
}

impl DriftwoodEngine {
    /// SDRAM the SPACE reverb network needs (so the caller can partition).
    pub fn reverb_len(fs: f32) -> usize {
        SpaceEngine::reverb_len(fs)
    }

    pub fn new(
        fs: f32,
        time_buf: &'static mut [f32],
        reverb_buf: &'static mut [f32],
        shimmer_buf: &'static mut [f32],
    ) -> Self {
        Self {
            time: TimeEngine::new(fs, time_buf),
            space: SpaceEngine::new(fs, reverb_buf, shimmer_buf),
            movement: MovementEngine::new(fs),
        }
    }

    pub fn set_params(&mut self, p: &Params) {
        self.time.set_params(p);
        self.space.set_params(p, p.freeze); // FS1-hold blooms the reverb
        self.movement.set_params(p);
    }

    /// Forward a looper transport event to the TIME engine (LOOPER mode).
    pub fn looper_transport(&mut self, input: LooperInput) {
        self.time.looper_transport(input);
    }

    /// Re-align the MOVEMENT LFO to a tap (tap-sync).
    pub fn tap_sync(&mut self) {
        self.movement.reset();
    }

    #[inline]
    pub fn process(&mut self, x: f32, p: &Params) -> f32 {
        if p.bypass {
            return x; // unity dry pass-through
        }
        self.movement.tick();
        let g_in = 0.5 + 1.5 * p.input_gain(); // ~0.5..2.0 operating point
        let t = self.time.process(x * g_in, self.movement.vib());
        // DIAGNOSTIC (freeze hunt): SPACE bypassed to test whether the reverb is
        // the cause. If the pedal engages cleanly with this, the freeze is in the
        // reverb; if it still freezes, the cause is MOVEMENT/wiring, not SPACE.
        let _ = self.movement.send_gain();
        let s = t; // self.space.process(t, self.movement.send_gain());
        let out = self.movement.apply_amp(s);
        soft_limit(out * p.output_level())
    }
}

/// Waveform-preserving soft limiter (monotonic over ±1.4) keeping the output
/// inside the codec's range during freeze/havoc blooms.
#[inline]
fn soft_limit(x: f32) -> f32 {
    let a = x.clamp(-1.4, 1.4);
    a - (a * a * a) * (1.0 / 6.0)
}
