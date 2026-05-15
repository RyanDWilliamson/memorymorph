# Memory Morph

A morphable guitar effects firmware for the
[Cleveland Music Co. HotHouse](https://www.clevelandmusicco.com/hothouse)
running on the Electrosmith Daisy Seed (STM32H750, 48 kHz, 480 MHz boost mode).

Three presets share the same pedal — switch between them on the fly using
hidden footswitch combos. Boot always lands in DMM mode.

## DMM mode (default)

A Chase Bliss–inspired chain that models the Electro-Harmonix Deluxe Memory
Man: SA571 compander (with matched expander and a coupled noise floor that
breathes between repeats) → BBD delay (modelled MN3005, with pre/de-emphasis
shelves and 2nd-harmonic asymmetric soft-clip) → Schroeder plate reverb with
an optional shimmer (PitchShifter +12 st) feedback loop. **MORPH** (Knob 1)
sweeps the whole signal chain through three sonic zones.

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
| Footswitch 1 | Tap tempo (short press) · Freeze (hold ≥ 1500 ms) · **Fuzz toggle** (double-press) · DFU (10 s hold + combo) |
| Footswitch 2 | **Bypass** — LED 2 on = active |

**Fuzz drive (DMM):** double-press FS1 while in DMM mode to toggle a fourth
drive level on top of SW1=UP. It pushes the preamp tanh into hard saturation
for fuzz-pedal squared corners; the other SW1 positions are unaffected. The
toggle is RAM-only — power-cycle returns to non-fuzz Hot. A slow random drift
on the DMM BBD clock (±0.3%, ~0.1 Hz) wanders the trailing repeats at long
delay times, separating the Echo zone audibly from the dry Tape zone.

## SDD-555 Delay mode

A circuit-level model of the Roland SRE-555 Chorus Echo fused with the
SDD-320 Dimension D: NE570 log-domain VCA compander (the "dirt source",
with an optional **Fuzz override** via FS1 double-press) → multi-head tape
echo (three playback heads at 0.33×/0.66×/1.0× of the user's echo time,
multi-rate wow/flutter, +4 dB head-bump EQ, asymmetric record saturation) →
BBD-style chorus with sine LFO and ±4% slow rate drift → 3-spring
Accutronics tank → choice of AMS Non-Lin gated verb or a Wildcard Resonator
verb. **MORPH** mirrors the DMM's three-zone identity — sweeping Drive →
+Echo → +Chorus/Verb.

| Position | Character | Description |
|---|---|---|
| 0.0 | **Drive** | NE570 compander dirt only — no echo, no chorus, no verb |
| 0.5 | **Echo** | Tape echo with light chorus motion and a touch of verb |
| 1.0 | **Ambient** | Full chorus depth + full verb wash on the echo |

## SDD-555 Chorus Verb mode

The SDD-555 chain with the tape echo stage skipped — straight NE570 dirt
into BBD/H910/Dimension chorus, then 3-spring tank or AMS/Wildcard verb.
**MORPH** sweeps Drive → +Chorus → +Verb.

| Position | Character | Description |
|---|---|---|
| 0.0 | **Drive** | NE570 compander dirt only — no chorus, no verb |
| 0.5 | **Chorus** | Full chorus character (BBD / H910 / Dimension D), no verb |
| 1.0 | **Verb** | Full chorus + full verb wash |

### Chorus Verb controls (knobs that differ from Delay mode)

| Control | Function |
|---|---|
| Knob 2 | **Chorus rate** — 0.1–3 Hz log (tap-synced via FS1) |
| Knob 3 | **Chorus depth** — 0–1 LFO swing intensity (direct, not morphed) |
| Footswitch 1 | Tap tempo (syncs chorus rate) · Freeze (hold ≥ 1500 ms) |

(KNOB_1 Morph, KNOB_4 Verb decay, KNOB_5 Mech age and KNOB_6 Mix behave the
same as in Delay mode.)

### Mode switch combos

Each combo is a direct toggle with DMM. Hold **FS1 + FS2** with Mix fully
dry for **3 seconds**; both LEDs blink alternately 3× to confirm.

| Target preset | Toggle pattern |
|---|---|
| **SDD-555 Delay** | SW1 **UP**, SW2 **DOWN**, SW3 **DOWN** |
| **SDD-555 Chorus Verb** | SW1 **DOWN**, SW2 **UP**, SW3 **DOWN** |

From either SDD-555 preset, the matching combo returns to DMM. To swap
directly between Delay and Chorus Verb, route through DMM (hit one combo,
then the other). The all-toggles-DOWN pattern is reserved for the 10 s DFU
bootloader hold with FS1 only.

### SDD-555 controls

| Control | Function |
|---|---|
| Knob 1 | **Morph** — Drive → +Echo → +Chorus/Verb (DMM-style three-zone sweep) |
| Knob 2 | Tape echo time 50 ms – 500 ms log (authentic SRE-555 range, tap-synced via FS1) |
| Knob 3 | Tape echo feedback / repeats |
| Knob 4 | Verb decay (whichever verb SW3 selects) |
| Knob 5 | Mechanical age — HF rolloff + breathing LFO |
| Knob 6 | **Mix** — dry/wet blend |
| Toggle 1 | NE570 drive (linear): ↑ Hot (double-press FS1 → **Fuzz**) · — Warm · ↓ Clean |
| Toggle 2 | Chorus: ↑ CE-1 BBD (0.5 Hz, modest swing) · — H910 Micropitch (+7c, 20 ms) · ↓ Dimension D ("buttons 1+4" widest) |
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
│       ├── memory_morph.cpp       # AudioCallback dispatcher + DmmBlock / Sdd555Block + main()
│       ├── constants.h            # shared math (kTwoPi, OnePoleCoeff)
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
