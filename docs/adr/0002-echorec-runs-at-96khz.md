# Echorec runs the whole engine at 96 kHz

The rest of the repo is locked to 48 kHz (CLAUDE.md forbids changing it for
MemoryMorph). Echorec is a separate firmware (ADR-0001), so the lock does not
bind it. We run the **entire Echorec engine at 96 kHz** because its defining
feature — pushed input drive (the "Hot" SW1 position) into a soft-clip drum
saturator — generates high harmonics that fold back as inharmonic aliasing at
48 kHz; 96 kHz moves the fold-point up an octave so hard drive stays clean. It
also smooths the fractional-delay interpolation on the moving drum-speed/wow
reads.

## Considered options

- **48 kHz** (repo default) — rejected: pushed drive aliases audibly, which
  fights the instrument's whole point.
- **48 kHz + oversample only the saturator** — rejected: more code complexity
  than running the whole (cheap) engine at 96 kHz.

## Consequences

- Affordable: Echorec has no FFT PitchShifter and no large reverb tank — the two
  expensive elements in the other instruments — so 2× sample rate fits the
  STM32H750 @ 480 MHz comfortably.
- All time constants, filter coefficients, and buffer sizes are computed against
  a 96 kHz sample rate, not the repo-standard 48 kHz. `hw.SetAudioSampleRate(...
  SAI_96KHZ)`; block size stays 48 (0.5 ms latency).
