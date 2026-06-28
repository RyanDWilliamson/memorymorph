// The doc bullet-list style is intentional; silence the layout nitpick.
#![allow(clippy::doc_overindented_list_items)]

//! Board bring-up test — codec- and SDRAM-independent so it runs on either
//! Daisy Seed revision. One flash, one bench sweep, verifies that every
//! Hothouse control is wired to the GPIO/ADC channel the firmware thinks it
//! is, and that the footswitch DFU gesture works without opening the pedal.
//!
//! Control map:
//!   * USER LED  : fixed ~2 Hz heartbeat. Proves the core is alive.
//!   * LED1      : blinks at a rate set by ONE of the six knobs,
//!                 `period_ms = 80 + (knob * 1200) as u32`
//!                 (~12 Hz fully CCW .. ~0.8 Hz fully CW).
//!   * Knob pick : the three toggles select which knob drives LED1.
//!                 Encode each as 0 (down) / 1 (middle) / 2 (up):
//!                     idx = (t1 * 9 + t2 * 3 + t3) % 6   →  KNOB_1..KNOB_6
//!                 Every knob is reachable; every toggle position must be
//!                 visited to reach every knob, so this also exercises the
//!                 toggle decoder.
//!   * FSW1      : short-press inverts LED1's blink phase. The rate stays
//!                 visible so you can still see the knob respond.
//!   * FSW2      : held → LED2 solid; released → LED2 off.
//!   * DFU       : BOTH footswitches + KNOB_5 (Mix) fully CCW, held ~1.5 s
//!                 → pedal re-enumerates as USB 0483:df11. Lets the player
//!                 reach DFU without opening the enclosure.
//!
//! Flash:
//!   cargo objcopy --release --bin bringup -- -O binary target/bringup.bin
//!   dfu-util -a 0 -s 0x08000000:leave -D target/bringup.bin -d ,0483:df11

#![no_main]
#![no_std]

use cortex_m_rt::entry;
use panic_halt as _;

use stm32h7xx_hal::delay::Delay;

use hothouse::board::{
    self, BootloaderGesture, Clock, ControlState, Controls, FootswitchEvent, FootswitchTracker,
    TogglePosition,
};

const HEARTBEAT_HALF_MS: u32 = 250; // ~2 Hz blink

#[inline]
fn toggle_code(t: TogglePosition) -> u32 {
    match t {
        TogglePosition::Down => 0,
        TogglePosition::Middle => 1,
        TogglePosition::Up => 2,
    }
}

#[inline]
fn selected_knob(state: &ControlState) -> f32 {
    let t1 = toggle_code(state.toggles[0]);
    let t2 = toggle_code(state.toggles[1]);
    let t3 = toggle_code(state.toggles[2]);
    let idx = ((t1 * 9 + t2 * 3 + t3) % 6) as usize;
    state.knobs[idx]
}

#[entry]
fn main() -> ! {
    let mut cp = cortex_m::Peripherals::take().unwrap();
    let dp = daisy::pac::Peripherals::take().unwrap();
    let board = daisy::Board::take().unwrap();

    let ccdr = daisy::board_freeze_clocks!(board, dp);
    let pins = daisy::board_split_gpios!(board, ccdr, dp);
    let mut led_user = daisy::board_split_leds!(pins).USER;

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

    let mut footswitches = FootswitchTracker::new();
    let mut boot_gesture = BootloaderGesture::new();

    let mut user_last = clock.now_cycles();
    let mut led1_last = clock.now_cycles();
    let mut led1_on = false;
    let mut bypass = false;

    loop {
        let state = controls.read();

        if boot_gesture.update(&state, &clock) {
            board::reset_to_bootloader();
        }

        let events = footswitches.update(state.footswitches, &clock);
        if let FootswitchEvent::Released = events[0] {
            bypass = !bypass;
        }

        controls.set_led(1, state.footswitches[1]);

        let period_ms = 80 + (selected_knob(&state) * 1200.0) as u32;
        if clock.elapsed_ms(led1_last) >= period_ms {
            led1_last = clock.now_cycles();
            led1_on = !led1_on;
        }
        controls.set_led(0, led1_on ^ bypass);

        if clock.elapsed_ms(user_last) >= HEARTBEAT_HALF_MS {
            user_last = clock.now_cycles();
            led_user.toggle();
        }
    }
}
