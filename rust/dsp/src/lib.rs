//! Cassette LoFi Junky DSP primitives, hand-ported from the C++ references in
//! `../../src` (Memory Morph) and the `echorec-refinements` branch (Echorec).
//!
//! `#![no_std]` on the firmware target; under `cargo test` the crate links
//! `std` so the same code can be unit-tested on the host. All transcendental
//! math goes through `libm`, which builds in both environments, so there is no
//! cfg-gated divergence between what runs on hardware and what the tests check.

#![cfg_attr(not(test), no_std)]

mod onepole;
pub mod bbd;
pub mod lofi;
pub mod saturation;
pub mod takeover;
pub mod tone;
pub mod warble;

pub use onepole::{one_pole_coeff, OnePole};

pub(crate) const TWO_PI: f32 = core::f32::consts::TAU;
