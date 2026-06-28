# Driftwood — phase history & rollback guide

Each build phase is a single commit **and** an annotated git tag, so you can roll
back to any phase to isolate a regression at the bench:

```sh
git checkout driftwood-phase3      # detached HEAD at that phase
make flash-driftwood               # flash just that phase
git checkout driftwood             # back to the tip
```

To bisect an audio problem, flash descending phases until the symptom disappears —
the phase that introduced it is the suspect.

| Tag | Commit | Adds | What you should hear when flashed |
|---|---|---|---|
| `driftwood-phase1` | `0d013f3` | Scaffold: paging + soft-takeover, FS2 bypass, DFU | Clean pass-through; MASTER-page K6 sets output level; FS2 toggles bypass; DFU gesture works. No effects yet. |
| `driftwood-phase2` | `8faae92` | All modeled DSP (BBD, PT2399 reverb, MOVEMENT LFO, +12 shimmer, looper FSM) — **host-tested only** | Same audio as phase 1 (DSP not wired into the engine yet). `cargo test -p dsp` exercises it. |
| `driftwood-phase3` | `ba0cb56` | TIME engine live: delay / tape-slip / looper, freeze/havoc | Delay with varispeed glide; tape-slip drift; looper (FS1 transport); FS1-hold freeze. SPACE still pass-through. |
| `driftwood-phase4` | `455b6b9` | SPACE engine: PT2399 reverb + shimmer + bloom (serial after TIME) | Reverb after the delay (TOGGLE_3 Dark/Modulated/Shimmer); FS1-hold blooms the reverb. |
| `driftwood-phase5` | `6737972` | MOVEMENT wired in (tremolo / vibrato / swell) | MASTER page animates the signal per the movement target (K5). |
| `driftwood` (tip) | latest | Docs, tap tempo + tap-sync; bench voicing pending | Full pedal: tap the delay time on FS1; remaining work is on-bench voicing/CPU. |

Notes:
- The `dsp/` crate is shared, so checking out an older phase also reverts the DSP
  modules to that phase's version — host tests stay consistent with the firmware.
- Phases 1–2 produce identical audio (pass-through); the first *audible* effect is
  phase 3. For audio troubleshooting, bisect between phase 3 and the tip.
