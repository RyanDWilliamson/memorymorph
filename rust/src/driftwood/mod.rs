//! Driftwood — dual-engine (TIME + SPACE), Mood-style ambient pedal.
//!
//! TIME engine (looper / delay / tape-slip) → SPACE engine (lo-fi PT2399
//! reverb), animated by an internal MOVEMENT LFO; six knobs paged over
//! TIME / MASTER / SPACE with soft-takeover. See `rust/docs/driftwood-plan.md`.

pub mod engine;
pub mod paging;
pub mod params;
pub mod time_engine;

pub use engine::DriftwoodEngine;
pub use paging::PagedKnobs;
pub use params::{Page, Params, SpaceMode, TimeMode};
pub use time_engine::TimeEngine;
