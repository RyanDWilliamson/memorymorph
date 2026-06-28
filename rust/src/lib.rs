//! Hothouse pedal platform — shared library for every pedal binary.
//!
//! Imported by each `src/bin/*.rs` (and the historical `src/main.rs` cassette
//! firmware) as the crate `hothouse`:
//!
//! ```ignore
//! use hothouse::board::{Controls, Clock, FootswitchTracker};
//! use hothouse::driftwood::{DriftwoodEngine, Params};
//! ```
//!
//! - [`board`] — Hothouse control-surface HAL (knobs, toggles, footswitches,
//!   LEDs, clock, footswitch state machine, DFU gesture).
//! - [`cassette`] — Cassette LoFi Junky signal chain.
//! - [`driftwood`] — Driftwood dual-engine (TIME + SPACE + MOVEMENT) pedal.

#![no_std]

pub mod board;
pub mod cassette;
pub mod delay;
pub mod driftwood;
