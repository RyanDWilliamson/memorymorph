//! Driftwood — Hothouse (Daisy Seed) firmware.
//!
//! Phase 1 scaffold: the control surface, knob paging (TOGGLE_1: TIME / MASTER /
//! SPACE) with soft-takeover, footswitches, LEDs and DFU are fully wired; the
//! audio path is a clean pass-through with the MASTER output-level knob and true
//! bypass. Modeled engines (BBD/tape TIME, PT2399 SPACE, MOVEMENT) land in later
//! phases.
//!
//! Control map (see rust/docs/driftwood-plan.md for the full knob-family table):
//!   TOGGLE_1 PAGE (up=TIME / mid=MASTER / down=SPACE) ·
//!   TOGGLE_2 TIME mode (up=Looper / mid=Delay / down=Tape-slip) ·
//!   TOGGLE_3 SPACE character (up=Dark / mid=Modulated / down=Shimmer).
//!   FOOTSWITCH_1 = tap / hold-freeze (freeze wired; tap = Phase 3) ·
//!   FOOTSWITCH_2 = bypass (LED_2 = effect active).
//!   DFU: BOTH footswitches + KNOB_5 fully dry, held ~1.5 s.

#![no_main]
#![no_std]

use core::cell::{Cell, RefCell};

use cortex_m::interrupt::Mutex;
use cortex_m_rt::entry;
use panic_halt as _;

use stm32h7xx_hal::delay::Delay;
use stm32h7xx_hal::pac::interrupt;

use daisy::audio;
use dsp::looper::LooperInput;
use hothouse::board::{self, BootloaderGesture, Clock, Controls, FootswitchEvent, FootswitchTracker};
use hothouse::driftwood::{DriftwoodEngine, Page, PagedKnobs, Params, SpaceMode, TimeMode};

#[cfg(feature = "sampling_rate_96khz")]
const FS: f32 = 96_000.0;
#[cfg(not(feature = "sampling_rate_96khz"))]
const FS: f32 = 48_000.0;

/// TIME engine delay/loop memory in SDRAM (~1 s @ 96 kHz).
const TIME_SAMPLES: usize = 96_000;
/// Shimmer pitch-shifter window (SPACE engine).
const SHIMMER_SAMPLES: usize = 2_048;

// FOOTSWITCH_1 timing (delay modes): a press shorter than TAP_PRESS_MAX is a
// tap; held past FREEZE_HOLD it becomes a momentary freeze. Valid tap intervals
// map directly to delay time.
const FREEZE_HOLD_MS: u32 = 350;
const TAP_PRESS_MAX_MS: u32 = 600;
const TAP_MIN_MS: u32 = 100;
const TAP_MAX_MS: u32 = 1_200;
/// Knob movement (resolved TIME-time) beyond this releases tap-tempo override.
const KNOB_MOVE_EPS: f32 = 0.01;

static AUDIO_INTERFACE: Mutex<RefCell<Option<audio::Interface>>> = Mutex::new(RefCell::new(None));
static ENGINE: Mutex<RefCell<Option<DriftwoodEngine>>> = Mutex::new(RefCell::new(None));
static PARAMS: Mutex<Cell<Params>> = Mutex::new(Cell::new(Params::DEFAULT));

// The SPACE reverb network + shimmer are small and latency-critical, so they
// live in fast internal RAM (not SDRAM): random access to SDRAM cache-misses and
// stalls the audio ISR, which was overrunning the 96 kHz block and freezing the
// firmware when the reverb was engaged. `REVERB_CAP` must be >= the reverb's
// required length at FS (see the boot-time assert).
const REVERB_CAP: usize = 14_336;
static mut REVERB_MEM: [f32; REVERB_CAP] = [0.0; REVERB_CAP];
static mut SHIMMER_MEM: [f32; SHIMMER_SAMPLES] = [0.0; SHIMMER_SAMPLES];

#[entry]
fn main() -> ! {
    let mut cp = cortex_m::Peripherals::take().unwrap();
    let dp = daisy::pac::Peripherals::take().unwrap();
    let board = daisy::Board::take().unwrap();

    cp.SCB.enable_icache();
    cp.SCB.enable_dcache(&mut cp.CPUID);

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

    // TIME delay/loop memory (large) lives in SDRAM; the reverb + shimmer buffers
    // live in fast internal RAM (see REVERB_MEM / SHIMMER_MEM above).
    assert!(
        REVERB_CAP >= DriftwoodEngine::reverb_len(FS),
        "REVERB_CAP too small for reverb at this sample rate"
    );
    let sdram = daisy::board_split_sdram!(cp, dp, ccdr, pins);
    let time_buf: &'static mut [f32] =
        unsafe { core::slice::from_raw_parts_mut(sdram.base_address as *mut f32, TIME_SAMPLES) };
    let reverb_buf: &'static mut [f32] = unsafe { &mut *core::ptr::addr_of_mut!(REVERB_MEM) };
    let shimmer_buf: &'static mut [f32] = unsafe { &mut *core::ptr::addr_of_mut!(SHIMMER_MEM) };
    let engine = DriftwoodEngine::new(FS, time_buf, reverb_buf, shimmer_buf);
    cortex_m::interrupt::free(|cs| {
        ENGINE.borrow(cs).replace(Some(engine));
    });

    let audio_interface = daisy::board_split_audio!(ccdr, pins);
    let audio_interface = audio_interface.spawn().unwrap();
    cortex_m::interrupt::free(|cs| {
        AUDIO_INTERFACE.borrow(cs).replace(Some(audio_interface));
    });

    let mut footswitches = FootswitchTracker::new();
    let mut boot_gesture = BootloaderGesture::new();
    let mut paged = PagedKnobs::new(Params::DEFAULT_KNOBS);
    let mut heartbeat = clock.now_cycles();
    let mut bypass = true;

    // FOOTSWITCH_1 tap / freeze state.
    let mut fs1_held = false;
    let mut fs1_press_at = clock.now_cycles();
    let mut last_tap_at: Option<u32> = None;
    let mut tap_delay_s = 0.0f32;
    let mut prev_time_knob = Params::DEFAULT_KNOBS[Page::Time.index()][0];

    loop {
        let state = controls.read();

        // DFU gesture: both footswitches + KNOB_5 fully dry, held ~1.5 s.
        if boot_gesture.update(&state, &clock) {
            board::reset_to_bootloader();
        }

        let events = footswitches.update(state.footswitches, &clock);
        // FOOTSWITCH_2 = bypass.
        if let FootswitchEvent::Released = events[1] {
            bypass = !bypass;
        }

        // Track FOOTSWITCH_1 press timing from the debounced edges.
        match events[0] {
            FootswitchEvent::Pressed => {
                fs1_held = true;
                fs1_press_at = clock.now_cycles();
            }
            FootswitchEvent::Released => fs1_held = false,
            _ => {}
        }

        // FOOTSWITCH_1 is mode-dependent (TOGGLE_2). In LOOPER it is the
        // transport (short = record→play→overdub, hold = stop/clear). In the
        // delay modes a short press is tap tempo and a sustained hold is a
        // momentary freeze/havoc.
        let time_mode = TimeMode::from_toggle(state.toggles[1]);
        let mut freeze = false;
        if let TimeMode::Looper = time_mode {
            let cmd = match events[0] {
                FootswitchEvent::Released => Some(LooperInput::ShortPress),
                FootswitchEvent::LongPress => Some(LooperInput::Hold),
                _ => None,
            };
            if let Some(c) = cmd {
                cortex_m::interrupt::free(|cs| {
                    if let Some(eng) = ENGINE.borrow(cs).borrow_mut().as_mut() {
                        eng.looper_transport(c);
                    }
                });
            }
        } else {
            if let FootswitchEvent::Released = events[0] {
                if clock.elapsed_ms(fs1_press_at) < TAP_PRESS_MAX_MS {
                    let now = clock.now_cycles();
                    if let Some(prev) = last_tap_at {
                        let interval = clock.elapsed_ms(prev);
                        if (TAP_MIN_MS..=TAP_MAX_MS).contains(&interval) {
                            tap_delay_s = interval as f32 / 1000.0;
                            cortex_m::interrupt::free(|cs| {
                                if let Some(eng) = ENGINE.borrow(cs).borrow_mut().as_mut() {
                                    eng.tap_sync();
                                }
                            });
                        }
                    }
                    last_tap_at = Some(now);
                }
            }
            freeze = fs1_held && clock.elapsed_ms(fs1_press_at) >= FREEZE_HOLD_MS;
        }

        // Resolve the paged knobs (soft-takeover) and publish for the ISR.
        let page = Page::from_toggle(state.toggles[0]);
        let knobs = paged.update(page, &state.knobs);

        // Moving the TIME-time knob releases the tap-tempo override.
        let cur_time_knob = knobs[Page::Time.index()][0];
        if (cur_time_knob - prev_time_knob).abs() > KNOB_MOVE_EPS {
            tap_delay_s = 0.0;
        }
        prev_time_knob = cur_time_knob;

        let params = Params {
            knobs,
            time_mode,
            space_mode: SpaceMode::from_toggle(state.toggles[2]),
            bypass,
            freeze,
            tap_delay_s,
        };
        cortex_m::interrupt::free(|cs| PARAMS.borrow(cs).set(params));

        controls.set_led(0, freeze); // LED1 = freeze held (tap pulse later)
        controls.set_led(1, !bypass); // LED2 = effect active

        if clock.elapsed_ms(heartbeat) >= 500 {
            heartbeat = clock.now_cycles();
            led_user.toggle();
        }
    }
}

/// Audio block ready: run the Driftwood chain, mono in → both out.
#[interrupt]
fn DMA1_STR1() {
    cortex_m::interrupt::free(|cs| {
        if let Some(audio_interface) = AUDIO_INTERFACE.borrow(cs).borrow_mut().as_mut() {
            let params = PARAMS.borrow(cs).get();
            if let Some(eng) = ENGINE.borrow(cs).borrow_mut().as_mut() {
                eng.set_params(&params);
                audio_interface
                    .handle_interrupt_dma1_str1(|audio_buffer| {
                        for frame in audio_buffer {
                            let (left, _right) = *frame;
                            let y = eng.process(left, &params);
                            *frame = (y, y);
                        }
                    })
                    .unwrap();
            }
        }
    });
}
