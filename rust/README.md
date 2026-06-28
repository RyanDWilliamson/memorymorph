# HotHouse Rust firmware

From-scratch **Rust** firmware for the Hothouse (Daisy Seed / STM32H750), living
alongside the C++ firmwares in `../src` which stay as bench references. Built on
the [`daisy`](https://crates.io/crates/daisy) BSP (no DaisySP — DSP is
hand-ported). Mono, 96 kHz. See `../AGENTS.md` for hardware context.

This workspace now hosts **two pedals** sharing a platform library (`[lib]
name = "hothouse"`): the control-surface HAL (`board`), the `delay` line, and
each pedal's engine. Pure, host-testable DSP lives in the `dsp/` crate
(`cargo test -p dsp` — 39 tests).

- **Cassette LoFi Junky** — `--bin cassette-lofi-junky` — Gen-Loss-style tape
  degradation.
- **Driftwood** — `--bin driftwood` — dual-engine (TIME + SPACE + MOVEMENT)
  Mood-style ambient machine; design in
  [`docs/driftwood-plan.md`](docs/driftwood-plan.md), glossary in
  [`src/driftwood/CONTEXT.md`](src/driftwood/CONTEXT.md), decisions in
  [`docs/adr/`](docs/adr/).

## ⚠️ Confirm your Daisy Seed revision first

The codec differs by board rev and the wrong feature = silence:

| Feature | Board | Codec |
|---|---|---|
| `seed` | original Daisy Seed | AK4556 |
| `seed_1_1` *(default)* | Daisy Seed 1.1 | WM8731 |
| `seed_1_2` | Daisy Seed 1.2 | PCM3060 |

Default is `seed_1_1`. If audio is silent on the bench, that's the first thing
to change (`make build BOARD=seed`).

## Build & flash

```sh
make build                 # compile (seed_1_1 + 96 kHz)
make build BOARD=seed      # compile for the original Seed
make flash-dfu             # flash Cassette LoFi Junky (Seed in DFU first)
make flash-driftwood       # flash Driftwood
```

### Driftwood control map

Both engines run in series (`IN → TIME → SPACE → OUT`); the six knobs are paged
by TOGGLE_1 with soft-takeover.

| | TIME page | MASTER page | SPACE page |
|---|---|---|---|
| K1 | delay time | move rate | reverb decay |
| K2 | repeats | move depth | regen |
| K3 | warble | move shape | reverb mod |
| K4 | drive | **input gain** | age/grit |
| K5 | delay mix | move target | reverb mix |
| K6 | time level | output level | reverb tone |

- **TOGGLE_2** TIME mode: Looper / Delay / Tape-slip · **TOGGLE_3** SPACE
  character: Dark / Modulated / Shimmer.
- **FOOTSWITCH_1** is mode-dependent: in delay modes short = tap tempo (and
  tap-syncs the MOVEMENT LFO), hold = freeze/havoc; in Looper short =
  record→play→overdub, hold = stop/clear. Moving the time knob releases the tap.
- **FOOTSWITCH_2** = bypass (trails). DFU = both held + KNOB_5 dry, ~1.5 s.

On-bench voicing and the CPU-budget check are the remaining Phase-6 items.

**Entering DFU without opening the pedal** (mandatory firmware gesture,
bench-verified 2026-06-27): hold **both footswitches** with the **wet/dry
(KNOB_5) fully dry** for ~1.5 s. The board re-enumerates as `0483:df11`;
then `make flash-dfu`.

Fallback (always works, needs the enclosure open): hold **BOOT**, tap **RESET**
on the Seed. Only needed if the firmware ever bricks itself.

### How the DFU gesture actually works

The Daisy Seed wires the STM32's **BOOT0 pin to PG3**. The hardware samples
BOOT0 at reset — HIGH means boot into the ST system DFU loader. So
`reset_to_bootloader()` doesn't try to *jump* anywhere; it just mirrors what
libDaisy's `System::ResetToBootloader(STM)` does (`libDaisy/src/sys/system.cpp`):

1. Enable GPIOG bus clock (`RCC.AHB4ENR.gpiogen`).
2. Configure PG3 as output, drive HIGH (`BSRR.bs3`).
3. Wait ~10 ms for the BOOT0 cap to charge.
4. Mask all RCC interrupts (`RCC.CIER = 0`).
5. `SCB::sys_reset()` — chip reset; BOOT0 sampled HIGH; ST bootloader runs.

**Do not** try to `cortex_m::asm::bootload(0x1FF09800)` directly — every
variant of that (cache/NVIC/MPU/DMA teardown, magic-value-then-reset with a
`#[pre_init]` trampoline, ±RTC backup register) was tried and is bench-
unreliable on the H7. The hardware boot-pin path is the only thing that works
on this board. See `feedback_footswitch_bootloader.md` in memory for history.

## Toolchain notes

- Target: `thumbv7em-none-eabihf` (`rustup target add` once).
- Needs `cargo-binutils` + `llvm-tools-preview` for `cargo objcopy`, and
  `dfu-util` for flashing.
- **flip-link is intentionally not used** — it double-includes the BSP's
  `REGION_ALIAS`-based `memory.x` and fails to link. The BSP supplies `memory.x`
  via its build script, so this crate ships neither `memory.x` nor a `build.rs`.

## Status

- [x] Phase 1 — toolchain, board init, blinky builds + flashes; **boot bench-
      verified** (board enumerates, runs Rust firmware).
- [x] Phase 2 — Hothouse control HAL **bench-verified** (2026-06-27): all 6
      knobs (ADC1), all 3 toggles (Up/Mid/Down), both footswitches, both LEDs,
      DWT clock, and the **DFU footswitch gesture** (both FS + KNOB_5 dry →
      DFU via PG3+sys_reset). See `src/hothouse.rs` and `src/bin/validate.rs`.
- [x] Phase 3 — SAI 96 kHz mono passthrough **builds** (11 KB FLASH). Manual
      cortex-m ISR (`DMA1_STR1`); input → both outputs. *Audio bench pending.*
- [x] Phase 4 — DSP library `dsp/` **builds no_std + 14 host tests pass**:
      `saturation` (TapeSat 3-stage, TubeStage), `lofi` (LCG Hiss, Dropout gate),
      `tone` (CassetteTone bandwidth), `warble` (wow+flutter LFO). Ported from
      Memory Morph + Echorec C++; run `cargo test -p dsp --target <host-triple>`.
- [x] Phase 5 — full signal chain **builds + clippy-clean** (27 KB FLASH, DFU
      `.bin` produced). SDRAM wow/flutter delay line, `dsp` chain wired into the
      audio ISR, control surface mapped to `Params` (control loop → ISR via
      `Mutex<Cell<Params>>`), bypass + FSW2 junk slam. See `src/engine.rs`.
      *Audio + SDRAM bench-pending — see risks.*
- [ ] Phase 6 — voicing (bench/ears only): tune the musical→wild gradient, dial
      the three intensity tiers, refine TOGGLE_2/TOGGLE_3 (reserved).

## Three binaries

| Binary | Purpose | Flash with |
|---|---|---|
| `cassette-lofi-junky` | Full effect (audio, SDRAM delay, all DSP). | `make flash-dfu` |
| `bringup` | Codec/SDRAM-independent control test (heartbeat + knob/toggle mapping via the onboard USER LED, less useful with the enclosure closed). | `make flash-bringup` |
| `validate` | **Codec/SDRAM-independent visible-only HIL test.** LED 1 blink rate = the *selected* knob (TOGGLE 1+2 pick 1 of 6; TOGGLE 1 mid = sweep). LED 2 = footswitches + TOGGLE 3-controlled idle. Accelerating LED feedback during the DFU gesture. | `make flash-validate` |

`validate.bin` is the firmware to flash whenever you want to confirm the
control surface and the DFU gesture without involving the codec / SDRAM /
audio chain. It's how we proved Phase 2.

## Risks to watch on the audio bench run

1. **Seed revision / codec feature** — wrong = silence (`make build BOARD=…`).
   Iterate freely — the DFU gesture means no opening the pedal between tries.
2. **SDRAM delay line** — `from_raw_parts_mut` over `sdram.base_address` + MPU
   config is the most likely hard-fault on the *audio* bring-up; if the unit
   faults only with the effect engaged, suspect this first.
3. **Dry/wet flange** — at Mix < 1 the short modulated delay combs with the dry
   path; intended to be voiced in Phase 6 (default Mix = 1.0 avoids it).
4. **Audio DMA at DFU time** — the full firmware enables SAI/DMA1 Stream 1; the
   PG3+sys_reset cleans them up, but cleaner would be `audio_interface.stop()`
   and ADC stop at the top of `reset_to_bootloader()`. Match libDaisy's order.

## Workspace layout

```
rust/            firmware crate (binary, no_std, thumbv7em) + [workspace] root
  src/main.rs    entry, control loop, audio ISR
  src/hothouse.rs  control HAL (knobs/toggles/footswitches/LEDs/clock/DFU)
dsp/             pure-DSP library — no_std on target, std under `cargo test`
  src/{saturation,lofi,tone,warble,onepole}.rs
```

Build firmware: `make build`. Test DSP on host:
`cargo test -p dsp --target x86_64-unknown-linux-gnu`.
