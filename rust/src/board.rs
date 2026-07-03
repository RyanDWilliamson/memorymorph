// Shared platform module used by several binaries (cassette, driftwood, bringup,
// validate); each uses a different subset of the API, so per-binary dead-code
// warnings are normal noise.
#![allow(dead_code)]

//! Hothouse hardware abstraction — Rust port of `../../src/hothouse.{h,cpp}`.
//!
//! Pin map (Daisy Seed silkscreen → `daisy::pins::Gpio` field → STM32 pin):
//! ```text
//!   Knob 1..6   : D16..D21  PIN_16..21  PA3,PB1,PA7,PA6,PC1,PC4   (all ADC1)
//!   Toggle 1 u/d: D9 / D10   PIN_9/PIN_10   (pull-up, active low)
//!   Toggle 2 u/d: D7 / D8    PIN_7/PIN_8
//!   Toggle 3 u/d: D5 / D6    PIN_5/PIN_6
//!   Footswitch 1: D25        PIN_25         (pull-up, active low)
//!   Footswitch 2: D26        PIN_26
//!   LED 1 / 2   : D22 / D23   PIN_22/PIN_23  PA5/PA4  (push-pull output)
//! ```
//! Toggles are ON-OFF-ON (centre = neither side closed). Footswitches and
//! toggle legs read low when engaged because they short the pulled-up pin to GND.

use stm32h7xx_hal as hal;

use hal::adc::{Adc, Enabled};
use hal::delay::Delay;
use hal::gpio::{self, Analog, ErasedPin, Input, Output, PushPull};
use hal::pac;
use hal::prelude::*;

pub const NUM_KNOBS: usize = 6;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TogglePosition {
    Up,
    Middle,
    Down,
}

/// Snapshot of every control for one control-loop tick.
#[derive(Clone, Copy)]
pub struct ControlState {
    /// Knob positions, 0.0..=1.0, in KNOB_1..KNOB_6 order.
    pub knobs: [f32; NUM_KNOBS],
    /// Toggle positions in TOGGLE_1..TOGGLE_3 order.
    pub toggles: [TogglePosition; 3],
    /// Raw (debounced upstream) footswitch state: true = pressed.
    pub footswitches: [bool; 2],
}

// ── Monotonic clock (DWT cycle counter) ─────────────────────────────────────
// CYCCNT wraps every 2^32 cycles (~8.9 s at 480 MHz); all durations are computed
// with `wrapping_sub` on raw cycles, so any window shorter than that is correct.

pub struct Clock {
    cycles_per_ms: u32,
}

impl Clock {
    pub fn new(
        sysclk_hz: u32,
        dcb: &mut cortex_m::peripheral::DCB,
        mut dwt: cortex_m::peripheral::DWT,
    ) -> Self {
        dcb.enable_trace();
        dwt.enable_cycle_counter();
        Self {
            cycles_per_ms: (sysclk_hz / 1000).max(1),
        }
    }

    #[inline]
    pub fn now_cycles(&self) -> u32 {
        cortex_m::peripheral::DWT::cycle_count()
    }

    /// Milliseconds elapsed since a previously captured cycle count.
    #[inline]
    pub fn elapsed_ms(&self, since_cycles: u32) -> u32 {
        self.now_cycles().wrapping_sub(since_cycles) / self.cycles_per_ms
    }
}

// ── Footswitch state machine (debounce + long-press) ────────────────────────

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FootswitchEvent {
    None,
    /// Debounced press edge (button went down).
    Pressed,
    /// Debounced release edge (button came up before the long-press window).
    Released,
    /// Held past `LONG_PRESS_MS`; fires once per hold.
    LongPress,
}

const DEBOUNCE_MS: u32 = 5;
pub const LONG_PRESS_MS: u32 = 2000;

pub struct FootswitchTracker {
    debounced: [bool; 2],
    last_raw: [bool; 2],
    last_change_cycles: [u32; 2],
    press_start_cycles: [u32; 2],
    long_fired: [bool; 2],
}

impl Default for FootswitchTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl FootswitchTracker {
    pub const fn new() -> Self {
        Self {
            debounced: [false; 2],
            last_raw: [false; 2],
            last_change_cycles: [0; 2],
            press_start_cycles: [0; 2],
            long_fired: [false; 2],
        }
    }

    /// Feed the current raw footswitch readings and the clock; returns a
    /// per-footswitch event. Call every control tick.
    pub fn update(&mut self, raw: [bool; 2], clock: &Clock) -> [FootswitchEvent; 2] {
        let now = clock.now_cycles();
        let mut events = [FootswitchEvent::None; 2];

        for i in 0..2 {
            if raw[i] != self.last_raw[i] {
                self.last_raw[i] = raw[i];
                self.last_change_cycles[i] = now;
            }

            let stable = clock.elapsed_ms(self.last_change_cycles[i]) >= DEBOUNCE_MS;
            if stable && raw[i] != self.debounced[i] {
                self.debounced[i] = raw[i];
                if raw[i] {
                    self.press_start_cycles[i] = now;
                    self.long_fired[i] = false;
                    events[i] = FootswitchEvent::Pressed;
                } else if !self.long_fired[i] {
                    events[i] = FootswitchEvent::Released;
                }
            }

            // Long-press fires while still held.
            if self.debounced[i]
                && !self.long_fired[i]
                && clock.elapsed_ms(self.press_start_cycles[i]) >= LONG_PRESS_MS
            {
                self.long_fired[i] = true;
                events[i] = FootswitchEvent::LongPress;
            }
        }
        events
    }
}

// ── Controls ────────────────────────────────────────────────────────────────

pub struct Controls {
    adc1: Adc<pac::ADC1, Enabled>,
    adc_max: f32,
    // Knob pins (heterogeneous concrete types — stored individually).
    k1: gpio::gpioa::PA3<Analog>, // D16
    k2: gpio::gpiob::PB1<Analog>, // D17
    k3: gpio::gpioa::PA7<Analog>, // D18
    k4: gpio::gpioa::PA6<Analog>, // D19
    k5: gpio::gpioc::PC1<Analog>, // D20
    k6: gpio::gpioc::PC4<Analog>, // D21
    // [sw1_up, sw1_dn, sw2_up, sw2_dn, sw3_up, sw3_dn, fsw1, fsw2]
    sw: [ErasedPin<Input>; 8],
    leds: [ErasedPin<Output<PushPull>>; 2],
}

impl Controls {
    /// Build the control surface from the board's split GPIOs and ADC1.
    ///
    /// `gpio` is `daisy::board_split_gpios!(...).GPIO`. `delay` is only used for
    /// the ADC power-up/calibration wait during init.
    pub fn new(
        gpio: daisy::pins::Gpio,
        adc1_periph: pac::ADC1,
        adc12_prec: hal::rcc::rec::Adc12,
        clocks: &hal::rcc::CoreClocks,
        delay: &mut Delay,
    ) -> Self {
        let mut adc1 = Adc::adc1(adc1_periph, 4.MHz(), delay, adc12_prec, clocks).enable();
        adc1.set_resolution(hal::adc::Resolution::SixteenBit);
        let adc_max = adc1.slope() as f32; // full-scale count (1 << bits) = 65536

        let sw: [ErasedPin<Input>; 8] = [
            gpio.PIN_9.into_pull_up_input().erase(),  // SW1 up
            gpio.PIN_10.into_pull_up_input().erase(), // SW1 down
            gpio.PIN_7.into_pull_up_input().erase(),  // SW2 up
            gpio.PIN_8.into_pull_up_input().erase(),  // SW2 down
            gpio.PIN_5.into_pull_up_input().erase(),  // SW3 up
            gpio.PIN_6.into_pull_up_input().erase(),  // SW3 down
            gpio.PIN_25.into_pull_up_input().erase(), // FSW1
            gpio.PIN_26.into_pull_up_input().erase(), // FSW2
        ];

        let leds: [ErasedPin<Output<PushPull>>; 2] = [
            gpio.PIN_22.into_push_pull_output().erase(), // LED1
            gpio.PIN_23.into_push_pull_output().erase(), // LED2
        ];

        Controls {
            adc1,
            adc_max,
            k1: gpio.PIN_16.into_analog(),
            k2: gpio.PIN_17.into_analog(),
            k3: gpio.PIN_18.into_analog(),
            k4: gpio.PIN_19.into_analog(),
            k5: gpio.PIN_20.into_analog(),
            k6: gpio.PIN_21.into_analog(),
            sw,
            leds,
        }
    }

    /// Read all knobs and switches into a snapshot. Blocking ADC reads (~µs each).
    pub fn read(&mut self) -> ControlState {
        let knobs = [
            self.adc1.read(&mut self.k1).unwrap_or(0u32) as f32 / self.adc_max,
            self.adc1.read(&mut self.k2).unwrap_or(0u32) as f32 / self.adc_max,
            self.adc1.read(&mut self.k3).unwrap_or(0u32) as f32 / self.adc_max,
            self.adc1.read(&mut self.k4).unwrap_or(0u32) as f32 / self.adc_max,
            self.adc1.read(&mut self.k5).unwrap_or(0u32) as f32 / self.adc_max,
            self.adc1.read(&mut self.k6).unwrap_or(0u32) as f32 / self.adc_max,
        ];

        let toggles = [
            toggle_pos(self.sw[0].is_low(), self.sw[1].is_low()),
            toggle_pos(self.sw[2].is_low(), self.sw[3].is_low()),
            toggle_pos(self.sw[4].is_low(), self.sw[5].is_low()),
        ];

        let footswitches = [self.sw[6].is_low(), self.sw[7].is_low()];

        ControlState {
            knobs,
            toggles,
            footswitches,
        }
    }

    /// Set a footswitch LED (0 = LED1, 1 = LED2).
    pub fn set_led(&mut self, index: usize, on: bool) {
        if on {
            self.leds[index].set_high();
        } else {
            self.leds[index].set_low();
        }
    }
}

#[inline]
fn toggle_pos(up_low: bool, down_low: bool) -> TogglePosition {
    if up_low {
        TogglePosition::Up
    } else if down_low {
        TogglePosition::Down
    } else {
        TogglePosition::Middle
    }
}

/// Hold time for the footswitch DFU gesture.
pub const BOOTLOADER_HOLD_MS: u32 = 1500;

/// Firmware DFU-entry gesture — **mandatory on every Hothouse firmware**: hold
/// BOTH footswitches with the wet/dry (KNOB_5 / Mix) fully dry for ~1.5 s. Lets
/// the player reach the bootloader without opening the enclosure.
pub struct BootloaderGesture {
    active_since: Option<u32>,
}

impl Default for BootloaderGesture {
    fn default() -> Self {
        Self::new()
    }
}

impl BootloaderGesture {
    pub const fn new() -> Self {
        Self { active_since: None }
    }

    /// Returns true exactly when the gesture has been held long enough.
    pub fn update(&mut self, state: &ControlState, clock: &Clock) -> bool {
        let engaged = state.footswitches[0]
            && state.footswitches[1]
            && state.knobs[4] < 0.05; // KNOB_5 (Mix) fully dry
        match (engaged, self.active_since) {
            (true, None) => {
                self.active_since = Some(clock.now_cycles());
                false
            }
            (true, Some(t)) => clock.elapsed_ms(t) >= BOOTLOADER_HOLD_MS,
            (false, _) => {
                self.active_since = None;
                false
            }
        }
    }

    /// While the gesture is currently engaged (both footswitches + KNOB_5 dry),
    /// returns Some(elapsed_ms) — i.e. how long it's been held. Returns None
    /// when not engaged. Lets callers drive LED feedback that confirms detection.
    pub fn hold_progress_ms(&self, clock: &Clock) -> Option<u32> {
        self.active_since.map(|t| clock.elapsed_ms(t))
    }
}

// ── Bootloader entry: drive BOOT0 (PG3) HIGH, then chip-reset ─────────────
//
// Discovery on 2026-06-27: the Daisy Seed wires the STM32's BOOT0 pin to PG3
// (see `libDaisy/src/sys/system.cpp` `ResetToBootloader(STM)`). The hardware
// samples BOOT0 at reset, so all we need is to pull PG3 HIGH, wait for the
// BOOT0 cap to charge, and trigger a system reset. The chip then enters the
// ST DFU loader naturally — no manual jump, no magic value, no peripheral
// teardown, no pre_init trampoline. Every previous "jump to 0x1FF09800"
// variant was solving the wrong problem; this is what libDaisy actually does.

/// Enable the FPU's **flush-to-zero** mode (FPSCR.FZ) and default-NaN.
///
/// Denormal floats — which a decaying delay or reverb tail produces once the
/// signal collapses toward zero — take a large cycle penalty on the Cortex-M7
/// FPU. In a per-sample feedback loop that penalty blows the 96 kHz audio-ISR
/// budget and locks the firmware. Flush-to-zero treats denormals as 0.0
/// (inaudible), keeping every op single-cycle. Call once at startup; the FPU is
/// already enabled by cortex-m-rt on this hardfloat target.
pub fn enable_flush_to_zero() {
    unsafe {
        let mut fpscr: u32;
        core::arch::asm!("vmrs {}, fpscr", out(reg) fpscr);
        fpscr |= 1 << 24; // FZ  — flush denormals to zero
        fpscr |= 1 << 25; // DN  — default NaN (a NaN can't linger in a feedback loop)
        core::arch::asm!("vmsr fpscr, {}", in(reg) fpscr);
    }
}

/// Request the STM32 system bootloader (USB DFU). Drives PG3 (= BOOT0) HIGH
/// and triggers a chip reset — the hardware boot logic does the rest. No
/// post-trigger LED diagnostic: the *accelerating* flash during the gesture
/// hold (in the bin's main loop) already confirms detection, so we go straight
/// from "gesture fired" to "chip reset" for the snappiest possible feel.
pub fn reset_to_bootloader() -> ! {
    cortex_m::interrupt::disable();
    unsafe {
        // Pull BOOT0 (PG3) HIGH the same way libDaisy does: enable GPIOG clock
        // → set PG3 as output → drive it high → 10 ms for the BOOT0 cap →
        // mask RCC interrupts → chip reset.
        let rcc = &*hal::pac::RCC::PTR;
        rcc.ahb4enr.modify(|_, w| w.gpiogen().set_bit());
        let _ = rcc.ahb4enr.read(); // wait one cycle for the clock to settle

        let gpiog = &*hal::pac::GPIOG::PTR;
        gpiog.moder.modify(|_, w| w.moder3().bits(0b01)); // PG3 = output
        gpiog.bsrr.write(|w| w.bs3().set_bit()); // PG3 HIGH
        cortex_m::asm::delay(4_800_000); // ~10 ms @ 480 MHz (matches HAL_Delay(10))

        rcc.cier.write(|w| w.bits(0)); // mask all RCC interrupts (libDaisy parity)
        cortex_m::peripheral::SCB::sys_reset();
    }
}
