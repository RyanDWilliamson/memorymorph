//! Driftwood — dual-engine (TIME + SPACE), Mood-style ambient pedal.
//!
//! TIME engine (looper / delay / tape-slip) → SPACE engine (lo-fi PT2399
//! reverb), animated by an internal MOVEMENT LFO; six knobs paged over
//! TIME / MASTER / SPACE with soft-takeover. See `rust/docs/driftwood-plan.md`.

pub mod engine;
pub mod movement_engine;
pub mod paging;
pub mod params;
pub mod space_engine;
pub mod time_engine;

pub use engine::DriftwoodEngine;
pub use movement_engine::MovementEngine;
pub use paging::PagedKnobs;
pub use params::{MoveTarget, Page, Params, SpaceMode, TimeMode};
pub use space_engine::SpaceEngine;
pub use time_engine::TimeEngine;
