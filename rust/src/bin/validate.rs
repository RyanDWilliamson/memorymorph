//! Validation firmware — visible-only HIL test for the Hothouse controls.
//!
//! Codec- and SDRAM-independent (no `board_split_audio!`, no
//! `board_split_sdram!`), so the build works on any Daisy Seed revision and
//! keeps the failure surface small while we shake out controls + DFU.
//!
//! Visible feedback uses only the two footswitch LEDs (the Daisy's onboard
//! USER LED sits inside the enclosure and is useless once the pedal is closed).
//!
//! ## What it tests
//!
//! - **LED 1 blink rate = the *selected* knob value** (slow at 0 → fast at 1).
//!   Selection is by toggle combination so all 6 knobs and both 3-position
//!   toggles get exercised:
//!
//!   | TOGGLE_1 | TOGGLE_2 | drives LED 1 |
//!   |---|---|---|
//!   | UP  | UP   | KNOB_1 |
//!   | UP  | MID  | KNOB_2 |
//!   | UP  | DOWN | KNOB_3 |
//!   | DOWN | UP   | KNOB_4 |
//!   | DOWN | MID  | KNOB_5 |
//!   | DOWN | DOWN | KNOB_6 |
//!   | MID  | (any) | **sweep** — auto-cycles K1..K6 every ~1 s |
//!
//! - **LED 2** = switch + TOGGLE_3 state. Press FS1 → solid ON. Press FS2 →
//!   ~10 Hz flicker. Idle behaviour by TOGGLE_3: UP off, MID 1 Hz heartbeat,
//!   DOWN anti-phase with LED 1.
//!
//! - **DFU gesture** (BOTH footswitches + KNOB_5 fully dry) — while engaged,
//!   both LEDs flash in unison; rate accelerates over the 1.5 s hold (3 → 6 →
//!   10 Hz). At threshold both go solid for ~100 ms, then we jump.
//!   If you see the flash but no DFU → the jump is broken.
//!   If you hold and see nothing → detection is broken.

#![no_main]
#![no_std]

use cortex_m_rt::entry;
use panic_halt as _;

use stm32h7xx_hal::delay::Delay;

use hothouse::board::{self, BootloaderGesture, Clock, ControlState, Controls, TogglePosition};

#[entry]
fn main() -> ! {
    let mut cp = cortex_m::Peripherals::take().unwrap();
    let dp = daisy::pac::Peripherals::take().unwrap();
    let board = daisy::Board::take().unwrap();

    let ccdr = daisy::board_freeze_clocks!(board, dp);
    let pins = daisy::board_split_gpios!(board, ccdr, dp);

    let sysclk = ccdr.clocks.sys_ck().to_Hz();
    let clock = Clock::new(sysclk, &mut cp.DCB, cp.DWT);

    let mut delay = Delay::new(cp.SYST, ccdr.clocks);
    let mut controls = Controls::new(
        pins.GPIO,
        dp.ADC1,
        ccdr.peripheral.ADC12,
        &ccdr.clocks,
        &mut delay,
    );

    let mut boot_gesture = BootloaderGesture::new();
    let t_start = clock.now_cycles();

    loop {
        let state = controls.read();
        let t_ms = clock.elapsed_ms(t_start);

        // Gesture armed → solid burst, then hand off to the hardened jump.
        if boot_gesture.update(&state, &clock) {
            controls.set_led(0, true);
            controls.set_led(1, true);
            let burst = clock.now_cycles();
            while clock.elapsed_ms(burst) < 100 {}
            board::reset_to_bootloader();
        }

        let (led1_on, led2_on) = match boot_gesture.hold_progress_ms(&clock) {
            Some(elapsed) => gesture_flash(elapsed, t_ms),
            None => normal_mode(&state, t_ms),
        };

        controls.set_led(0, led1_on);
        controls.set_led(1, led2_on);
    }
}

/// Both LEDs in unison, flashing faster as the hold approaches `BOOTLOADER_HOLD_MS`.
fn gesture_flash(elapsed_ms: u32, t_ms: u32) -> (bool, bool) {
    let hz: u32 = if elapsed_ms < 500 {
        3
    } else if elapsed_ms < 1000 {
        6
    } else {
        10
    };
    let half_period = (500 / hz).max(1);
    let on = (t_ms / half_period).is_multiple_of(2);
    (on, on)
}

fn normal_mode(state: &ControlState, t_ms: u32) -> (bool, bool) {
    let selected = select_knob(state, t_ms);
    let knob_val = state.knobs[selected];
    // 0.0 → ~1500 ms period (slow), 1.0 → ~80 ms (fast).
    let period_ms = (80.0 + (1.0 - knob_val) * 1420.0) as u32;
    let half_period = (period_ms / 2).max(1);
    let led1_on = (t_ms / half_period).is_multiple_of(2);

    let led2_on = if state.footswitches[0] {
        true
    } else if state.footswitches[1] {
        (t_ms / 50).is_multiple_of(2) // ~10 Hz flicker
    } else {
        match state.toggles[2] {
            TogglePosition::Up => false,
            TogglePosition::Middle => (t_ms / 500).is_multiple_of(2), // 1 Hz heartbeat
            TogglePosition::Down => !led1_on,
        }
    };

    (led1_on, led2_on)
}

fn select_knob(state: &ControlState, t_ms: u32) -> usize {
    use TogglePosition::*;
    match (state.toggles[0], state.toggles[1]) {
        (Up, Up) => 0,
        (Up, Middle) => 1,
        (Up, Down) => 2,
        (Down, Up) => 3,
        (Down, Middle) => 4,
        (Down, Down) => 5,
        (Middle, _) => ((t_ms / 1000) % 6) as usize,
    }
}
