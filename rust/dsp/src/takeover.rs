//! Soft-takeover ("pick-up") for paged knobs.
//!
//! On a knob-paged pedal, one physical knob controls several parameters across
//! pages. When the page changes, the knob's physical position no longer matches
//! the parameter it now drives, so taking control immediately would make the
//! value jump. Soft-takeover holds the stored value frozen until the physical
//! knob is swept *through* that value ("picks it up"), after which it tracks
//! normally.
//!
//! Pure logic — host-tested under `cargo test`.

/// How close the knob must be to the stored value to be considered "on it".
/// 0..1 knob domain; ~2% of travel.
const CATCH_EPS: f32 = 0.02;

#[derive(Clone, Copy)]
pub struct SoftTakeover {
    value: f32,
    /// `true` once the physical knob has picked the value up (then it tracks).
    caught: bool,
    /// Sign of (phys - value) captured at activation; used to detect a crossing.
    phys_above: bool,
}

impl SoftTakeover {
    /// Create a slot whose stored value is `initial`; starts caught (the knob is
    /// assumed to match until a page change proves otherwise).
    pub const fn new(initial: f32) -> Self {
        Self {
            value: initial,
            caught: true,
            phys_above: false,
        }
    }

    /// Current stored parameter value.
    #[inline]
    pub fn value(&self) -> f32 {
        self.value
    }

    #[inline]
    pub fn is_caught(&self) -> bool {
        self.caught
    }

    /// Call when this slot becomes the active page's knob. Decides whether the
    /// knob is already on the value (caught) or must pick it up first.
    pub fn activate(&mut self, phys: f32) {
        if (phys - self.value).abs() <= CATCH_EPS {
            self.caught = true;
        } else {
            self.caught = false;
            self.phys_above = phys > self.value;
        }
    }

    /// Feed the live physical knob position while this slot is active; returns
    /// the (possibly still-frozen) stored value. Once the knob crosses the
    /// stored value the slot latches `caught` and tracks the knob thereafter.
    pub fn update(&mut self, phys: f32) -> f32 {
        if self.caught {
            self.value = phys;
        } else {
            // Pick up once the knob reaches or crosses the stored value from the
            // side it was parked on at activation.
            let reached = if self.phys_above {
                phys <= self.value
            } else {
                phys >= self.value
            };
            if reached {
                self.caught = true;
                self.value = phys;
            }
        }
        self.value
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tracks_immediately_when_already_on_value() {
        let mut s = SoftTakeover::new(0.5);
        s.activate(0.51); // within eps
        assert!(s.is_caught());
        assert!((s.update(0.7) - 0.7).abs() < 1e-6);
    }

    #[test]
    fn frozen_until_knob_picks_up_from_below() {
        let mut s = SoftTakeover::new(0.8);
        s.activate(0.2); // far below stored value
        assert!(!s.is_caught());
        // Sweeping up but still below: stays frozen at 0.8.
        assert!((s.update(0.4) - 0.8).abs() < 1e-6);
        assert!((s.update(0.79) - 0.8).abs() < 1e-6);
        assert!(!s.is_caught());
        // Cross the stored value: latch and track.
        let v = s.update(0.85);
        assert!(s.is_caught());
        assert!((v - 0.85).abs() < 1e-6);
    }

    #[test]
    fn frozen_until_knob_picks_up_from_above() {
        let mut s = SoftTakeover::new(0.3);
        s.activate(0.9); // far above
        assert!(!s.is_caught());
        assert!((s.update(0.5) - 0.3).abs() < 1e-6); // still above, frozen
        let v = s.update(0.25); // cross downward
        assert!(s.is_caught());
        assert!((v - 0.25).abs() < 1e-6);
    }

    #[test]
    fn reactivating_can_unlatch() {
        let mut s = SoftTakeover::new(0.5);
        s.activate(0.5);
        s.update(0.6); // caught, value now 0.6
        // Leave page and come back with the knob parked elsewhere.
        s.activate(0.1);
        assert!(!s.is_caught());
        assert!((s.update(0.3) - 0.6).abs() < 1e-6); // frozen at 0.6
    }
}
