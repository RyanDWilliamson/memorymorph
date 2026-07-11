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
    /// Input operating-point gain (~0.5..2.0), computed at block rate.
    g_in: f32,
    /// Output level, computed at block rate.
    g_out: f32,
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
            g_in: 0.35 + 0.9 * Params::DEFAULT_KNOBS[1][3],
            g_out: Params::DEFAULT_KNOBS[1][5],
        }
    }

    pub fn set_params(&mut self, p: &Params) {
        self.time.set_params(p);
        self.space.set_params(p); // reads p.freeze itself: FS1-hold blooms
        self.movement.set_params(p);
        // 0.35..1.25 operating point: enough trim for quiet/hot pickups without
        // slamming the modeled stages into their rails (gain-staging plan).
        self.g_in = 0.35 + 0.9 * p.input_gain();
        self.g_out = p.output_level();
    }

    /// Forward a looper transport event to the TIME engine (LOOPER mode).
    pub fn looper_transport(&mut self, input: LooperInput) {
        self.time.looper_transport(input);
    }

    /// Current looper transport state (for LED feedback).
    pub fn looper_state(&self) -> dsp::looper::LooperState {
        self.time.looper_state()
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
        let t = self.time.process(x * self.g_in, self.movement.vib());
        let s = self.space.process(t, self.movement.send_gain());
        let out = self.movement.apply_amp(s);
        soft_limit(out * self.g_out)
    }
}

/// Waveform-preserving soft limiter (monotonic over ±1.4) keeping the output
/// inside the codec's range during freeze/havoc blooms. Also the chain's NaN
/// firewall: `clamp` passes NaN through untouched, and a NaN reaching the codec
/// converts to 0 — a silently muted pedal. Better one zeroed sample than a
/// poisoned feedback chain presenting as "no audio".
#[inline]
fn soft_limit(x: f32) -> f32 {
    if !x.is_finite() {
        return 0.0;
    }
    let a = x.clamp(-1.4, 1.4);
    a - (a * a * a) * (1.0 / 6.0)
}
