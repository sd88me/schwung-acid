# Acid — a generative acid-bassline MIDI FX for Ableton Move (Schwung)

Two independent generative sequencers, blended into one output, running as a
[Schwung](https://github.com/charlesvestal/schwung) slot MIDI FX. Design notes
and the decisions behind this module live in the "Schwung - Ableton Move"
project (`claude/acid-seq-design.md`).

## What it is

- **Sequencer A / Sequencer B**, each 2–32 steps, each with its own
  Density / Accent / Slide / Octaves / Length / Gate and a per-step
  four-way state (rest / note / accent / slide).
- Both free-run continuously off Move's transport. Playing a note into the
  slot **transposes** both sequencers live, relative to C4 (C4 = no shift);
  the shared **Root** knob still sets the key they play in and is never
  moved by note input — so a clip on the track, a stray pad, or an echo of
  Acid's own output can't drag it around. Sequencing itself is never gated
  by note input.
- Each sequencer has its own **Algo** knob (1–16): 1 reproduces
  [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po)'s
  density/accent/slide/octave model exactly; higher settings blend in a
  second, `Sting.amxd`-inspired generator (non-repeating pitch draw, a
  density-modulated random walk, and a fixed-permutation accent shape).
- A MIDI FX slot can only forward to the one synth in that slot, so Sequencer
  A and B **merge into a single output stream**, mixed by a bipolar **Blend**
  knob. Unlike a level crossfader, Blend scales each sequence's *own*
  velocity by a 0–100% multiplier rather than substituting an absolute
  target velocity, so each sequence's accent/normal-note ratio — a defining
  feature of acid lines — stays intact no matter where the knob sits: −63 =
  A at 100% / B at 0%, centre = both at 100%, +64 = A at 0% / B at 100%,
  with the opposing side ramping linearly between. Defaults to −63 (Seq A
  alone), so B stays silent until you dial it in.
- **Reset Both** (1/2/4/8 bars, or Off) periodically snaps both sequencers
  back to step 1 together. Off lets differently-lengthed A/B patterns drift
  as a genuine polymeter.
- No banks, no undo, no persistence in this version — Generate and Mutate
  only.

## Pages (8 knobs each)

| Page | Knobs |
|---|---|
| **SEQUENCE A** (root level) | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **SEQUENCE B** | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **Global** | Scale, Root, Seq B on/off, Blend, Algo A, Algo B, Reset Both |

## What each knob does

### SEQUENCE A / SEQUENCE B (one identical set per sequencer)

- **Generate** — turn it to re-roll that sequencer's whole pattern from a
  fresh random seed. Density / Accent / Slide / Octaves / Algo are read at
  this moment, so they shape *the next* re-roll, not the pattern already
  playing.
- **Mutate** — turn it to nudge ~25 % of the steps in place (rest↔note,
  re-pick degree/octave) without a full re-roll. Uses Density / Accent /
  Slide / Octaves but not Algo. Keeps the pattern recognisable while it
  drifts; repeated Mutates keep evolving.
- **Density** (0–100 %) — chance that any given step is a note rather than a
  rest. Low = sparse, high = every step fires. Applied on Generate/Mutate.
- **Accent** (0–100 %) — chance a note is accented (velocity 118 vs the
  normal 72). Applied on Generate/Mutate.
- **Slide** (0–100 %) — chance a note slides into the next step: it holds
  through the step boundary and sends portamento (CC 65), 303-style. A
  slide into a rest is demoted to a plain note. Applied on Generate/Mutate.
- **Octaves** (1–3) — how many octaves above the root the pattern's pitches
  may span. Applied on Generate/Mutate.
- **Length** (2–32) — number of steps before the pattern loops. Takes
  effect immediately (the loop point moves); Generate to fill the new
  span with fresh steps.
- **Gate** (0–100 %) — note length as a fraction of one step. Low =
  staccato blips; high = notes nearly touch. Held slides ignore it and
  ring until the next note. Live — no re-roll needed.

### Global

- **Scale** — the scale both sequencers quantise to: Minor, Phrygian,
  Harmonic Minor, Minor Pentatonic, Dorian, Major.
- **Root** — the key both sequencers play in (C…B). Authoritative and
  stable: playing a note into the slot transposes around this live (C4 =
  no shift) but never moves the knob.
- **Seq B** (on/off) — off silences Sequencer B and releases any note it
  was holding; A keeps running.
- **Blend** (−63…+64) — velocity balance between the two sequencers merged
  into the one output. −63 = A only, centre = both at full, +64 = B only,
  each side scaling the *other* sequencer's own velocities down as the knob
  travels (so each line keeps its internal accent/normal ratio). Default
  −63.
- **Algo A / Algo B** (1–16) — per sequencer, how much of the secondary
  generator is blended into the primary on the next re-roll. 1 = pure
  tb3po model (density/accent/slide/octave rolls); 16 = mostly the
  secondary (urn-style non-repeating pitch draw, random-walk gate density,
  pyramid-shaped accents); in between substitutes secondary for primary
  per step at a rising probability. Read on Generate only.
- **Reset Both** — 1 / 2 / 4 / 8 bars, or Off. Every N bars, snap both
  sequencers back to step 1 together. Off lets differently-lengthed A/B
  patterns run free as a genuine polymeter.

## Install

```bash
git clone <this repo>
cd schwung-acid
cp <schwung>/src/host/plugin_api_v1.h  src/include/
cp <schwung>/src/host/midi_fx_api_v1.h src/include/
./scripts/build.sh
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Power-cycle the Move, or rescan modules. **Acid** appears as an option in a
MIDI FX chain slot. Route it to a sound generator in the same slot, press
Play.

## Build from source

Requires Docker (cross-compiles the DSP for the Move's ARM64 chip).

```bash
bash scripts/build.sh
# produces dist/acid-module.tar.gz
```

### Vendored headers

The DSP compiles against two Schwung API headers, kept out of git so they
stay in sync with the host ABI:

```bash
cp <schwung>/src/host/plugin_api_v1.h  src/include/
cp <schwung>/src/host/midi_fx_api_v1.h src/include/
```

## Repository layout

```
src/
  acid/
    module.json  help.json
    dsp/acid.c
  include/          # vendored Schwung headers (not committed)
scripts/
  build.sh  install.sh  Dockerfile
```

## Credits & license

Generation ideas drawn from two references (see the design doc for detail):
- [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) by Charles
  Vestal (GPL-3.0), itself a port of djphazer's `TB_3PO` applet from the
  [O_C-Phazerville](https://github.com/djphazer/O_C-Phazerville) Hemisphere
  Suite — the primary generator (Algo 1) is adapted from this model.
- `Sting.amxd` ("Sting 2.26" by Iftah Gabbai, CC BY-NC-ND) — the secondary
  generator's *mechanisms* (urn-style non-repeating draw, random-walk gate
  density) are independently reimplemented, inspired by the patch's approach
  but not copied from it. The **accent-order table itself is a direct
  exception**: `VEL_PYRAMID`'s 16 values are Sting's actual "VelPyra"
  permutation, reused as-is (not just inspired by) for v1 while the module is
  developed and played on real hardware. This is a placeholder, not a
  final decision — it's slated to be replaced with an original permutation
  before any release beyond personal/non-commercial use, consistent with
  Sting.amxd's CC BY-NC-ND (NonCommercial, NoDerivatives) terms.

Created by sd88me for [Schwung](https://github.com/charlesvestal/schwung).
