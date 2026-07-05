//! Driftwood — Hothouse (Daisy Seed) firmware.
//!
//! Full dual-engine pedal: `IN → TIME (BBD delay / tape-slip / looper) →
//! SPACE (PT2399 reverb) → OUT`, animated by the MOVEMENT LFO. Six knobs are
//! paged by TOGGLE_1 with soft-takeover; tap tempo, freeze/havoc and the looper
//! transport share FOOTSWITCH_1 (mode-dependent).
//!
//! Control map (see rust/docs/driftwood-plan.md for the full knob-family table):
//!   TOGGLE_1 PAGE (up=TIME / mid=MASTER / down=SPACE) ·
//!   TOGGLE_2 TIME mode (up=Looper / mid=Delay / down=Tape-slip) ·
//!   TOGGLE_3 SPACE character (up=Dark / mid=Modulated / down=Shimmer).
//!   FOOTSWITCH_1 (delay modes) = tap tempo · hold = freeze/havoc;
//!   FOOTSWITCH_1 (looper) = record→play→overdub · hold = stop/clear.
//!   FOOTSWITCH_2 = bypass (LED_2 = effect active).
//!   DFU: BOTH footswitches + KNOB_5 fully dry, held ~1.5 s.

#![no_main]
#![no_std]

use core::cell::{Cell, RefCell};
use core::sync::atomic::{AtomicU32, Ordering};

use cortex_m::interrupt::Mutex;
use cortex_m_rt::{entry, exception};

use stm32h7xx_hal::delay::Delay;
use stm32h7xx_hal::pac::{self, interrupt};

use daisy::audio;
use dsp::looper::{LooperInput, LooperState};
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

// FOOTSWITCH_1 timing (delay modes): a press shorter than FREEZE_HOLD is a
// tap; held past it, the press becomes a momentary freeze (and is NOT a tap —
// the two gestures are disjoint by construction). Valid tap intervals map
// directly to delay time.
const FREEZE_HOLD_MS: u32 = 350;
const TAP_MIN_MS: u32 = 100;
const TAP_MAX_MS: u32 = 1_200;
/// Knob movement (resolved TIME-time) beyond this releases the tap-tempo
/// override. Must exceed the soft-takeover catch window (±0.02 in
/// `dsp::takeover`), or a page flip that snaps a knob within the catch window
/// would silently cancel the tap.
const KNOB_MOVE_EPS: f32 = 0.03;

// ── Lock-up diagnostics ─────────────────────────────────────────────────────
// The pedal locks with digital artifacts when engaged; the LEDs report the
// failure mode directly so we measure instead of guessing:
//   LED1 ON (live, ISR-owned)         → audio-ISR CPU overrun within the last
//                                       second; SOLID while locked = sustained
//   both LEDs strobe TOGETHER (~10Hz) → Rust panic
//   LEDs strobe ALTERNATING  (~10Hz)  → HardFault (bus/memory fault)
//
// Cycle budget per DMA block: at 480 MHz / 96 kHz each sample is 5 000 cycles;
// the BSP's block length is `daisy::audio::BLOCK_LENGTH` (feature-dependent).
#[cfg(feature = "sampling_rate_96khz")]
const CYCLES_PER_SAMPLE: u32 = 5_000;
#[cfg(not(feature = "sampling_rate_96khz"))]
const CYCLES_PER_SAMPLE: u32 = 10_000;
const BLOCK_BUDGET_CYCLES: u32 = CYCLES_PER_SAMPLE * daisy::audio::BLOCK_LENGTH as u32;
/// Blocks to hold the overrun LED after the last overrun (~1 s), so single
/// hiccups read as a blink and a sustained overrun reads as solid.
const OVERRUN_HOLD_BLOCKS: u32 = 96_000 / daisy::audio::BLOCK_LENGTH as u32;
static OVERRUN_HOLD: AtomicU32 = AtomicU32::new(0);

// DIAGNOSTIC (silent-pedal hunt): input-activity meter. The ISR measures the
// codec's RX peak per block; the main loop lights LED1 while real input level
// was seen recently. Splits the silence at the codec boundary:
//   LED1 follows your playing → codec RX works: silence is OUTPUT-side
//   LED1 dark while playing   → input never reaches the codec: INPUT-side
/// RX peak above this counts as "signal present" (guitar strum ≈ 0.05–0.5).
const INPUT_SEEN_LEVEL: f32 = 0.02;
/// Blocks to hold the indication (~150 ms) so it reads as steady flicker.
const INPUT_HOLD_BLOCKS: u32 = 14_400 / daisy::audio::BLOCK_LENGTH as u32;
/// Left-channel activity (the channel the chain and bypass consume).
static INPUT_HOLD: AtomicU32 = AtomicU32::new(0);
/// Right-channel activity — if the guitar shows up here and not on the left,
/// the codec channel mapping is swapped relative to the firmware's assumption.
static INPUT_R_HOLD: AtomicU32 = AtomicU32::new(0);

/// Counts audio ISR invocations, so the main loop can tell "ISR runs but the
/// codec RX is silent" (input-side analog/codec fault) apart from "audio ISR
/// never fires" (SAI/DMA never started). Rendered as a short LED1 tick.
static ISR_BLOCKS: AtomicU32 = AtomicU32::new(0);
/// Counts skipped blocks from DMA state mismatches (late ISR). Occasional = a
/// click under load; growing steadily = the chain is over budget.
static DMA_ERRORS: AtomicU32 = AtomicU32::new(0);

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
    board::enable_flush_to_zero(); // denormal tails must not stall the audio ISR

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

    // FOOTSWITCH_1 tap / freeze state. Held/press-duration tracking lives in
    // FootswitchTracker (debounced); only the gesture interpretation is here.
    let mut freeze_latched = false;
    let mut last_tap_at: Option<u32> = None;
    let mut tap_delay_s = 0.0f32;
    let mut prev_time_knob = Params::DEFAULT_KNOBS[Page::Time.index()][0];

    // Audio-ISR liveness + DMA-error activity, sampled on the heartbeat.
    let mut last_blocks = 0u32;
    let mut isr_alive = false;
    let mut last_dma_errors = 0u32;
    let mut dma_erroring = false;

    loop {
        let state = controls.read();

        // DFU gesture: both footswitches + KNOB_5 fully dry, held ~1.5 s.
        if boot_gesture.update(&state, &clock) {
            board::reset_to_bootloader();
        }

        let events = footswitches.update(state.footswitches, &clock);
        // FOOTSWITCH_2 = bypass, toggled on PRESS: immediate response, and
        // immune to the tracker's Released-suppression after a ≥2 s rest of
        // the foot (which would swallow a release-triggered toggle entirely).
        if let FootswitchEvent::Pressed = events[1] {
            bypass = !bypass;
        }

        // Expire a stale first tap. Checked every tick so it can never survive
        // to the DWT wrap (~8.9 s), where a later tap would alias into the
        // valid window and jump the delay to a random time.
        if let Some(prev) = last_tap_at {
            if clock.elapsed_ms(prev) > TAP_MAX_MS {
                last_tap_at = None;
            }
        }

        // FOOTSWITCH_1 is mode-dependent (TOGGLE_2). In LOOPER it is the
        // transport (short = record→play→overdub, hold = stop/clear). In the
        // delay modes a short press is tap tempo and a sustained hold is a
        // momentary freeze/havoc.
        let time_mode = TimeMode::from_toggle(state.toggles[1]);
        let mut freeze = false;
        if let TimeMode::Looper = time_mode {
            freeze_latched = false;
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
            // A release counts as a tap only if the press stayed under the
            // freeze threshold — tap and freeze gestures are disjoint.
            if let FootswitchEvent::Released = events[0] {
                if footswitches.hold_ms(0, &clock) < FREEZE_HOLD_MS {
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
            // Freeze latches once the (debounced) hold passes the threshold
            // and stays until release — the latch makes holds longer than the
            // ~8.9 s DWT wrap immune to elapsed-time aliasing.
            freeze_latched = footswitches.is_held(0)
                && (freeze_latched || footswitches.hold_ms(0, &clock) >= FREEZE_HOLD_MS);
            freeze = freeze_latched;
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

        // LED1 = state indicator. Decode:
        //   any mode:    solid while the ISR overrun meter holds → CPU overrun
        //   delay modes: solid = FREEZE is active, or (DIAGNOSTIC) the codec
        //                sees input level — a silent pedal reports which side
        //                of the codec is dead
        //   looper:      solid = recording/overdubbing · blink = loop playing
        let led1 = if let TimeMode::Looper = time_mode {
            // Only the looper needs the engine's transport state; skip the
            // critical section entirely in the delay modes.
            let lstate = cortex_m::interrupt::free(|cs| {
                ENGINE
                    .borrow(cs)
                    .borrow_mut()
                    .as_mut()
                    .map(|e| e.looper_state())
            });
            match lstate {
                Some(LooperState::Recording) | Some(LooperState::Overdubbing) => true,
                Some(LooperState::Playing) => (clock.elapsed_ms(heartbeat) / 250).is_multiple_of(2),
                _ => false,
            }
        } else {
            // DIAGNOSTIC decode (delay modes):
            //   SOLID while you play        → guitar on the LEFT channel (the
            //                                 one the chain consumes) — good
            //   2 Hz BLINK while you play   → guitar only on the RIGHT channel
            //                                 → codec channel mapping swapped
            //   fast ~10 Hz flicker         → DMA errors occurring right now
            //   short tick every heartbeat  → audio ISR alive but RX silent
            //   fully dark                  → audio ISR never fires (SAI/DMA)
            let left_in = INPUT_HOLD.load(Ordering::Relaxed) > 0;
            let right_in = INPUT_R_HOLD.load(Ordering::Relaxed) > 0;
            freeze
                || left_in
                || (right_in && (clock.elapsed_ms(heartbeat) / 250).is_multiple_of(2))
                || (dma_erroring && (clock.elapsed_ms(heartbeat) / 50).is_multiple_of(2))
                || (isr_alive && clock.elapsed_ms(heartbeat) < 60)
        };
        // Including the overrun hold here keeps the meter SOLID when the main
        // loop is alive; the ISR's raw write only matters when this loop is
        // starved and cannot run.
        controls.set_led(0, led1 || OVERRUN_HOLD.load(Ordering::Relaxed) > 0);
        controls.set_led(1, !bypass); // LED2 = effect active

        if clock.elapsed_ms(heartbeat) >= 500 {
            heartbeat = clock.now_cycles();
            led_user.toggle();
            let b = ISR_BLOCKS.load(Ordering::Relaxed);
            isr_alive = b != last_blocks;
            last_blocks = b;
            let e = DMA_ERRORS.load(Ordering::Relaxed);
            dma_erroring = e != last_dma_errors;
            last_dma_errors = e;
        }
    }
}

/// Audio block ready: run the Driftwood chain, mono in → both out.
/// Instrumented: measures the block's cycle cost against `BLOCK_BUDGET_CYCLES`
/// and reports overrun on LED1 (raw GPIO — works even if the main loop starves).
#[interrupt]
fn DMA1_STR1() {
    let t0 = cortex_m::peripheral::DWT::cycle_count();
    ISR_BLOCKS.fetch_add(1, Ordering::Relaxed);
    let mut rx_peak = 0.0f32;
    let mut rx_r_peak = 0.0f32;
    cortex_m::interrupt::free(|cs| {
        if let Some(audio_interface) = AUDIO_INTERFACE.borrow(cs).borrow_mut().as_mut() {
            let params = PARAMS.borrow(cs).get();
            if let Some(eng) = ENGINE.borrow(cs).borrow_mut().as_mut() {
                eng.set_params(&params);
                // A DMA state mismatch (e.g. the ISR ran late once) must be a
                // one-block glitch, not death: unwrap() here killed the whole
                // pedal (panic strobe + freewheeling buzz) on the first late
                // block. Skip the block and count it instead.
                if audio_interface
                    .handle_interrupt_dma1_str1(|audio_buffer| {
                        for frame in audio_buffer {
                            let (left, right) = *frame;
                            rx_peak = rx_peak.max(left.abs());
                            rx_r_peak = rx_r_peak.max(right.abs());
                            // Mono chain, channel-agnostic: sum both RX
                            // channels so the guitar gets through regardless
                            // of which codec channel the Hothouse input lands
                            // on (unity gain for a single-channel source).
                            let y = eng.process(left + right, &params);
                            *frame = (y, y);
                        }
                    })
                    .is_err()
                {
                    DMA_ERRORS.fetch_add(1, Ordering::Relaxed);
                    // The BSP's examine_interrupt only clears the HT/TC flags;
                    // a latched error flag (TEIF/FEIF/DMEIF) re-fires this ISR
                    // with neither HT nor TC set → Err on every call, forever:
                    // the TX buffer freewheels as a steady digital tone and no
                    // audio passes. Clear ALL stream-1 flags so the stream
                    // self-heals at the next half/full boundary.
                    unsafe {
                        (*pac::DMA1::PTR).lifcr.write(|w| {
                            w.ctcif1()
                                .set_bit()
                                .chtif1()
                                .set_bit()
                                .cteif1()
                                .set_bit()
                                .cdmeif1()
                                .set_bit()
                                .cfeif1()
                                .set_bit()
                        });
                    }
                }
            }
        }
    });
    // Input-activity meters, per channel (diagnostic; see the constants above).
    let ih = if rx_peak > INPUT_SEEN_LEVEL {
        INPUT_HOLD_BLOCKS
    } else {
        INPUT_HOLD.load(Ordering::Relaxed).saturating_sub(1)
    };
    INPUT_HOLD.store(ih, Ordering::Relaxed);
    let irh = if rx_r_peak > INPUT_SEEN_LEVEL {
        INPUT_HOLD_BLOCKS
    } else {
        INPUT_R_HOLD.load(Ordering::Relaxed).saturating_sub(1)
    };
    INPUT_R_HOLD.store(irh, Ordering::Relaxed);
    let dt = cortex_m::peripheral::DWT::cycle_count().wrapping_sub(t0);
    // Live overrun meter: the main loop renders OVERRUN_HOLD on LED1. The ISR
    // additionally raw-SETS the pin (never clears) so the report still appears
    // when a sustained overrun starves the main loop — the one situation the
    // main loop cannot report. (Deliberate, documented deviation from the
    // "LED updates belong in the main loop" rule, for exactly that reason.)
    let hold = if dt > BLOCK_BUDGET_CYCLES {
        unsafe { (*pac::GPIOA::PTR).bsrr.write(|w| w.bs5().set_bit()) };
        OVERRUN_HOLD_BLOCKS
    } else {
        OVERRUN_HOLD.load(Ordering::Relaxed).saturating_sub(1)
    };
    OVERRUN_HOLD.store(hold, Ordering::Relaxed);
}

// ── Failure-mode reporters (see the diagnostics comment near the top) ────────

/// Rust panic → both LEDs strobe together (~10 Hz).
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    cortex_m::interrupt::disable();
    loop {
        unsafe { board::raw_leds(true, true) };
        cortex_m::asm::delay(24_000_000); // ~50 ms @ 480 MHz
        unsafe { board::raw_leds(false, false) };
        cortex_m::asm::delay(24_000_000);
    }
}

/// HardFault (bus/memory fault) → LEDs strobe alternating (~10 Hz).
#[exception]
unsafe fn HardFault(_frame: &cortex_m_rt::ExceptionFrame) -> ! {
    cortex_m::interrupt::disable();
    loop {
        board::raw_leds(true, false);
        cortex_m::asm::delay(24_000_000);
        board::raw_leds(false, true);
        cortex_m::asm::delay(24_000_000);
    }
}
