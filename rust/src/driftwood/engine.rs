//! Driftwood signal chain: `IN → [TIME engine] → [SPACE engine] → OUT`.
//!
//! Phase 3: the TIME engine (BBD delay / tape-slip / looper) is live. The SPACE
//! reverb is still a pass-through and arrives in Phase 4. Gain staging follows
//! the plan: MASTER input-gain at the front, MASTER output-level at the back,
//! and a waveform-preserving soft limiter protecting the codec.

use dsp::looper::LooperInput;

use super::params::Params;
use super::time_engine::TimeEngine;

pub struct DriftwoodEngine {
    fs: f32,
    time: TimeEngine,
}

impl DriftwoodEngine {
    pub fn new(fs: f32, buf: &'static mut [f32]) -> Self {
        Self {
            fs,
            time: TimeEngine::new(fs, buf),
        }
    }

    pub fn set_params(&mut self, p: &Params) {
        let _ = self.fs;
        self.time.set_params(p);
    }

    /// Forward a looper transport event to the TIME engine (LOOPER mode).
    pub fn looper_transport(&mut self, input: LooperInput) {
        self.time.looper_transport(input);
    }

    #[inline]
    pub fn process(&mut self, x: f32, p: &Params) -> f32 {
        if p.bypass {
            return x; // unity dry pass-through
        }
        let g_in = 0.5 + 1.5 * p.input_gain(); // ~0.5..2.0 operating point
        let t = self.time.process(x * g_in);
        // SPACE engine: pass-through until Phase 4.
        let out = t * p.output_level();
        soft_limit(out)
    }
}

/// Waveform-preserving soft limiter (monotonic over ±1.4) keeping the output
/// inside the codec's range during freeze/havoc blooms.
#[inline]
fn soft_limit(x: f32) -> f32 {
    let a = x.clamp(-1.4, 1.4);
    a - (a * a * a) * (1.0 / 6.0)
}
