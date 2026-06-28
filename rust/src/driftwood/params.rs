//! Driftwood control model: knob pages, engine modes, and the resolved
//! parameter snapshot shared from the control loop to the audio ISR.

use crate::board::TogglePosition;

/// Knob page selected by TOGGLE_1. Each physical knob keeps its semantic family
/// across pages (time→time, mix→mix, drive→drive); see the design plan.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Page {
    Time,
    Master,
    Space,
}

impl Page {
    pub fn from_toggle(t: TogglePosition) -> Self {
        match t {
            TogglePosition::Up => Page::Time,
            TogglePosition::Middle => Page::Master,
            TogglePosition::Down => Page::Space,
        }
    }

    #[inline]
    pub fn index(self) -> usize {
        match self {
            Page::Time => 0,
            Page::Master => 1,
            Page::Space => 2,
        }
    }
}

/// TIME engine behaviour (TOGGLE_2).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TimeMode {
    Looper,
    Delay,
    TapeSlip,
}

impl TimeMode {
    pub fn from_toggle(t: TogglePosition) -> Self {
        match t {
            TogglePosition::Up => TimeMode::Looper,
            TogglePosition::Middle => TimeMode::Delay,
            TogglePosition::Down => TimeMode::TapeSlip,
        }
    }
}

/// SPACE engine character (TOGGLE_3).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SpaceMode {
    Dark,
    Modulated,
    Shimmer,
}

impl SpaceMode {
    pub fn from_toggle(t: TogglePosition) -> Self {
        match t {
            TogglePosition::Up => SpaceMode::Dark,
            TogglePosition::Middle => SpaceMode::Modulated,
            TogglePosition::Down => SpaceMode::Shimmer,
        }
    }
}

/// Resolved parameter snapshot (soft-takeover already applied). `Copy` so it can
/// live in a `Mutex<Cell<Params>>` handed from the control loop to the ISR.
///
/// `knobs[page][knob]` holds the stored 0..1 value for every page/knob slot, so
/// both engines can read their parameters regardless of which page is currently
/// being edited. Named accessors below follow the knob-family table.
#[derive(Clone, Copy)]
pub struct Params {
    pub knobs: [[f32; 6]; 3],
    pub time_mode: TimeMode,
    pub space_mode: SpaceMode,
    pub bypass: bool,
    pub freeze: bool,
}

impl Params {
    /// Sensible power-on values per [page][knob] slot.
    pub const DEFAULT_KNOBS: [[f32; 6]; 3] = [
        // TIME:   time  repeats warble drive  mix   level
        [0.30, 0.30, 0.20, 0.30, 0.40, 0.80],
        // MASTER: rate  depth   shape  inGain target outLvl
        [0.30, 0.30, 0.00, 0.50, 0.00, 0.80],
        // SPACE:  decay regen   mod    age    mix    tone
        [0.40, 0.30, 0.20, 0.20, 0.30, 0.60],
    ];

    pub const DEFAULT: Self = Self {
        knobs: Self::DEFAULT_KNOBS,
        time_mode: TimeMode::Delay,
        space_mode: SpaceMode::Modulated,
        bypass: true,
        freeze: false,
    };

    // ── MASTER page (global) ────────────────────────────────────────────────
    #[inline]
    pub fn input_gain(&self) -> f32 {
        self.knobs[Page::Master.index()][3]
    }
    #[inline]
    pub fn output_level(&self) -> f32 {
        self.knobs[Page::Master.index()][5]
    }
}

impl Default for Params {
    fn default() -> Self {
        Self::DEFAULT
    }
}
