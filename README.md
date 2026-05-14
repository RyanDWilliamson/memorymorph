# Memory Morph

A morphable guitar effects firmware for the
[Cleveland Music Co. HotHouse](https://www.clevelandmusicco.com/hothouse)
running on the Electrosmith Daisy Seed (STM32H750, 48 kHz, 480 MHz boost mode).

Two complete instruments share the same pedal — switch between them on the fly
using a hidden footswitch combo. Boot always lands in DMM mode.

## DMM mode (default)

A Chase Bliss–inspired chain that models the Electro-Harmonix Deluxe Memory
Man: SA571 compander → BBD delay (modelled MN3005) → Schroeder plate reverb
with an optional shimmer (PitchShifter +12 st) feedback loop. **MORPH**
(Knob 1) sweeps the whole signal chain through three sonic zones.

| Position | Character | Description |
|---|---|---|
| 0.0 | **Tape** | Pure saturated tape tone — no delay, no reverb |
| 0.5 | **Echo** | Warm Memory Man-style echo with light saturation |
| 1.0 | **Ambient** | Modulated shimmer reverb wash |

### DMM controls

| Control | Function |
|---|---|
| Knob 1 | **Morph** — sweeps all parameters between Tape → Echo → Ambient |
| Knob 2 | **Time** — delay time 50 ms – 2000 ms (log) |
| Knob 3 | **Repeats** — delay feedback 0 – 97% |
| Knob 4 | **Depth** — modulation intensity (scaled by Morph) |
| Knob 5 | **Tone** — LPF cutoff 4000 Hz – 18 kHz (log) |
| Knob 6 | **Mix** — dry/wet blend |
| Toggle 1 | Drive: ↑ High · — Med · ↓ Low |
| Toggle 2 | Modulation type: ↑ Chorus · — Vibrato · ↓ Wow/Flutter |
| Toggle 3 | Reverb tail: ↑ Short plate · — Long plate · ↓ Shimmer (octave up) |
| Footswitch 1 | Tap tempo (short press) · Freeze (hold ≥ 1500 ms) · DFU (10 s hold + combo) |
| Footswitch 2 | **Bypass** — LED 2 on = active |

## SDD-555 mode

A circuit-level model of the Roland SRE-555 Chorus Echo fused with the
SDD-320 Dimension D: NE570 log-domain VCA compander (the "dirt source") →
tape echo (single-tap, tap-syncable, soft-clipped feedback) → BBD-style
chorus with trapezoidal LFO → 3-spring Accutronics tank → choice of AMS
Non-Lin gated verb or a Wildcard Resonator verb. **MORPH** sweeps
Echo → +Chorus → +Verb.

### Mode switch

Hold **FS1 + FS2** while all three toggles are DOWN and the Mix knob is
fully dry for **3 seconds**. Both LEDs blink alternately 3× to confirm.
Toggle back to DMM the same way.

### SDD-555 controls

| Control | Function |
|---|---|
| Knob 1 | **Morph** — Echo only → +Chorus → +Verb |
| Knob 2 | Tape echo time 50 ms – 500 ms log (authentic SRE-555 range, tap-synced via FS1) |
| Knob 3 | Tape echo feedback / repeats |
| Knob 4 | Verb decay (whichever verb SW3 selects) |
| Knob 5 | Mechanical age — HF rolloff + breathing LFO |
| Knob 6 | **Mix** — dry/wet blend |
| Toggle 1 | NE570 drive (linear, no preamp clip): ↑ Hot · — Warm · ↓ Clean |
| Toggle 2 | Chorus type: ↑ BBD (CE-1) · — Eventide pitch · ↓ Dimension D |
| Toggle 3 | Verb: ↑ AMS Non-Lin · — Wildcard Resonator · ↓ Spring only |
| Footswitch 1 | Tap tempo (syncs echo time) · Freeze |
| Footswitch 2 | **Bypass** — LED 2 on = active |

## Building

```bash
# Install ARM toolchain (arm-none-eabi-gcc) and dfu-util first
cd src/MemoryMorph
make

# Flash via USB DFU (hold BOOT on Daisy Seed, plug USB, release BOOT)
make program-dfu
```

A `PostToolUse` hook in `.claude/settings.json` automatically rebuilds
`src/MemoryMorph` whenever a `.cpp` or `.h` file under that directory is
edited, surfacing build errors directly in the conversation.

## Project structure

```
hothouse/
├── DaisySP/                       # submodule — DSP algorithm library
├── libDaisy/                      # submodule — hardware abstraction library
├── src/
│   ├── hothouse.h                 # HotHouse board support
│   ├── hothouse.cpp
│   └── MemoryMorph/
│       ├── memory_morph.cpp       # top-level DSP + control logic
│       ├── morph.h                # MORPH macro interpolation (DMM)
│       ├── plate_reverb.h         # Schroeder plate reverb (DMM)
│       ├── dmm_chain.h            # SA571 compander + BBD filters (DMM)
│       ├── tap_tempo.h            # FS1 tap/freeze state machine
│       ├── shimmer.h              # HPF → PitchShifter → auto-duck (DMM)
│       ├── ne570.h                # NE570 VCA compander (SDD-555)
│       ├── bbd_chorus.h           # BBD / Eventide / Dimension D chorus (SDD-555)
│       ├── spring_reverb.h        # 3-spring Accutronics tank (SDD-555)
│       ├── nl_verb.h              # AMS Non-Lin + Wildcard Resonator (SDD-555)
│       └── Makefile
├── .github/
│   ├── copilot-instructions.md
│   └── instructions/cpp-dsp.instructions.md
├── AGENTS.md                      # full technical context
├── CLAUDE.md                      # Claude-specific guidance
└── README.md
```

## Dependencies

- [libDaisy](https://github.com/electro-smith/libDaisy)
- [DaisySP](https://github.com/electro-smith/DaisySP)
- [HothouseExamples](https://github.com/clevelandmusicco/HothouseExamples) (`hothouse.h` / `hothouse.cpp` sourced from here)
- `arm-none-eabi-gcc` toolchain
- `dfu-util`
