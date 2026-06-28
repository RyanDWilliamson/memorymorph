//! Cassette LoFi Junky — Hothouse (Daisy Seed) firmware.
//!
//! Phase 5: the full signal chain is live. The control loop reads the Hothouse
//! surface, maps it to `Params`, and publishes them; the audio ISR runs the
//! `dsp` cassette chain (saturation → wow/flutter delay → bandwidth → hiss →
//! dropouts → mix → level) over an SDRAM delay line.
//!
//! Control map:
//!   KNOB_1 Drive · KNOB_2 Warble · KNOB_3 Tone · KNOB_4 Junk · KNOB_5 Mix ·
//!   KNOB_6 Level · TOGGLE_1 tape speed (up=fast/mid=normal/down=slow) ·
//!   FOOTSWITCH_1 bypass · FOOTSWITCH_2 momentary "junk slam".
//!   DFU: BOTH footswitches + KNOB_5 (wet/dry) fully dry, held ~1.5 s.
//!   (TOGGLE_2 mod character / TOGGLE_3 intensity are reserved for voicing.)

#![no_main]
#![no_std]

use core::cell::{Cell, RefCell};

use cortex_m::interrupt::Mutex;
use cortex_m_rt::entry;
use panic_halt as _;

use stm32h7xx_hal::delay::Delay;
use stm32h7xx_hal::pac::interrupt;

use daisy::audio;
use hothouse::board::{
    self, BootloaderGesture, Clock, Controls, FootswitchEvent, FootswitchTracker, TogglePosition,
};
use hothouse::cassette::{CassetteEngine, Params, TapeSpeed};

#[cfg(feature = "sampling_rate_96khz")]
const FS: f32 = 96_000.0;
#[cfg(not(feature = "sampling_rate_96khz"))]
const FS: f32 = 48_000.0;

/// 1 s of mono delay memory in SDRAM — far more than the wow/flutter needs.
const DELAY_SAMPLES: usize = 96_000;

static AUDIO_INTERFACE: Mutex<RefCell<Option<audio::Interface>>> = Mutex::new(RefCell::new(None));
static ENGINE: Mutex<RefCell<Option<CassetteEngine>>> = Mutex::new(RefCell::new(None));
static PARAMS: Mutex<Cell<Params>> = Mutex::new(Cell::new(Params::DEFAULT));

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

    // SDRAM-backed delay buffer for the wow/flutter line.
    let sdram = daisy::board_split_sdram!(cp, dp, ccdr, pins);
    let delay_buf: &'static mut [f32] =
        unsafe { core::slice::from_raw_parts_mut(sdram.base_address as *mut f32, DELAY_SAMPLES) };
    let cassette = CassetteEngine::new(FS, delay_buf);
    cortex_m::interrupt::free(|cs| {
        ENGINE.borrow(cs).replace(Some(cassette));
    });

    // Bring up SAI audio and start the interrupt.
    let audio_interface = daisy::board_split_audio!(ccdr, pins);
    let audio_interface = audio_interface.spawn().unwrap();
    cortex_m::interrupt::free(|cs| {
        AUDIO_INTERFACE.borrow(cs).replace(Some(audio_interface));
    });

    let mut footswitches = FootswitchTracker::new();
    let mut boot_gesture = BootloaderGesture::new();
    let mut heartbeat = clock.now_cycles();
    let mut bypass = true;

    loop {
        let state = controls.read();

        // DFU gesture: both footswitches + wet/dry (KNOB_5) fully dry, held ~1.5 s.
        if boot_gesture.update(&state, &clock) {
            board::reset_to_bootloader();
        }

        let events = footswitches.update(state.footswitches, &clock);
        if let FootswitchEvent::Released = events[0] {
            bypass = !bypass;
        }

        // Map the control surface to engine parameters and publish for the ISR.
        let params = Params {
            drive: state.knobs[0],
            warble: state.knobs[1],
            tone: state.knobs[2],
            junk: state.knobs[3],
            mix: state.knobs[4],
            level: state.knobs[5],
            speed: tape_speed(state.toggles[0]),
            bypass,
            slam: state.footswitches[1],
        };
        cortex_m::interrupt::free(|cs| PARAMS.borrow(cs).set(params));

        controls.set_led(0, !bypass); // LED1 = effect engaged
        controls.set_led(1, state.footswitches[1]); // LED2 = junk slam held

        if clock.elapsed_ms(heartbeat) >= 500 {
            heartbeat = clock.now_cycles();
            led_user.toggle();
        }
    }
}

fn tape_speed(toggle: TogglePosition) -> TapeSpeed {
    match toggle {
        TogglePosition::Up => TapeSpeed::Fast,
        TogglePosition::Middle => TapeSpeed::Normal,
        TogglePosition::Down => TapeSpeed::Slow,
    }
}

/// Audio block ready: run the cassette chain over the buffer, mono in → both out.
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
