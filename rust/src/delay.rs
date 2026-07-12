//! Re-export of the shared delay line, which lives in the host-testable `dsp`
//! crate (moved there after a float-edge indexing panic in the audio ISR that
//! no firmware-crate code could ever catch on host — see `dsp::delayline`).

pub use dsp::delayline::DelayLine;
