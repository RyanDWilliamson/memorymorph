# Memory Morph

A Chase Bliss–inspired morphable guitar effects pedal for the
[Cleveland Music Co. HotHouse](https://www.clevelandmusicco.com/hothouse)
running on the Electrosmith Daisy Seed (STM32H750, 96 kHz, boost mode).

## Concept

**MORPH** (Knob 1) sweeps through three distinct sonic characters:

| Position | Character | Description |
|---|---|---|
| 0.0 | **Tape** | Pure saturated tape tone — no delay, no reverb |
| 0.5 | **Echo** | Warm Memory Man-style echo with light saturation |
| 1.0 | **Ambient** | Modulated shimmer reverb wash |

## Controls

| Control | Function |
|---|---|
| Knob 1 | **Morph** — sweeps all parameters between Tape → Echo → Ambient |
| Knob 2 | **Time** — delay time 50 ms – 1600 ms (log) |
| Knob 3 | **Repeats** — delay feedback 0 – 97% |
| Knob 4 | **Depth** — modulation intensity (scaled by Morph) |
| Knob 5 | **Tone** — LPF cutoff 800 Hz – 18 kHz (log) |
| Knob 6 | **Mix** — dry/wet blend |
| Toggle 1 | Saturation character: ↑ Tape (soft clip) · — Warm (fold) · ↓ Clean |
| Toggle 2 | Modulation type: ↑ Chorus · — Vibrato · ↓ Wow/Flutter |
| Toggle 3 | Reverb tail: ↑ Short plate · — Long plate · ↓ Shimmer (octave up) |
| Footswitch 2 | **Bypass** — LED 2 on = active |
| Footswitch 1 | **Freeze** (single press) · DFU bootloader (hold 2 s) — LED 1 pulses when active |

## Building

```bash
# Install ARM toolchain (arm-none-eabi-gcc) and dfu-util first
cd src/MemoryMorph
make

# Flash via USB DFU (hold BOOT on Daisy Seed, plug USB, release BOOT)
make program-dfu
```

## Project Structure

```
hothouse/
├── DaisySP/            # submodule — DSP algorithm library
├── libDaisy/           # submodule — hardware abstraction library
├── src/
│   ├── hothouse.h      # HotHouse board support (Cleveland Music Co.)
│   ├── hothouse.cpp
│   └── MemoryMorph/
│       ├── memory_morph.cpp
│       └── Makefile
├── .github/
│   ├── copilot-instructions.md
│   └── instructions/
│       └── cpp-dsp.instructions.md
├── AGENTS.md
└── CLAUDE.md
```

## Dependencies

- [libDaisy](https://github.com/electro-smith/libDaisy)
- [DaisySP](https://github.com/electro-smith/DaisySP)
- [HothouseExamples](https://github.com/clevelandmusicco/HothouseExamples) (hothouse.h / hothouse.cpp sourced from here)
- `arm-none-eabi-gcc` toolchain
- `dfu-util`
