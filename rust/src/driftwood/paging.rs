//! Paged knob bank with soft-takeover.
//!
//! The six physical knobs edit whichever [`Page`] TOGGLE_1 selects. Each
//! page/knob slot keeps its own stored value (frozen while another page is
//! active), and uses [`SoftTakeover`] so values never jump when the page
//! changes — the knob must sweep through the stored value to pick it up.

use dsp::takeover::SoftTakeover;

use super::params::Page;

pub struct PagedKnobs {
    slots: [[SoftTakeover; 6]; 3],
    current: Page,
}

impl PagedKnobs {
    /// Build from an initial `[page][knob]` value matrix (e.g.
    /// `Params::DEFAULT_KNOBS`).
    pub fn new(init: [[f32; 6]; 3]) -> Self {
        let mut slots = [[SoftTakeover::new(0.0); 6]; 3];
        for (pg, row) in init.iter().enumerate() {
            for (i, &v) in row.iter().enumerate() {
                slots[pg][i] = SoftTakeover::new(v);
            }
        }
        Self {
            slots,
            current: Page::Time,
        }
    }

    /// One control-tick update. On a page change, re-arms soft-takeover for the
    /// newly active page; then feeds the live knobs to the active page's slots.
    /// Returns the full stored `[page][knob]` matrix for [`Params`].
    pub fn update(&mut self, page: Page, knobs: &[f32; 6]) -> [[f32; 6]; 3] {
        if page != self.current {
            self.current = page;
            let p = page.index();
            for (slot, &k) in self.slots[p].iter_mut().zip(knobs.iter()) {
                slot.activate(k);
            }
        }
        let p = page.index();
        for (slot, &k) in self.slots[p].iter_mut().zip(knobs.iter()) {
            slot.update(k);
        }

        let mut out = [[0.0f32; 6]; 3];
        for (pg, row) in out.iter_mut().enumerate() {
            for (i, v) in row.iter_mut().enumerate() {
                // Quantize to 1/512 steps: swallows ADC noise (so equality-based
                // block-rate memoization in the engines actually hits) while
                // staying well below audible parameter resolution.
                *v = dsp::fastmath::round(self.slots[pg][i].value() * 512.0) / 512.0;
            }
        }
        out
    }
}
