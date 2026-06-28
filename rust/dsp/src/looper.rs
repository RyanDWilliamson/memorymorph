//! Looper transport state machine (pure logic; host-tested).
//!
//! Drives the TIME engine's LOOPER mode. The buffer I/O lives in the firmware
//! engine — this is only the transport: what a short press or a hold does in
//! each state, and what action the engine should take.
//!
//! Mode-dependent FOOTSWITCH_1 (LOOPER): short press cycles
//! `record → play → overdub → play → overdub …`; hold stops and clears.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LooperState {
    /// No loop captured; passing input through.
    Empty,
    /// Capturing the loop (writing, length growing).
    Recording,
    /// Looping playback of the captured buffer.
    Playing,
    /// Looping playback while summing input into the buffer.
    Overdubbing,
}

/// Transport input for one control tick.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LooperInput {
    None,
    /// Debounced short press of FOOTSWITCH_1.
    ShortPress,
    /// Long-press (hold) of FOOTSWITCH_1.
    Hold,
}

/// What the engine should do as a result of a transition. Returned alongside the
/// new state so the engine can react (set the loop length, clear, etc.).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LooperAction {
    None,
    /// Begin recording from buffer position 0.
    StartRecording,
    /// Latch the loop length to the current record position and start playback.
    CloseLoopAndPlay,
    /// Clear the buffer and return to Empty.
    Clear,
}

pub struct Looper {
    state: LooperState,
}

impl Default for Looper {
    fn default() -> Self {
        Self::new()
    }
}

impl Looper {
    pub const fn new() -> Self {
        Self {
            state: LooperState::Empty,
        }
    }

    #[inline]
    pub fn state(&self) -> LooperState {
        self.state
    }

    #[inline]
    pub fn is_recording(&self) -> bool {
        self.state == LooperState::Recording
    }

    #[inline]
    pub fn is_overdubbing(&self) -> bool {
        self.state == LooperState::Overdubbing
    }

    /// Whether playback should be sounding the captured loop.
    #[inline]
    pub fn is_playing(&self) -> bool {
        matches!(self.state, LooperState::Playing | LooperState::Overdubbing)
    }

    /// Advance the transport. Returns the action the engine must perform.
    pub fn step(&mut self, input: LooperInput) -> LooperAction {
        use LooperInput::*;
        use LooperState::*;

        match (self.state, input) {
            // A hold always stops and clears, from any non-empty state.
            (Empty, Hold) => LooperAction::None,
            (_, Hold) => {
                self.state = Empty;
                LooperAction::Clear
            }

            (Empty, ShortPress) => {
                self.state = Recording;
                LooperAction::StartRecording
            }
            (Recording, ShortPress) => {
                self.state = Playing;
                LooperAction::CloseLoopAndPlay
            }
            (Playing, ShortPress) => {
                self.state = Overdubbing;
                LooperAction::None
            }
            (Overdubbing, ShortPress) => {
                self.state = Playing;
                LooperAction::None
            }

            (_, None) => LooperAction::None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn records_then_plays_then_overdubs() {
        let mut l = Looper::new();
        assert_eq!(l.state(), LooperState::Empty);

        assert_eq!(l.step(LooperInput::ShortPress), LooperAction::StartRecording);
        assert!(l.is_recording());

        assert_eq!(
            l.step(LooperInput::ShortPress),
            LooperAction::CloseLoopAndPlay
        );
        assert!(l.is_playing() && !l.is_overdubbing());

        assert_eq!(l.step(LooperInput::ShortPress), LooperAction::None);
        assert!(l.is_overdubbing() && l.is_playing());

        assert_eq!(l.step(LooperInput::ShortPress), LooperAction::None);
        assert!(l.is_playing() && !l.is_overdubbing());
    }

    #[test]
    fn hold_clears_from_any_state() {
        for setup in 0..4 {
            let mut l = Looper::new();
            for _ in 0..setup {
                l.step(LooperInput::ShortPress);
            }
            let action = l.step(LooperInput::Hold);
            if setup == 0 {
                assert_eq!(action, LooperAction::None); // nothing to clear when Empty
            } else {
                assert_eq!(action, LooperAction::Clear);
            }
            assert_eq!(l.state(), LooperState::Empty);
        }
    }

    #[test]
    fn none_is_inert() {
        let mut l = Looper::new();
        l.step(LooperInput::ShortPress); // Recording
        assert_eq!(l.step(LooperInput::None), LooperAction::None);
        assert!(l.is_recording());
    }
}
