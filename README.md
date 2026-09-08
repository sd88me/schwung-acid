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
  slot sets the shared **root** live — sequencing itself is never gated by
  note input.
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
  with the opposing side ramping linearly between. Inspired by the
  merged-output architecture of sd88me's own
  [Maze Lite](https://github.com/sd88me/schwung-maze-sequencer)'s Trig Mix,
  though the velocity curve itself differs from Trig Mix's crossfade.
- **Reset Both** (1/2/4/8 bars, or Off) periodically snaps both sequencers
  back to step 1 together. Off lets differently-lengthed A/B patterns drift
  as a genuine polymeter.
- No banks, no undo, no persistence in this version — Generate and Mutate
  only.

## Pages (8 knobs each)

| Page | Knobs |
|---|---|
| **Sequencer A** (root level) | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **Sequencer B** | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **Global** | Root, Scale, Seq B on/off, A Algo, B Algo, Blend, Reset Both, *(free)* |

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

Generation ideas drawn from three references (see the design doc for detail):
- [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) by Charles
  Vestal (GPL-3.0), itself a port of djphazer's `TB_3PO` applet from the
  [O_C-Phazerville](https://github.com/djphazer/O_C-Phazerville) Hemisphere
  Suite — the primary generator (Algo 1) is adapted from this model.
- `Sting.amxd` ("Sting 2.26" by Iftah Gabbai, CC BY-NC-ND) — inspiration only
  for the secondary generator's mechanism (urn-style draw, random-walk
  density, fixed-permutation accents); no code or assets reused.
- [schwung-maze-sequencer](https://github.com/sd88me/schwung-maze-sequencer)
  by sd88me — the dual-sequencer-merged-into-one-midifx-output
  architecture that Blend is built on (the velocity curve itself is
  Acid-specific — see above).

Created by sd88me for [Schwung](https://github.com/charlesvestal/schwung).
