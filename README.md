# Acid — a generative acid-bassline MIDI FX for Ableton Move (Schwung)

**v1.1.1** — the Advanced page (Offset / Direction / Jitter / Auto Gen)
and six more scales. See the [Changelog](#changelog) below.

Two independent generative sequencers, blended into one output, running as a
[Schwung](https://github.com/charlesvestal/schwung) slot MIDI FX. Combination of influences from existing popular, acid sequencer generators.


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
  second, `Sting`-inspired generator.
- A MIDI FX slot can only forward to the one synth in that slot, so Sequencer
  A and B **merge into a single output stream**, mixed by a bipolar **Blend**
  knob. Unlike a level crossfader, Blend scales each sequence's *own*
  velocity by a 0–100% multiplier rather than substituting an absolute
  target velocity, so each sequence's accent/normal-note ratio — a defining
  feature of acid lines — stays intact no matter where the knob sits: −63 =
  A at 100% / B at 0%, centre = both at 100%, +64 = A at 0% / B at 100%,
  with the opposing side ramping linearly between. Defaults to −63 (Seq A
  alone), so B stays silent until you dial it in — which is also how you
  switch B off, no separate on/off needed.
- **Reset Both** (1/2/4/8 bars, or Off) periodically snaps both sequencers
  back to step 1 together. Off lets differently-lengthed A/B patterns drift
  as a genuine polymeter.
- An **Advanced** page adds per-sequencer **Offset** (rotate which step
  plays without rewriting the pattern) and **Direction** (Fwd / Rev /
  Pendulum), plus shared **Jitter** (occasionally skip, repeat, or jump a
  step) and **Auto Gen** (re-roll both sequencers every 1–32 bars).
- No banks, no undo, no persistence in this version — Generate and Mutate
  only.

## Pages

| Page | Knobs |
|---|---|
| **SEQUENCE A** (root level) | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **SEQUENCE B** | Generate, Mutate, Density, Accent, Slide, Octaves, Length, Gate |
| **Global** | Scale, Root, Tune B, Blend, Algo A, Algo B, Reset Both, Swing |
| **Advanced** | Offset A, Offset B, Jitter, Auto Gen, Direction A, Direction B (6 of 8 slots) |

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

- **Scale** — the scale both sequencers quantise to. The first six —
  Minor, Phrygian, Harmonic Minor, Minor Pentatonic, Dorian, Major —
  are followed by six acid/techno-leaning additions: Phrygian Dominant,
  Locrian, Whole Tone, Hungarian Minor, Minor Blues, and Chromatic
  (all 12 semitones — an effective "scale off").
- **Root** — the key both sequencers play in (C…B). Authoritative and
  stable: playing a note into the slot transposes around this live (C4 =
  no shift) but never moves the knob.
- **Tune B** (−24…+24 semitones) — Sequencer B's interval *relative to
  Sequencer A*: 0 = same key, +7 = a fifth above, −12 = an octave below.
  It sits on top of Root and live note-in transposition rather than
  replacing them, so transposing from a clip or a played note moves both
  sequencers together and the A→B interval you dialled in is preserved.
  Both sequencers still quantise to the same Scale, so B lands on real
  scale degrees of its own transposed key.
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
- **Swing** (50-75%) — MPC-style 16th-note swing, shared by both
  sequencers so they stay locked together.  Works whether Acid is free-running or following
  an external MIDI clock.

### Advanced

- **Offset A / Offset B** (0…length−1) — rotates which stored step plays
  on each tick, without touching the pattern itself. A read-side shift, so
  it's live and reversible; re-clamped if you shorten Length below it.
- **Direction A / Direction B** — **Fwd** (default), **Rev** (play the
  pattern backwards), or **Pendulum** (run to the end, then back, bouncing
  off each end without repeating it). Reset Both and transport start always
  resume Pendulum travelling forward.
- **Jitter** (0–100%) — per-step chance of perturbing *which* step plays,
  never *when* — it's kept clear of the Swing timing entirely. On a hit it
  does one of three things at even odds: skip an extra step, repeat the
  step it just left, or jump to a random one. Draws from the same PRNG
  stream as Mutate, so it stays deterministic relative to the seed. 0 =
  off (default), and at 0 it's a complete no-op.
- **Auto Gen** — Off, or every 1 / 2 / 4 / 8 / 16 / 32 bars, re-roll both
  sequencers from a fresh seed (the same thing Generate does), on its own
  bar counter independent of Reset Both. Set to the same interval as Reset
  Both and both fire on the tick: regenerate, then snap to step 1.

## Install

Needs **Schwung v1.2.0 or later** on the Move (`min_host_version` in
`module.json`).

### From a release (no build)

1. Download `acid-module.tar.gz` from the
   [latest release](https://github.com/sd88me/schwung-acid/releases/latest).
2. Copy it to the Move and unpack it into the `midi_fx` module folder:

   ```bash
   scp acid-module.tar.gz ableton@move.local:/data/UserData/
   ssh ableton@move.local \
     'tar -xzf /data/UserData/acid-module.tar.gz -C /data/UserData/schwung/modules/midi_fx/'
   ```

   The tarball holds a single `acid/` folder, so this lands it at
   `/data/UserData/schwung/modules/midi_fx/acid/`.
3. Power-cycle the Move (or rescan modules).

**Acid** then appears as an option in a MIDI FX chain slot. Route it to a
sound generator in the same slot, press Play.

### From source

```bash
git clone https://github.com/sd88me/schwung-acid
cd schwung-acid
./scripts/build.sh                                   # produces dist/acid-module.tar.gz
MOVE_HOST=ableton@move.local ./scripts/install.sh    # scp's dist/acid/ to the Move
```

Then power-cycle / rescan as above.

## Build from source

Requires Docker (cross-compiles the DSP for the Move's ARM64 chip). No Schwung
checkout is needed — the two host ABI headers are vendored in `src/include/`
(see `src/include/README.md` for how to re-sync them).

```bash
bash scripts/build.sh
# produces dist/acid-module.tar.gz  and  dist/acid/  (module.json, help.json, dsp.so)
```

## Repository layout

```
src/
  acid/
    module.json  help.json
    dsp/acid.c
  include/          # vendored Schwung host ABI headers (committed)
scripts/
  build.sh  install.sh  Dockerfile
```

## Changelog

- **v1.1.1** — Offset A / Offset B knob feel: declared as normalised-curve
  floats (like Length A/B and Swing) so they no longer step one value per
  detent across the 0–31 span. No behaviour change.
- **v1.1** — new **Advanced** page: per-sequencer **Offset** (read-side
  step rotation) and **Direction** (Fwd / Rev / Pendulum), plus shared
  **Jitter** (occasional skip / repeat / jump) and **Auto Gen** (re-roll
  both sequencers every 1–32 bars). Six curated scales added after the
  original six — Phrygian Dominant, Locrian, Whole Tone, Hungarian Minor,
  Minor Blues, Chromatic — with the first six unchanged so stored `scale`
  values keep their meaning.
- **v1.0.1** — packaging only: installable as a custom module,
  `min_host_version` 1.2.0.
- **v1.0** — feature-complete first release.

## Credits & license

Generation ideas drawn from two references (see the design doc for detail):
- [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) by Charles
  Vestal (GPL-3.0), itself a port of djphazer's `TB_3PO` applet from the
  [O_C-Phazerville](https://github.com/djphazer/O_C-Phazerville) Hemisphere
  Suite — the primary generator (Algo 1) is adapted from this model.
- `Sting.amxd` ("Sting 2.26" by Iftah Gabbai, CC BY-NC-ND) — the secondary
  generator's *mechanisms* (urn-style non-repeating draw, random-walk gate
  density) are independently reimplemented, inspired by the patch's approach
  but not copied from it.

Created by sd88me for [Schwung](https://github.com/charlesvestal/schwung).
