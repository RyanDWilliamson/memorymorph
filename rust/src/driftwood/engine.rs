//! Driftwood signal chain: `IN → [TIME engine] → [SPACE engine] → OUT`.
//!
//! **Phase 1 scaffold:** this is a wiring stub — clean dry pass-through with the
//! MASTER output-level knob and true bypass, so the control surface, paging,
//! footswitches, LEDs and DFU can be bench-verified before any DSP lands. The
//! modeled BBD/tape TIME engine, PT2399 SPACE reverb and MOVEMENT LFO arrive in
//! later phases and will hang off the SDRAM buffer reserved here.

use super::params::Params;

pub struct DriftwoodEngine {
    fs: f32,
    /// SDRAM-backed audio memory, reserved for the TIME engine's delay/loop and
    /// the SPACE reverb network (unused in the Phase 1 scaffold).
    buf: &'static mut [f32],
}

impl DriftwoodEngine {
    pub fn new(fs: f32, buf: &'static mut [f32]) -> Self {
        for s in buf.iter_mut() {
            *s = 0.0;
        }
        Self { fs, buf }
    }

    /// Re-map parameters once per audio block. (No-op in the scaffold.)
    pub fn set_params(&mut self, _p: &Params) {
        let _ = self.fs;
        let _ = self.buf.len();
    }

    #[inline]
    pub fn process(&mut self, x: f32, p: &Params) -> f32 {
        if p.bypass {
            return x; // unity dry pass-through
        }
        x * p.output_level()
    }
}
