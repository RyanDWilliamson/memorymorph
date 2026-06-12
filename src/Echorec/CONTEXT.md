# Echorec

The instrument modelling the **Binson Echorec 2 (T7E)** — a magnetic-drum,
fixed-multi-head echo (Gilmour, Hank Marvin). Shipped as a standalone firmware
build, not a runtime mode of the MemoryMorph pedal.

## Language

### Mechanism

**Magnetic drum**:
The rotating steel drum (fixed speed) that is the Echorec's recording medium —
the analogue of tape in a tape echo, but with fixed-position heads. Its fixed
rotation period sets the maximum echo time (~300 ms).
_Avoid_: disc, wheel, tape, loop.

**Record head**:
The single head that writes the input signal onto the magnetic drum.
_Avoid_: write head, input head.

**Playback head**:
One of the **four** fixed-position heads that read the drum at four fixed delay
times. Numbered 1–4 from shortest to longest, at even quarters of the drum
period in a **1:2:3:4 ratio** (75 / 150 / 225 / 300 ms). The head selector picks
combinations of these four; drum speed scales all four together.
_Avoid_: read head, tap (reserve "tap" for the SRE-555 tape model and tap tempo).

**Head selector**:
The 12-position rotary on the Echorec 2 that selects which *combination* of the
four playback heads is active, producing the box's signature rhythmic
multi-echo patterns. The selector changes the **pattern**, not the delay time.
In this model it is a **value-gated knob**: a plain continuous pot quantized in
firmware into 12 detented zones (with boundary hysteresis + LED confirm), not a
stepped-pot hardware part.
_Avoid_: pattern knob, tap selector.

### Controls

**Drum speed**:
This model's (non-original) control that scales all four playback-head delay
times *proportionally*, preserving the fixed head ratios — so the rhythmic
patterns stay intact while the whole grid moves faster/slower. Noon = the
authentic ~300 ms drum. The real Echorec had no such control (fixed drum
speed); this is a deliberate modern-usability liberty.
_Avoid_: delay time, rate, tape speed.

**Swell**:
The Echorec's regeneration/feedback control — how much of the playback-head
output (the *selected* multi-head sum) is re-recorded onto the drum, setting the
number of repeats. The feedback recirculates the selected program, so patterns
multiply and cascade; the regeneration valve narrows bandwidth each pass so
repeats progressively darken.
_Avoid_: feedback, regen, repeats.

### Voicing

**Bias**:
The AC record-bias level, which on the real unit trades distortion against high-
frequency content (too low → thin/distorted, too high → mushy/dark). Not a
separate control in this model: drive is fixed at Hot (pushing record level into
the mushy end) and the Age control darkens the playback further.
_Avoid_: bias knob.

**Age**:
The K5 "wear" control. Pushes the model toward hairy lofi: HF loss, raised tape
hiss, intermittent amplitude **dropouts**, and harder record-stage **grit** —
plus a *small* amount of warble. Deliberately weighted toward grit/noise over
pitch wobble.
_Avoid_: tone, age/wear as warble.

**Warble**:
The subtle pitch instability of the steel recording wire on the drum — gentler
than tape wow/flutter (the wound-wire drum was prized for stability), and kept
deliberately light so Age reads as lofi grit, not seasickness.
_Avoid_: wow, flutter (those name the tape-transport artifacts of the SRE-555).

**Head voicing**:
The SW1 control on the playback heads: Normal, Octave-shimmer (a +12 pitch tap
injected into the swell feedback so the wash rises), or Sub (a −12 tap blended
into the output to thicken). Replaces the old drive switch (drive is now fixed
at Hot).
_Avoid_: pitch knob, shimmer (reserve for the Octave mode specifically).

**Head bump**:
The resonant low-mid EQ rise from the playback head's magnetic response, modeled
on the head-amp stage. Shared concept with the SRE-555 tape model's playback
head-bump.

**Trail character**:
The SW3 voicing of how repeats *degrade* (independent of Swell amount):
**Shoegaze** (bright trails + allpass diffusion in the feedback → blurred
ambient wash), Vintage (authentic darkening), Dub (heavy darkening + extra
in-loop saturation). Replaces the earlier "Clean" mode.
_Avoid_: feedback tone.
