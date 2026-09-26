/* Acid -- dual generative acid-bassline sequencer, slot MIDI FX for Schwung.
 *
 * Two independent 8..32-step generators (Seq A / Seq B) free-run off Move's
 * transport, sharing one Root/Scale. Each sequencer's own "Algo" knob (1-16)
 * blends a primary generator (density/accent/slide/octave probabilistic
 * model, ported from schwung-tb3po / the Phazerville TB_3PO applet) with a
 * secondary generator (Sting.amxd-inspired: urn-style non-repeating pitch
 * draw, a density-modulated bounded random walk for gate/rest, and a fixed
 * permutation ("VelPyra") accent shape instead of independent per-step
 * accent rolls). Algo=1 is 100% primary -- byte-for-byte the same generation
 * behaviour as tb3po's model. Algo=16 is mostly secondary.
 *
 * Because a MIDI FX slot forwards process_midi/tick output to exactly ONE
 * synth on ONE channel (chain_midi.c overwrites the channel byte with the
 * slot's recv channel regardless of what we send), Seq A and Seq B are not
 * routed to separate synths the way tb3po's two Tool slots are. Instead
 * both merge into the single output stream, mixed by a bipolar Blend knob
 * (-63..64) that crossfades the two by scaling each side's own velocity:
 * -63 = A only @127, 0 = both @100, +64 = B only @127.
 *
 * Blend doubles as Seq B's on/off -- at -63 it mutes B outright -- so the
 * knob that would otherwise be a B enable is Tune B instead: B's interval
 * from A in semitones (+/-24), layered on top of Root and live transpose.
 *
 * Swing (50-75%, MPC-style 16th) is shared by both sequencers off one
 * clock, so they always stay swung together regardless of Reset Both or
 * differing lengths -- see swing_delay_frac()/swing_gap_mult() for the
 * timing math.
 *
 * No banks, no undo, no persistence in this version -- Generate/Mutate only.
 * An incoming note-on transposes both sequencers live, relative to C4
 * (hybrid trigger model) -- the Root knob is left untouched, and Tune B's
 * interval rides along, so A and B stay locked at the interval you set.
 * Sequencing itself keeps running regardless of note input.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "plugin_api_v1.h"
#include "midi_fx_api_v1.h"

#define MAX_STEPS   32
#define MIN_LENGTH  2
#define NUM_SEQS    2
#define SEQ_A       0
#define SEQ_B       1
#define OUT_CH      0   /* channel byte is overwritten by the chain host anyway */
#define ACID_ANCHOR_NOTE 60  /* incoming note that means "no transpose" (C4, like tb3po-lite) */
#define ACID_MAX_TUNE 24     /* Seq B's interval from Seq A, +/- two octaves */
#define ACID_MAX_TRANSPOSE 48

typedef enum { STEP_REST = 0, STEP_NOTE = 1, STEP_ACCENT = 2, STEP_SLIDE = 3 } step_kind_t;

/* Scale degrees in semitones from root -- ported verbatim from tb3po. */
typedef struct { const char *name; int degrees[12]; int len; } scale_t;
static const scale_t SCALES[] = {
    { "Minor",     {0, 2, 3, 5, 7, 8, 10},         7 },
    { "Phrygian",  {0, 1, 3, 5, 7, 8, 10},         7 },
    { "HarmMinor", {0, 2, 3, 5, 7, 8, 11},         7 },
    { "MinPent",   {0, 3, 5, 7, 10, 0, 0},         5 },
    { "Dorian",    {0, 2, 3, 5, 7, 9, 10},         7 },
    { "Major",     {0, 2, 4, 5, 7, 9, 11},         7 },
    /* Appended for v1.1 -- a curated acid/techno-leaning set, not an attempt
     * at completeness. Order matters: these must stay at indices 6..11 so any
     * stored/automated `scale` value <= 5 keeps its meaning. note_for_step(),
     * gen_primary(), gen_secondary() and mutate_pattern() all read SCALES[]
     * generically, and NUM_SCALES is sizeof-derived, so nothing else changes
     * with the list. Chromatic (len 12) is the one that functionally answers
     * "scale defeat" without a second code path -- degrees[12]/len already
     * supports it. */
    { "PhrygDom",  {0, 1, 4, 5, 7, 8, 10},         7 },  /* Phrygian Dominant / Spanish */
    { "Locrian",   {0, 1, 3, 5, 6, 8, 10},         7 },
    { "WholeTone", {0, 2, 4, 6, 8, 10},            6 },
    { "HungMinor", {0, 2, 3, 6, 7, 8, 11},         7 },  /* Hungarian / Gypsy Minor */
    { "MinBlues",  {0, 3, 5, 6, 7, 10},            6 },
    { "Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12 }
};
#define NUM_SCALES ((int)(sizeof(SCALES) / sizeof(SCALES[0])))

/* Sting.amxd's "VelPyra" fixed permutation -- an evenly-spread-but-irregular
 * accent order (used by the secondary generator instead of independent
 * per-step accent rolls). Values 0-15, taken directly from the patch --
 * kept deliberately for now (v1: match/learn the reference's actual feel on
 * hardware before designing an original replacement for final release; see
 * the design doc and README credit for the license/attribution status of
 * this specific table). */
static const uint8_t VEL_PYRAMID[16] = {0, 14, 15, 6, 1, 2, 10, 3, 12, 13, 11, 5, 4, 8, 7, 9};

/* Bar-length lookup for the Reset Both control: index -> 16th-notes per bar
 * boundary (index 4 = "Off", handled separately, never indexes this). */
static const int BAR_STEPS[4] = {16, 32, 64, 128};

/* Auto Gen (Advanced page) bar-boundary lookup: index 1..6 -> 16th-notes per
 * automatic-regenerate boundary; index 0 = "Off", handled separately and
 * never indexes this. Kept separate from BAR_STEPS[] on purpose -- Auto Gen's
 * enum puts "Off" first and reaches further (up to 32 bars) than Reset Both. */
static const int AUTO_GEN_BAR_STEPS[6] = {16, 32, 64, 128, 256, 512};

typedef struct {
    /* PRNG -- xorshift32. `rng` is the live, ever-advancing generator state
     * (mutate consumes from wherever it currently sits); `seed` is the last
     * value a full Generate was seeded with, kept for reference/debugging. */
    uint32_t rng;
    uint32_t seed;

    /* Pattern */
    uint8_t steps[MAX_STEPS];
    uint8_t degrees[MAX_STEPS];
    uint8_t octaves[MAX_STEPS];
    int length;
    int position;

    /* Knobs */
    float density, accent, slide;
    int octave_range;   /* 1..3 */
    float gate;         /* 0.05..1.0, fraction of a step */
    int algo;           /* 1..16 */

    /* Advanced page -- per-sequencer playback modifiers. Offset is read-side
     * only (rotates which stored step plays, never touches steps[]/degrees[]/
     * octaves[]); Direction changes how `position` advances each tick. */
    int offset;         /* 0..length-1 rotation applied when reading a step */
    int dir;            /* 0=Fwd, 1=Rev, 2=Pendulum */
    int pendulum_fwd;   /* dir==2 only: 1 = currently travelling forward */

    /* Playback */
    int tune;           /* semitone offset from the shared key; Seq A is always
                         * 0, Seq B is the user's A->B interval. Applied on top
                         * of root + live_transpose, so MIDI transposition moves
                         * both sequencers and preserves the interval. */
    int last_note_on;   /* -1 = none */
    int portamento_on;
    long gate_samples_remaining;
} acid_seq_t;

typedef struct {
    acid_seq_t seq[NUM_SEQS];

    int root;             /* 0-11, knob/preset only -- never written by MIDI in */
    int live_transpose;   /* semitones from incoming notes, anchored at C4; kept
                           * separate from `root` so playing notes (or an echo of
                           * our own output) never moves the Root knob/field */
    int scale;            /* index into SCALES */
    int blend;             /* -63..64, A<->B sweep; meaning depends on blend_mode */
    int chain_side;        /* Chain mode: 0 = A's phrase, 1 = B's */
    int chain_pass;        /* passes completed on the current side */
    int chain_step;        /* steps played in the current pass */
    int chain_fresh;       /* 1 = next tick begins the chain from the top */
    int blend_mode;        /* BM_* -- see blend_pick() */
    int reset_bars_idx;   /* 0..3 -> {1,2,4,8} bars, 4 = Off */
    int swing_pct;         /* 50-75, MPC-style 16th swing; 50 = straight.
                           * Whole percent, but the chain_param below is
                           * declared "float" (like Length A/B) so the knob
                           * rides the range-normalised curve instead of
                           * one-step-per-detent -- an int chain_param over
                           * this same 50-75 span felt too twitchy. */
    long swing_pulse_idx;  /* count of 16th pulses fired since Start -- pulse 0
                            * is always exactly on the grid, odd pulses land
                            * late and the following even pulse lands early by
                            * the same amount, so tempo never drifts. Shared
                            * between the internal free-run clock and the
                            * external 24 PPQN follow so swing survives a
                            * hand-off between the two. */

    /* Shared clock -- ported from tb3po's dual-slot clock handling. */
    float bpm;
    int running;
    int follow_transport;
    int clock_pulses;
    double samples_per_step;
    double sample_accum;
    int pulse_sync_active;
    long blocks_since_last_pulse;
    int poll_counter;
    float host_bpm;
    int host_clock_status;
    int clock_stable_count;

    long bar_step_count;  /* 16th-notes advanced since the last bar-reset */

    /* Advanced page -- shared across both sequencers. */
    float jitter;              /* 0.0..1.0: per-tick chance of perturbing WHICH
                               * step plays (not WHEN -- kept clear of swing) */
    int auto_gen_idx;          /* 0=Off, 1..6 -> {1,2,4,8,16,32} bars */
    long auto_gen_step_count;  /* own counter, parallel to bar_step_count, so
                               * Auto Gen and Reset Both keep independent
                               * intervals rather than sharing one */
} acid_inst_t;

static const host_api_v1_t *g_host = NULL;

/* ---------------------------------------------------------------------- */
/* PRNG                                                                    */
/* ---------------------------------------------------------------------- */

static uint32_t rng_next_u32(uint32_t *rng) {
    uint32_t x = *rng;
    if (x == 0) x = 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x;
    return x;
}

static float rng_next_f(uint32_t *rng) {
    return (float)(rng_next_u32(rng) & 0xFFFFFF) / (float)0x1000000;
}

/* ---------------------------------------------------------------------- */
/* Primary generator -- tb3po's density/accent/slide/octave model verbatim */
/* ---------------------------------------------------------------------- */

static void gen_primary(const acid_seq_t *s, uint32_t *rng, int scale_idx,
                         uint8_t *out_steps, uint8_t *out_deg, uint8_t *out_oct) {
    const scale_t *sc = &SCALES[scale_idx];
    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(rng) > s->density) {
            out_steps[i] = STEP_REST;
            continue;
        }
        int on_beat = (i % 4 == 0);
        int degree;
        if (on_beat && rng_next_f(rng) < 0.35f) {
            degree = 0;
        } else {
            degree = (int)(rng_next_f(rng) * (float)sc->len);
            if (degree >= sc->len) degree = sc->len - 1;
        }
        out_deg[i] = (uint8_t)degree;

        int oct = (int)(rng_next_f(rng) * (float)s->octave_range);
        if (oct >= s->octave_range) oct = s->octave_range - 1;
        out_oct[i] = (uint8_t)oct;

        int kind = STEP_NOTE;
        if (rng_next_f(rng) < s->accent) kind = STEP_ACCENT;
        if (rng_next_f(rng) < s->slide) kind = STEP_SLIDE;
        out_steps[i] = (uint8_t)kind;
    }

    /* Slide-into-rest is a no-op -- demote to a plain note. */
    for (int i = 0; i < s->length; i++) {
        int nxt = (i + 1) % s->length;
        if (out_steps[i] == STEP_SLIDE && out_steps[nxt] == STEP_REST) out_steps[i] = STEP_NOTE;
    }

    int any = 0;
    for (int i = 0; i < s->length; i++) if (out_steps[i] != STEP_REST) { any = 1; break; }
    if (!any) { out_steps[0] = STEP_NOTE; out_deg[0] = 0; out_oct[0] = 0; }
}

/* ---------------------------------------------------------------------- */
/* Secondary generator -- Sting.amxd-inspired                              */
/* ---------------------------------------------------------------------- */

static void gen_secondary(const acid_seq_t *s, uint32_t *rng, int scale_idx,
                           uint8_t *out_steps, uint8_t *out_deg, uint8_t *out_oct) {
    const scale_t *sc = &SCALES[scale_idx];

    /* "Classic" (urn 12/16 in the patch): a bag of scale-degree indices,
     * drawn without repeat until exhausted, then reshuffled by refilling. */
    uint8_t bag[12];
    int bag_n = sc->len;
    for (int k = 0; k < bag_n; k++) bag[k] = (uint8_t)k;

    /* "CakeWalk": a bounded random walk drives gate/rest density instead of
     * an independent per-step coin flip. Density knob sets where the walk
     * starts (and roughly where it idles). */
    int walk = (int)(s->density * 8.0f);
    if (walk < 0) walk = 0;
    if (walk > 8) walk = 8;

    /* "VelPyra": mark the first N pyramid slots (N set by the Accent knob)
     * as accented, instead of rolling accent independently per step. */
    int accent_count = (int)(s->accent * 16.0f + 0.5f);
    if (accent_count > 16) accent_count = 16;
    uint8_t accented[MAX_STEPS];
    memset(accented, 0, sizeof(accented));
    for (int k = 0; k < accent_count; k++) {
        int pos = VEL_PYRAMID[k] % s->length;
        accented[pos] = 1;
    }

    for (int i = 0; i < s->length; i++) {
        int step_delta = (int)(rng_next_f(rng) * 3.0f) - 1; /* -1, 0, +1 */
        walk += step_delta;
        if (walk < 0) walk = 0;
        if (walk > 8) walk = 8;

        if (walk < 2) {
            out_steps[i] = STEP_REST;
            continue;
        }

        if (bag_n == 0) {
            bag_n = sc->len;
            for (int k = 0; k < bag_n; k++) bag[k] = (uint8_t)k;
        }
        int idx = (int)(rng_next_f(rng) * (float)bag_n);
        if (idx >= bag_n) idx = bag_n - 1;
        int degree = bag[idx];
        bag[idx] = bag[--bag_n];
        out_deg[i] = (uint8_t)degree;

        int oct = (int)(rng_next_f(rng) * (float)s->octave_range);
        if (oct >= s->octave_range) oct = s->octave_range - 1;
        out_oct[i] = (uint8_t)oct;

        int kind = accented[i] ? STEP_ACCENT : STEP_NOTE;
        if (rng_next_f(rng) < s->slide) kind = STEP_SLIDE;
        out_steps[i] = (uint8_t)kind;
    }

    for (int i = 0; i < s->length; i++) {
        int nxt = (i + 1) % s->length;
        if (out_steps[i] == STEP_SLIDE && out_steps[nxt] == STEP_REST) out_steps[i] = STEP_NOTE;
    }

    int any = 0;
    for (int i = 0; i < s->length; i++) if (out_steps[i] != STEP_REST) { any = 1; break; }
    if (!any) { out_steps[0] = STEP_NOTE; out_deg[0] = 0; out_oct[0] = 0; }
}

/* Algo (1-16) sets the per-step substitution weight of secondary into
 * primary: 1 = 100% primary (tb3po's exact behaviour), 16 = mostly
 * secondary -- directly reproducing the mechanism a comment in Sting.amxd
 * describes ("which notes in the random sequence will be replaced by the
 * 2nd generator and how often"). One evolving PRNG stream is threaded
 * through primary generation, secondary generation and the mix decision,
 * and its final state is persisted so a later Mutate keeps evolving from
 * here rather than repeating itself. */
static void regenerate_pattern(acid_seq_t *s, int scale_idx, uint32_t seed) {
    uint32_t rng = seed ? seed : 1;
    s->seed = seed;

    uint8_t p_steps[MAX_STEPS], p_deg[MAX_STEPS], p_oct[MAX_STEPS];
    gen_primary(s, &rng, scale_idx, p_steps, p_deg, p_oct);

    float weight = (float)(s->algo - 1) / 15.0f;
    if (weight <= 0.0f) {
        memcpy(s->steps, p_steps, MAX_STEPS);
        memcpy(s->degrees, p_deg, MAX_STEPS);
        memcpy(s->octaves, p_oct, MAX_STEPS);
        s->rng = rng;
        return;
    }

    uint8_t sec_steps[MAX_STEPS], sec_deg[MAX_STEPS], sec_oct[MAX_STEPS];
    gen_secondary(s, &rng, scale_idx, sec_steps, sec_deg, sec_oct);

    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(&rng) < weight) {
            s->steps[i] = sec_steps[i]; s->degrees[i] = sec_deg[i]; s->octaves[i] = sec_oct[i];
        } else {
            s->steps[i] = p_steps[i]; s->degrees[i] = p_deg[i]; s->octaves[i] = p_oct[i];
        }
    }
    s->rng = rng;
}

/* Nudge ~25% of steps -- ported from tb3po's mutate_pattern. Algorithm-
 * agnostic: it nudges whatever pattern is currently live, regardless of
 * which Algo blend produced it. */
static void mutate_pattern(acid_seq_t *s, int scale_idx) {
    const scale_t *sc = &SCALES[scale_idx];
    uint32_t rng = s->rng;
    for (int i = 0; i < s->length; i++) {
        if (rng_next_f(&rng) >= 0.25f) continue;
        if (rng_next_f(&rng) < 0.5f) {
            if (rng_next_f(&rng) > s->density) {
                s->steps[i] = STEP_REST;
            } else {
                int kind = STEP_NOTE;
                if (rng_next_f(&rng) < s->accent) kind = STEP_ACCENT;
                if (rng_next_f(&rng) < s->slide) kind = STEP_SLIDE;
                s->steps[i] = (uint8_t)kind;
            }
        } else {
            int degree = (int)(rng_next_f(&rng) * (float)sc->len);
            if (degree >= sc->len) degree = sc->len - 1;
            s->degrees[i] = (uint8_t)degree;
            int oct = (int)(rng_next_f(&rng) * (float)s->octave_range);
            if (oct >= s->octave_range) oct = s->octave_range - 1;
            s->octaves[i] = (uint8_t)oct;
        }
    }
    s->rng = rng;
}

/* ---------------------------------------------------------------------- */
/* Playback                                                                */
/* ---------------------------------------------------------------------- */

/* Offset (Advanced page) is a pure read-side rotation: the stored pattern is
 * never modified, we just index it `offset` steps further along. length >= 2
 * and both operands are non-negative, so the result is always in range. */
static int play_idx(const acid_seq_t *s, int pos) {
    return (pos + s->offset) % s->length;
}

static int note_for_step(const acid_seq_t *s, int scale_idx, int root, int transpose, int step_idx) {
    const scale_t *sc = &SCALES[scale_idx];
    /* Base in the C1 octave, matching tb3po -- keeps the emitted range well
     * clear of Move's pad-LED note range. `transpose` is the live offset from
     * incoming notes (0 = play at the Root knob's key); `s->tune` is this
     * sequencer's own interval on top of that. */
    int base = 24 + root + transpose + s->tune;
    int note = base + sc->degrees[s->degrees[step_idx]] + 12 * s->octaves[step_idx];
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

/* Advance `position` one step per the Direction mode. Pendulum is stateful --
 * it flips pendulum_fwd at each end -- so this takes a mutable pointer rather
 * than staying the pure function it was. Endpoints are turn-around points,
 * not repeated: 0,1,2,..,N-1,N-2,..,1,0,1,.. */
static int next_position(acid_seq_t *s) {
    int n = s->length;
    if (n <= 1) return 0;
    switch (s->dir) {
        case 1: /* Rev */
            return (s->position - 1 + n) % n;
        case 2: /* Pendulum */
            if (s->pendulum_fwd) {
                if (s->position >= n - 1) { s->pendulum_fwd = 0; return n - 2; }
                return s->position + 1;
            }
            if (s->position <= 0) { s->pendulum_fwd = 1; return 1; }
            return s->position - 1;
        default: /* Fwd */
            return (s->position + 1) % n;
    }
}

/* Blend Mode -- every mode keeps the output a single monophonic line by
 * deciding, per step, WHICH sequencer is audible (the other is muted, which
 * also kills its ringing note). Blend then sweeps w = 0 (A alone) .. 1 (B
 * alone) using a fixed per-step threshold (a bit-reversed 16-step order) so
 * the hand-over between sources is even and repeatable, never random.
 *   Morph -- each step comes from A or B.
 *   Split -- rhythm (note/rest/slide/accent) always from A; pitch from B
 *            on the steps the threshold hands to B.
 *   Fill  -- OR, A priority: B plays where A rests.
 *   XOR   -- plays where exactly one of A/B has a note.
 *   Lock  -- AND: plays (A's note) only where both have a note.
 *   Chain -- call and response: A plays a full pass, then B, then A...
 *            Blend sets the pass ratio (see chain_counts()). Not per step.
 * The logic modes sweep A -> logic result (centre) -> B. */
enum { BM_MORPH = 0, BM_SPLIT, BM_FILL, BM_XOR, BM_LOCK, BM_CHAIN, NUM_BLEND_MODES };

static const uint8_t BLEND_THRESH[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

/* Decide the audible sequencer for this step: 0 = A, 1 = B, -1 = silence.
 * gate_a/gate_b: does that sequencer have a note on this step. *pitch_from_b
 * is set only for Split. */
static int blend_pick(const acid_inst_t *t, int gate_a, int gate_b, int *pitch_from_b) {
    float w = (float)(t->blend + 63) / 127.0f;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    float thr = ((float)BLEND_THRESH[t->swing_pulse_idx & 15] + 0.5f) / 16.0f;
    *pitch_from_b = 0;

    if (t->blend_mode == BM_MORPH) return (thr < w) ? 1 : 0;
    if (t->blend_mode == BM_SPLIT) { *pitch_from_b = (thr < w); return 0; }

    /* Logic modes: three-way source A / result / B. */
    int src; /* 0 = A alone, 1 = logic result, 2 = B alone */
    if (w < 0.5f) src = (thr < w * 2.0f) ? 1 : 0;
    else          src = (thr < (w - 0.5f) * 2.0f) ? 2 : 1;
    if (src == 0) return gate_a ? 0 : -1;
    if (src == 2) return gate_b ? 1 : -1;
    switch (t->blend_mode) {
        case BM_FILL: return gate_a ? 0 : (gate_b ? 1 : -1);
        case BM_XOR:  return (gate_a && !gate_b) ? 0 : ((gate_b && !gate_a) ? 1 : -1);
        default:      return (gate_a && gate_b) ? 0 : -1; /* Lock */
    }
}

static void recompute_step_length(acid_inst_t *t, int sample_rate) {
    double sixteenths_per_sec = (double)t->bpm / 60.0 * 4.0;
    if (sixteenths_per_sec <= 0) sixteenths_per_sec = 8.0;
    t->samples_per_step = (double)sample_rate / sixteenths_per_sec;
}

/* Swing -- MPC-style 16th: swing_pct runs 50 (straight) to 75 (the point
 * past which it stops reading as swing and starts reading as a different
 * subdivision, per the classic MPC ceiling), same convention Move's own
 * Groove control approximates -- 0%/100%/beyond map onto 50%/66.7%/75%
 * here. 50% -> 2/3 gives the textbook 8th-note-triplet feel; 75% pushes
 * the off-16th almost onto the next downbeat.
 *
 * Delayed as a fraction of one *pair* of 16th steps: the odd (off-beat)
 * pulse of each pair arrives late by this fraction of a step, and the
 * following even pulse arrives early by the same amount, so every pair
 * still spans exactly two steps and the average tempo never drifts. */
static double swing_delay_frac(const acid_inst_t *t) {
    if (t->swing_pct <= 50) return 0.0;
    double d = (double)(t->swing_pct - 50) / 50.0; /* 0 .. 0.5 at pct=75 */
    if (d > 0.5) d = 0.5;
    return d;
}

/* Multiplier on samples_per_step for the gap leading INTO the next pulse
 * (internal free-run clock). Pulse 0 (first step after Start) is always
 * exactly on the grid. */
static double swing_gap_mult(const acid_inst_t *t) {
    if (t->swing_pulse_idx <= 0) return 1.0;
    double d = swing_delay_frac(t);
    if (d <= 0.0) return 1.0;
    return (t->swing_pulse_idx & 1) ? (1.0 + d) : (1.0 - d);
}

/* Same idea in units of 24-PPQN clock pulses (6 nominal per 16th step),
 * for the external MIDI-clock-follow path. Rounds to whole pulses since
 * that clock can't subdivide further; a pair (e.g. 8+4 at max swing)
 * still sums to the unswung 12. */
static int swing_pulse_target(const acid_inst_t *t) {
    double mult = swing_gap_mult(t);
    int target = (int)(6.0 * mult + 0.5);
    if (target < 1) target = 1;
    return target;
}

static int kill_seq_note(acid_inst_t *t, int seq_idx, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    (void)t;
    acid_seq_t *s = &t->seq[seq_idx];
    if (s->last_note_on < 0 || max_out < 1) return 0;
    out_msgs[0][0] = 0x80 | OUT_CH;
    out_msgs[0][1] = (uint8_t)s->last_note_on;
    out_msgs[0][2] = 0;
    out_lens[0] = 3;
    s->last_note_on = -1;
    return 1;
}

static int kill_all_notes(acid_inst_t *t, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    for (int i = 0; i < NUM_SEQS && count < max_out; i++) {
        count += kill_seq_note(t, i, &out_msgs[count], &out_lens[count], max_out - count);
    }
    return count;
}

/* Emits (or silences) one sequencer's current step. vel_scale (0-127) is
 * 127 when Blend Mode has made this sequencer audible for the step and 0
 * when it hasn't (which mutes it outright and kills its ringing note).
 * Rhythm/kind/slide come from seq_idx; pitch comes from pitch_seq (== seq_idx
 * except in Split mode). */
static int emit_step_for_seq(acid_inst_t *t, int seq_idx, int pitch_seq, int prev_pos, int vel_scale,
                              uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_seq_t *s = &t->seq[seq_idx];
    int count = 0;
    int pidx = play_idx(s, s->position);
    uint8_t kind = s->steps[pidx];

    if (kind == STEP_REST || vel_scale <= 0) {
        if (count < max_out) count += kill_seq_note(t, seq_idx, &out_msgs[count], &out_lens[count], max_out - count);
        s->gate_samples_remaining = 0;
        return count;
    }

    const acid_seq_t *ps = &t->seq[pitch_seq];
    int note = note_for_step(ps, t->scale, t->root, t->live_transpose, play_idx(ps, ps->position));
    int is_accent = (kind == STEP_ACCENT);
    int base_vel = is_accent ? 118 : 72;
    int vel = (base_vel * vel_scale) / 127;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;

    int was_slide = (prev_pos >= 0 && s->steps[play_idx(s, prev_pos)] == STEP_SLIDE);

    if (was_slide) {
        if (!s->portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | OUT_CH; out_msgs[count][1] = 65; out_msgs[count][2] = 127;
            out_lens[count] = 3; count++;
            s->portamento_on = 1;
        }
        if (count < max_out) {
            out_msgs[count][0] = 0x90 | OUT_CH; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
            out_lens[count] = 3; count++;
        }
        if (s->last_note_on >= 0 && s->last_note_on != note && count < max_out) {
            out_msgs[count][0] = 0x80 | OUT_CH; out_msgs[count][1] = (uint8_t)s->last_note_on; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
        }
    } else {
        if (s->portamento_on && count < max_out) {
            out_msgs[count][0] = 0xB0 | OUT_CH; out_msgs[count][1] = 65; out_msgs[count][2] = 0;
            out_lens[count] = 3; count++;
            s->portamento_on = 0;
        }
        if (s->last_note_on >= 0 && count < max_out) {
            count += kill_seq_note(t, seq_idx, &out_msgs[count], &out_lens[count], max_out - count);
        }
        if (count < max_out) {
            out_msgs[count][0] = 0x90 | OUT_CH; out_msgs[count][1] = (uint8_t)note; out_msgs[count][2] = (uint8_t)vel;
            out_lens[count] = 3; count++;
        }
    }
    s->last_note_on = note;
    s->gate_samples_remaining = (long)(t->samples_per_step * s->gate);
    return count;
}

/* Chain mode: passes each side plays before handing over. Blend w (0 = A
 * alone .. 1 = B alone) maps to the B:A ratio r = w/(1-w): centre = 1:1,
 * w = 0.25 -> A x3 : B x1, w = 0.75 -> A x1 : B x3, capped at 8; the far ends
 * are A only / B only. */
static void chain_counts(const acid_inst_t *t, int *na, int *nb) {
    float w = (float)(t->blend + 63) / 127.0f;
    if (w <= 0.02f) { *na = 1; *nb = 0; return; }
    if (w >= 0.98f) { *na = 0; *nb = 1; return; }
    float r = w / (1.0f - w);
    if (r <= 1.0f) { *na = (int)(1.0f / r + 0.5f); *nb = 1; }
    else           { *na = 1; *nb = (int)(r + 0.5f); }
    if (*na > 8) *na = 8;
    if (*nb > 8) *nb = 8;
}

/* Park a sequencer one step before its first step, so the next
 * next_position() lands on it: Fwd -> 0, Rev -> length-1, Pendulum -> 0
 * heading forward. */
static void chain_start_pass(acid_seq_t *s) {
    if (s->dir == 1)      s->position = 0;               /* Rev: next = length-1 */
    else if (s->dir == 2) { s->position = 1; s->pendulum_fwd = 0; } /* next = 0 */
    else                  s->position = s->length - 1;   /* Fwd: next = 0 */
    if (s->dir != 2) s->pendulum_fwd = 1;
}

/* Called at the top of a tick in Chain mode: begin the chain, or finish a
 * pass (one pass = `length` steps) and hand over when the side's count of
 * passes is done. */
static int chain_begin_tick(acid_inst_t *t) {
    int na, nb;
    chain_counts(t, &na, &nb);
    if (t->chain_fresh) {
        t->chain_fresh = 0;
        t->chain_side = (na > 0) ? 0 : 1;
        t->chain_pass = 0;
    } else if (t->chain_step >= t->seq[t->chain_side].length) {
        t->chain_pass++;
        int n = t->chain_side ? nb : na;
        if (t->chain_pass >= n) {
            int other = !t->chain_side;
            if ((other ? nb : na) > 0) t->chain_side = other;
            t->chain_pass = 0;
        }
    } else {
        return 0;   /* mid-pass */
    }
    chain_start_pass(&t->seq[t->chain_side]);
    t->chain_step = 0;
    return 1;
}

/* Advances both sequencers by one 16th-note tick, applies the Reset Both
 * bar-boundary snap when armed, and emits through the Blend crossfade.
 * Each sequencer wraps independently at its own Length every tick -- that
 * independent wrap is what makes differently-lengthed A/B patterns a real
 * polymeter when Reset Both is Off, rather than something that needs
 * explicit polymeter support. */
static int advance_all(acid_inst_t *t, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;

    int forced_reset = 0;
    if (t->reset_bars_idx != 4) {
        t->bar_step_count++;
        if (t->bar_step_count >= BAR_STEPS[t->reset_bars_idx]) {
            t->bar_step_count = 0;
            forced_reset = 1;
        }
    } else {
        t->bar_step_count = 0;
    }

    /* Auto Gen (Advanced) -- its own bar counter, independent of Reset Both's.
     * On its boundary, re-roll both sequencers from a fresh per-seq seed (the
     * same call the "generate" param makes). Done before the position snap /
     * emit below, so the freshly generated pattern is what plays this tick. If
     * Auto Gen and Reset Both are set to the same interval they fire on the
     * same tick: regenerate first, then snap to step 0 -- intended. */
    if (t->auto_gen_idx != 0) {
        t->auto_gen_step_count++;
        if (t->auto_gen_step_count >= AUTO_GEN_BAR_STEPS[t->auto_gen_idx - 1]) {
            t->auto_gen_step_count = 0;
            for (int i = 0; i < NUM_SEQS; i++) {
                acid_seq_t *s = &t->seq[i];
                regenerate_pattern(s, t->scale, rng_next_u32(&s->rng));
                if (s->position >= s->length) s->position = s->length - 1;
            }
        }
    } else {
        t->auto_gen_step_count = 0;
    }

    int prev[NUM_SEQS];
    int chain = (t->blend_mode == BM_CHAIN);
    if (chain) {
        if (forced_reset) {
            /* Reset Both: the chain restarts at the top of A's phrase. */
            t->chain_fresh = 0; t->chain_side = 0; t->chain_pass = 0; t->chain_step = 0;
            chain_start_pass(&t->seq[SEQ_A]);
        } else {
            chain_begin_tick(t);
        }
    }
    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        prev[i] = s->position;
        if (chain && i != t->chain_side) continue;   /* the resting side holds still */
        if (chain && t->chain_step == 0) prev[i] = -1; /* a pass opens cold: no slide in from its parked step */
        if (forced_reset) {
            s->position = 0;
            s->pendulum_fwd = 1;  /* Reset Both is authoritative -- pendulum resumes forward */
        } else {
            int np = next_position(s);
            /* Jitter (Advanced) -- perturb WHICH step plays, never WHEN, so it
             * stays independent of swing_gap_mult()/swing_delay_frac(). Rolls
             * off the same per-seq PRNG stream Mutate draws from, so it stays
             * deterministic relative to the seed. Three outcomes, split evenly
             * for now: skip an extra step, repeat the step just left, or jump
             * somewhere random. The exact split is an ear-check call -- flagged
             * like the Algo blend curve and the VelPyra table, not settled. */
            if (t->jitter > 0.0f && s->length > 1 &&
                rng_next_f(&s->rng) < t->jitter) {
                switch (rng_next_u32(&s->rng) % 3u) {
                    case 0: np = (np + 1) % s->length; break;
                    case 1: np = prev[i]; break;
                    default: {
                        int r = (int)(rng_next_f(&s->rng) * (float)s->length);
                        if (r >= s->length) r = s->length - 1;
                        np = r;
                    }
                }
            }
            s->position = np;
        }
    }

    if (chain) t->chain_step++;

    /* Both positions are settled, so Blend Mode can see both steps. */
    int gate_a = t->seq[SEQ_A].steps[play_idx(&t->seq[SEQ_A], t->seq[SEQ_A].position)] != STEP_REST;
    int gate_b = t->seq[SEQ_B].steps[play_idx(&t->seq[SEQ_B], t->seq[SEQ_B].position)] != STEP_REST;
    int pitch_from_b;
    int pick = chain ? t->chain_side : blend_pick(t, gate_a, gate_b, &pitch_from_b);
    if (chain) pitch_from_b = 0;

    /* Muted sequencer first, so its note-off can never land after (and cut)
     * the audible sequencer's note-on when both share a pitch. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < NUM_SEQS; i++) {
            int audible = (i == pick) || (t->blend_mode == BM_SPLIT && i == SEQ_A);
            if ((pass == 0) == audible) continue;
            int pitch_seq = (i == SEQ_A && pitch_from_b) ? SEQ_B : i;
            if (count < max_out) {
                count += emit_step_for_seq(t, i, pitch_seq, prev[i], audible ? 127 : 0,
                                           &out_msgs[count], &out_lens[count], max_out - count);
            }
        }
    }
    return count;
}

/* ---------------------------------------------------------------------- */
/* Plugin API surface                                                     */
/* ---------------------------------------------------------------------- */

static void *acid_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    acid_inst_t *t = (acid_inst_t *)calloc(1, sizeof(acid_inst_t));
    if (!t) return NULL;

    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        s->rng = 0xBEEFu + (uint32_t)i;
        s->seed = s->rng;
        s->length = 16;
        s->density = 0.7f;
        s->accent = 0.4f;
        s->slide = 0.25f;
        s->octave_range = 2;
        s->gate = 0.5f;
        s->algo = 1;
        s->tune = 0;
        s->last_note_on = -1;
        s->pendulum_fwd = 1; /* offset/dir default to 0 (Fwd, no rotation) via calloc */
    }
    t->root = 9; /* A, matches tb3po's default */
    t->live_transpose = 0;
    t->scale = 0;
    t->blend = -63; /* Seq A alone until Blend is dialled up */
    t->reset_bars_idx = 4; /* Off */
    t->swing_pct = 50; /* straight */
    t->swing_pulse_idx = 0; t->chain_fresh = 1;
    t->bpm = 120.0f;
    t->running = 0;
    t->follow_transport = 1;

    if (g_host && g_host->get_clock_status) {
        if (g_host->get_clock_status() == MOVE_CLOCK_STATUS_RUNNING) t->running = 1;
    }

    recompute_step_length(t, MOVE_SAMPLE_RATE);

    for (int i = 0; i < NUM_SEQS; i++) {
        regenerate_pattern(&t->seq[i], t->scale, t->seq[i].seed);
        t->seq[i].position = t->seq[i].length - 1; /* first advance lands on 0 */
    }

    return t;
}

static void acid_destroy_instance(void *instance) {
    /* No output channel available here (set_param/destroy_instance carry no
     * out_msgs) to send note-offs -- the chain host is responsible for
     * silencing voices on unload, same as every other midi_fx module. */
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t) return;
    free(t);
}

static int acid_process_midi(void *instance, const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0];
    uint8_t type = status & 0xF0;

    if (status == 0xF8) { /* MIDI clock tick, 24 PPQN */
        t->pulse_sync_active = 1;
        t->blocks_since_last_pulse = 0;
        if (t->running) {
            t->clock_pulses++;
            if (t->clock_pulses >= swing_pulse_target(t)) {
                t->clock_pulses = 0;
                int fired = advance_all(t, out_msgs, out_lens, max_out);
                t->swing_pulse_idx++;
                return fired;
            }
        }
        return 0;
    }
    if (status == 0xFA) { /* Start */
        if (t->follow_transport) {
            t->running = 1;
            for (int i = 0; i < NUM_SEQS; i++) {
                t->seq[i].position = t->seq[i].length - 1;
                t->seq[i].pendulum_fwd = 1;
            }
            t->clock_pulses = 5; /* first 0xF8 bumps to 0 -> fires step 0 on the downbeat */
            t->sample_accum = 0;
            t->bar_step_count = 0;
            t->auto_gen_step_count = 0;
            t->swing_pulse_idx = 0; t->chain_fresh = 1;
        }
        return 0;
    }
    if (status == 0xFB) { /* Continue */
        if (t->follow_transport) t->running = 1;
        return 0;
    }
    if (status == 0xFC) { /* Stop */
        if (t->follow_transport) {
            t->running = 0;
            return kill_all_notes(t, out_msgs, out_lens, max_out);
        }
        return 0;
    }

    /* Hybrid trigger model: an incoming note transposes both sequencers live
     * (relative to C4), leaving the Root knob/field untouched -- so a clip on
     * the track, a stray pad, or an echo of our own output can't drag Root
     * around. Sequencing is not gated by note input. Note-on/off are both
     * swallowed (Acid generates its own stream, it does not pass notes through). */
    if (type == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        int tr = (int)in_msg[1] - ACID_ANCHOR_NOTE;
        if (tr < -ACID_MAX_TRANSPOSE) tr = -ACID_MAX_TRANSPOSE;
        if (tr >  ACID_MAX_TRANSPOSE) tr =  ACID_MAX_TRANSPOSE;
        t->live_transpose = tr;
        return 0;
    }
    if (type == 0x80 || (type == 0x90 && in_len >= 3 && in_msg[2] == 0)) {
        return 0;
    }

    /* Pass everything else (CC, pitch bend, program change...) through
     * unchanged -- same convention as the built-in arp. */
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int acid_tick(void *instance, int frames, int sample_rate,
                     uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t) return 0;
    int count = 0;

    if (t->pulse_sync_active) {
        t->blocks_since_last_pulse++;
        if (t->blocks_since_last_pulse > 200) { /* ~580ms of silence -> fall back to internal timer */
            t->pulse_sync_active = 0;
            t->clock_pulses = 0;
        }
        t->sample_accum = 0;
    }

    if ((++t->poll_counter & 0x1F) == 0 && g_host && g_host->get_bpm) {
        float hb = g_host->get_bpm();
        if (hb > 20.0f && hb < 400.0f && hb != t->host_bpm) {
            t->host_bpm = hb;
            t->bpm = hb;
            recompute_step_length(t, sample_rate);
        }
    }

    if (t->follow_transport && g_host && g_host->get_clock_status && ((t->poll_counter & 0x07) == 0)) {
        int cs = g_host->get_clock_status();
        if (cs == t->host_clock_status) {
            if (t->clock_stable_count < 32) t->clock_stable_count++;
        } else {
            t->host_clock_status = cs;
            t->clock_stable_count = 1;
        }
        if (t->clock_stable_count >= 8) {
            if (cs == MOVE_CLOCK_STATUS_RUNNING && !t->running) {
                t->running = 1;
                for (int i = 0; i < NUM_SEQS; i++) {
                    t->seq[i].position = t->seq[i].length - 1;
                    t->seq[i].pendulum_fwd = 1;
                }
                t->clock_pulses = 5;
                t->sample_accum = 0;
                t->bar_step_count = 0;
                t->auto_gen_step_count = 0;
                t->swing_pulse_idx = 0; t->chain_fresh = 1;
            } else if (cs == MOVE_CLOCK_STATUS_STOPPED && t->running) {
                t->running = 0;
                if (count < max_out) count += kill_all_notes(t, &out_msgs[count], &out_lens[count], max_out - count);
            }
        }
    }

    if (!t->running) return count;

    if (!t->pulse_sync_active) {
        t->sample_accum += (double)frames;
        while (count < max_out - 4) {
            double gap = t->samples_per_step * swing_gap_mult(t);
            if (t->sample_accum < gap) break;
            t->sample_accum -= gap;
            count += advance_all(t, &out_msgs[count], &out_lens[count], max_out - count);
            t->swing_pulse_idx++;
        }
    }

    /* Gate-off accounting, per sequencer -- unless the next-held step is a
     * slide (slide holds the note until the following note replaces it). */
    for (int i = 0; i < NUM_SEQS; i++) {
        acid_seq_t *s = &t->seq[i];
        if (s->gate_samples_remaining > 0) {
            s->gate_samples_remaining -= frames;
            if (s->gate_samples_remaining <= 0) {
                s->gate_samples_remaining = 0;
                if (s->steps[play_idx(s, s->position)] != STEP_SLIDE && count < max_out) {
                    count += kill_seq_note(t, i, &out_msgs[count], &out_lens[count], max_out - count);
                }
            }
        }
    }
    return count;
}

static int parse_int(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    return (int)strtol(s, NULL, 10);
}
static float parse_float(const char *s, float fallback) {
    if (!s || !*s) return fallback;
    return (float)strtod(s, NULL);
}

static int is_state_key(const char *key);
static void state_decode(acid_inst_t *t, const char *val);

static void acid_set_param(void *instance, const char *key, const char *val) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key) return;

    if (is_state_key(key)) { state_decode(t, val); return; }

    int seq_idx = -1;
    const char *k = key;
    if (key[0] == 'a' && key[1] == '_') { seq_idx = SEQ_A; k = key + 2; }
    else if (key[0] == 'b' && key[1] == '_') { seq_idx = SEQ_B; k = key + 2; }

    if (seq_idx >= 0) {
        acid_seq_t *s = &t->seq[seq_idx];
        if (strcmp(k, "generate") == 0) {
            uint32_t seed = rng_next_u32(&s->rng);
            regenerate_pattern(s, t->scale, seed);
            if (s->position >= s->length) s->position = s->length - 1;
        } else if (strcmp(k, "mutate") == 0) {
            mutate_pattern(s, t->scale);
        } else if (strcmp(k, "density") == 0) {
            float v = parse_float(val, 0.7f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->density = v;
        } else if (strcmp(k, "accent") == 0) {
            float v = parse_float(val, 0.4f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->accent = v;
        } else if (strcmp(k, "slide") == 0) {
            float v = parse_float(val, 0.25f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->slide = v;
        } else if (strcmp(k, "octaves") == 0) {
            int v = parse_int(val, 2);
            if (v < 1) v = 1;
            if (v > 3) v = 3;
            s->octave_range = v;
        } else if (strcmp(k, "length") == 0) {
            /* Declared as a float chain_param (min 2, max 32) so the knob rides
             * the range-normalised curve instead of one-step-per-detent -- the
             * wire value arrives like "16.000", so round rather than truncate. */
            int v = (int)(parse_float(val, 16.0f) + 0.5f);
            if (v < MIN_LENGTH) v = MIN_LENGTH;
            if (v > MAX_STEPS) v = MAX_STEPS;
            s->length = v;
            if (s->position >= v) s->position = v - 1;
            if (s->offset >= v) s->offset = v - 1;  /* keep Offset < the new length */
        } else if (strcmp(k, "gate") == 0) {
            float v = parse_float(val, 0.5f);
            if (v < 0.05f) v = 0.05f;
            if (v > 1.0f) v = 1.0f;
            s->gate = v;
        } else if (strcmp(k, "algo") == 0) {
            int v = parse_int(val, 1);
            if (v < 1) v = 1;
            if (v > 16) v = 16;
            s->algo = v;
        } else if (strcmp(k, "tune") == 0) {
            int v = parse_int(val, 0);
            if (v < -ACID_MAX_TUNE) v = -ACID_MAX_TUNE;
            if (v >  ACID_MAX_TUNE) v =  ACID_MAX_TUNE;
            s->tune = v;
        } else if (strcmp(k, "offset") == 0) {
            /* Declared as a float chain_param (like Length A/B and Swing) so
             * the knob rides the range-normalised curve instead of stepping
             * one-per-detent across the 0..31 span -- the wire value arrives
             * like "7.000", so round rather than truncate. */
            int v = (int)(parse_float(val, 0.0f) + 0.5f);
            if (v < 0) v = 0;
            if (v >= s->length) v = s->length - 1;
            s->offset = v;
        } else if (strcmp(k, "dir") == 0) {
            int v = parse_int(val, 0);
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            if (v == 2 && s->dir != 2) s->pendulum_fwd = 1; /* enter Pendulum travelling forward */
            s->dir = v;
        }
        return;
    }

    if (strcmp(key, "root") == 0) {
        int v = parse_int(val, 9) % 12; if (v < 0) v += 12; t->root = v;
    } else if (strcmp(key, "scale") == 0) {
        int v = parse_int(val, 0); if (v < 0 || v >= NUM_SCALES) v = 0; t->scale = v;
    } else if (strcmp(key, "blend") == 0) {
        int v = parse_int(val, 0);
        if (v < -63) v = -63;
        if (v > 64) v = 64;
        t->blend = v;
    } else if (strcmp(key, "blend_mode") == 0) {
        int v = parse_int(val, 0); if (v < 0 || v >= NUM_BLEND_MODES) v = 0;
        if (v == BM_CHAIN && t->blend_mode != BM_CHAIN) t->chain_fresh = 1;
        t->blend_mode = v;
    } else if (strcmp(key, "reset_bars") == 0) {
        int v = parse_int(val, 4); if (v < 0 || v > 4) v = 4;
        t->reset_bars_idx = v;
        t->bar_step_count = 0;
    } else if (strcmp(key, "swing") == 0) {
        /* Declared as a float chain_param for the range-normalised knob
         * curve (see the struct comment) -- the wire value arrives like
         * "62.000", so round rather than truncate, same as Length A/B. */
        int v = (int)(parse_float(val, 50.0f) + 0.5f);
        if (v < 50) v = 50;
        if (v > 75) v = 75;
        t->swing_pct = v;
    } else if (strcmp(key, "jitter") == 0) {
        float v = parse_float(val, 0.0f);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        t->jitter = v;
    } else if (strcmp(key, "auto_gen") == 0) {
        int v = parse_int(val, 0);
        if (v < 0) v = 0;
        if (v > 6) v = 6;
        t->auto_gen_idx = v;
        t->auto_gen_step_count = 0;
    }
}


/* ---- Preset/autosave state ------------------------------------------------
 * Full, self-contained text blob: shared knobs, then per sequencer its knobs,
 * PRNG state and the literal pattern (hex), so recall reproduces the exact
 * sequence rather than regenerating one. Flat "v1;k=v;..." -- no nesting. */
static int is_state_key(const char *key) {
    size_t n = strlen(key);
    return strcmp(key, "state") == 0 || (n > 6 && strcmp(key + n - 6, ":state") == 0);
}

static int hex_put(char *o, size_t cap, const uint8_t *a, int n) {
    static const char H[] = "0123456789abcdef";
    if ((size_t)(2 * n + 1) > cap) return -1;
    for (int i = 0; i < n; i++) { o[2*i] = H[a[i] >> 4]; o[2*i+1] = H[a[i] & 15]; }
    o[2*n] = 0;
    return 2 * n;
}

static int hex_get(const char *h, uint8_t *a, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v;
        char b[3] = { h[2*i], h[2*i] ? h[2*i+1] : 0, 0 };
        if (!b[0] || !b[1] || sscanf(b, "%2x", &v) != 1) return -1;
        a[i] = (uint8_t)v;
    }
    return 0;
}

static int state_encode(const acid_inst_t *t, char *buf, int cap) {
    int n = snprintf(buf, cap, "v1;root=%d;scale=%d;blend=%d;bm=%d;rb=%d;sw=%d;jit=%.3f;ag=%d",
                     t->root, t->scale, t->blend, t->blend_mode, t->reset_bars_idx, t->swing_pct,
                     t->jitter, t->auto_gen_idx);
    for (int i = 0; i < NUM_SEQS && n > 0 && n < cap; i++) {
        const acid_seq_t *s = &t->seq[i];
        n += snprintf(buf + n, cap - n,
                      ";%c=%.3f,%.3f,%.3f,%d,%d,%.3f,%d,%d,%d,%d,%u,%u,",
                      'a' + i, s->density, s->accent, s->slide, s->octave_range,
                      s->length, s->gate, s->algo, s->tune, s->offset, s->dir,
                      (unsigned)s->rng, (unsigned)s->seed);
        if (n >= cap) return -1;
        int h;
        if ((h = hex_put(buf + n, cap - n, s->steps, MAX_STEPS)) < 0) return -1;
        n += h;
        buf[n++] = ',';
        if (n >= cap) return -1;
        if ((h = hex_put(buf + n, cap - n, s->degrees, MAX_STEPS)) < 0) return -1;
        n += h;
        buf[n++] = ',';
        if (n >= cap) return -1;
        if ((h = hex_put(buf + n, cap - n, s->octaves, MAX_STEPS)) < 0) return -1;
        n += h;
    }
    return (n > 0 && n < cap) ? n : -1;
}

static void state_decode(acid_inst_t *t, const char *val) {
    if (!val || strncmp(val, "v1;", 3) != 0) return;
    const char *p = val + 3;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char tok[512];
        if (len < sizeof(tok)) {
            memcpy(tok, p, len); tok[len] = 0;
            if (tok[0] >= 'a' && tok[0] <= 'b' && tok[1] == '=') {
                acid_seq_t *s = &t->seq[tok[0] - 'a'];
                float d, a, sl, g, fl = 0; int oc, ln, al, tu, of, dr; unsigned rn, sd; int used = 0;
                if (sscanf(tok + 2, "%f,%f,%f,%d,%d,%f,%d,%d,%d,%d,%u,%u,%n",
                           &d, &a, &sl, &oc, &ln, &g, &al, &tu, &of, &dr, &rn, &sd, &used) >= 12 && used) {
                    (void)fl;
                    uint8_t st[MAX_STEPS], dg[MAX_STEPS], ov[MAX_STEPS];
                    const char *h = tok + 2 + used;
                    size_t hl = 2 * MAX_STEPS;
                    if (strlen(h) >= 3 * hl + 2 && h[hl] == ',' && h[2*hl+1] == ',' &&
                        hex_get(h, st, MAX_STEPS) == 0 &&
                        hex_get(h + hl + 1, dg, MAX_STEPS) == 0 &&
                        hex_get(h + 2*hl + 2, ov, MAX_STEPS) == 0) {
                        memcpy(s->steps, st, MAX_STEPS);
                        memcpy(s->degrees, dg, MAX_STEPS);
                        memcpy(s->octaves, ov, MAX_STEPS);
                        s->density = d < 0 ? 0 : d > 1 ? 1 : d;
                        s->accent = a < 0 ? 0 : a > 1 ? 1 : a;
                        s->slide = sl < 0 ? 0 : sl > 1 ? 1 : sl;
                        s->octave_range = oc < 1 ? 1 : oc > 3 ? 3 : oc;
                        s->length = ln < MIN_LENGTH ? MIN_LENGTH : ln > MAX_STEPS ? MAX_STEPS : ln;
                        s->gate = g < 0.05f ? 0.05f : g > 1 ? 1 : g;
                        s->algo = al < 1 ? 1 : al > 16 ? 16 : al;
                        s->tune = tu < -ACID_MAX_TUNE ? -ACID_MAX_TUNE : tu > ACID_MAX_TUNE ? ACID_MAX_TUNE : tu;
                        s->offset = of < 0 ? 0 : of >= s->length ? s->length - 1 : of;
                        s->dir = dr < 0 ? 0 : dr > 2 ? 2 : dr;
                        s->rng = rn ? rn : 1u; s->seed = sd;
                        if (s->position >= s->length) s->position = s->length - 1;
                    }
                }
            } else {
                char *k = tok; char *eq = strchr(tok, '=');
                if (eq) {
                    *eq = 0;
                    if (!strcmp(k, "root")) acid_set_param(t, "root", eq + 1);
                    else if (!strcmp(k, "scale")) acid_set_param(t, "scale", eq + 1);
                    else if (!strcmp(k, "blend")) acid_set_param(t, "blend", eq + 1);
                    else if (!strcmp(k, "bm")) acid_set_param(t, "blend_mode", eq + 1);
                    else if (!strcmp(k, "rb")) acid_set_param(t, "reset_bars", eq + 1);
                    else if (!strcmp(k, "sw")) acid_set_param(t, "swing", eq + 1);
                    else if (!strcmp(k, "jit")) acid_set_param(t, "jitter", eq + 1);
                    else if (!strcmp(k, "ag")) acid_set_param(t, "auto_gen", eq + 1);
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

static int acid_get_param(void *instance, const char *key, char *buf, int buf_len) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key || !buf || buf_len < 2) return -1;

    if (strcmp(key, "state") == 0) return state_encode(t, buf, buf_len);

    int seq_idx = -1;
    const char *k = key;
    if (key[0] == 'a' && key[1] == '_') { seq_idx = SEQ_A; k = key + 2; }
    else if (key[0] == 'b' && key[1] == '_') { seq_idx = SEQ_B; k = key + 2; }

    int n = -1;

    if (seq_idx >= 0) {
        acid_seq_t *s = &t->seq[seq_idx];
        if (strcmp(k, "density") == 0) n = snprintf(buf, buf_len, "%.3f", s->density);
        else if (strcmp(k, "accent") == 0) n = snprintf(buf, buf_len, "%.3f", s->accent);
        else if (strcmp(k, "slide") == 0) n = snprintf(buf, buf_len, "%.3f", s->slide);
        else if (strcmp(k, "octaves") == 0) n = snprintf(buf, buf_len, "%d", s->octave_range);
        else if (strcmp(k, "length") == 0) n = snprintf(buf, buf_len, "%d", s->length);
        else if (strcmp(k, "gate") == 0) n = snprintf(buf, buf_len, "%.3f", s->gate);
        else if (strcmp(k, "algo") == 0) n = snprintf(buf, buf_len, "%d", s->algo);
        else if (strcmp(k, "tune") == 0) n = snprintf(buf, buf_len, "%d", s->tune);
        else if (strcmp(k, "offset") == 0) n = snprintf(buf, buf_len, "%d", s->offset);
        else if (strcmp(k, "dir") == 0) n = snprintf(buf, buf_len, "%d", s->dir);
        else if (strcmp(k, "generate") == 0 || strcmp(k, "mutate") == 0) n = snprintf(buf, buf_len, "off");
        else return -1;
        if (n < 0) return -1;
        if (n >= buf_len) n = buf_len - 1;
        return n;
    }

    if (strcmp(key, "root") == 0) n = snprintf(buf, buf_len, "%d", t->root);
    else if (strcmp(key, "scale") == 0) n = snprintf(buf, buf_len, "%d", t->scale);
    else if (strcmp(key, "blend") == 0) n = snprintf(buf, buf_len, "%d", t->blend);
    else if (strcmp(key, "blend_mode") == 0) n = snprintf(buf, buf_len, "%d", t->blend_mode);
    else if (strcmp(key, "reset_bars") == 0) n = snprintf(buf, buf_len, "%d", t->reset_bars_idx);
    else if (strcmp(key, "swing") == 0) n = snprintf(buf, buf_len, "%d", t->swing_pct);
    else if (strcmp(key, "jitter") == 0) n = snprintf(buf, buf_len, "%.3f", t->jitter);
    else if (strcmp(key, "auto_gen") == 0) n = snprintf(buf, buf_len, "%d", t->auto_gen_idx);
    else if (strcmp(key, "chain_params") == 0) {
        /* Not actually consulted for midi_fx loading -- chain_midi.c reads
         * chain_params straight out of module.json on disk (parse_chain_params),
         * falling back to it precisely because our ui_hierarchy params carry
         * only key/short_name, no inline type info. Kept here anyway for
         * parity with the rest of the ecosystem (arp.c does the same) and
         * any other caller that does ask the loaded plugin directly. Must be
         * kept in sync with module.json's chain_params by hand. */
        static const char params[] =
            "["
            "{\"key\":\"a_generate\",\"name\":\"Generate A\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_mutate\",\"name\":\"Mutate A\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_density\",\"name\":\"Density A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7,\"unit\":\"%\"},"
            "{\"key\":\"a_accent\",\"name\":\"Accent A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4,\"unit\":\"%\"},"
            "{\"key\":\"a_slide\",\"name\":\"Slide A\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25,\"unit\":\"%\"},"
            "{\"key\":\"a_octaves\",\"name\":\"Octaves A\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"a_length\",\"name\":\"Length A\",\"type\":\"float\",\"min\":2,\"max\":32,\"step\":1,\"default\":16,\"display_format\":\".0f\"},"
            "{\"key\":\"a_gate\",\"name\":\"Gate A\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5,\"unit\":\"%\"},"
            "{\"key\":\"b_generate\",\"name\":\"Generate B\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_mutate\",\"name\":\"Mutate B\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_density\",\"name\":\"Density B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7,\"unit\":\"%\"},"
            "{\"key\":\"b_accent\",\"name\":\"Accent B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4,\"unit\":\"%\"},"
            "{\"key\":\"b_slide\",\"name\":\"Slide B\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25,\"unit\":\"%\"},"
            "{\"key\":\"b_octaves\",\"name\":\"Octaves B\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"b_length\",\"name\":\"Length B\",\"type\":\"float\",\"min\":2,\"max\":32,\"step\":1,\"default\":16,\"display_format\":\".0f\"},"
            "{\"key\":\"b_gate\",\"name\":\"Gate B\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5,\"unit\":\"%\"},"
            "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\",\"options\":[\"Minor\",\"Phrygian\",\"HarmMinor\",\"MinPent\",\"Dorian\",\"Major\",\"PhrygDom\",\"Locrian\",\"WholeTone\",\"HungMinor\",\"MinBlues\",\"Chromatic\"],\"default\":0},"
            "{\"key\":\"root\",\"name\":\"Root\",\"type\":\"enum\",\"options\":[\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"],\"default\":9},"
            "{\"key\":\"b_tune\",\"name\":\"Tune B\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1,\"default\":0},"
            "{\"key\":\"blend_mode\",\"name\":\"Blend Mode\",\"type\":\"enum\",\"options\":[\"Morph\",\"Split\",\"Fill\",\"XOR\",\"Lock\",\"Chain\"],\"default\":0},"
            "{\"key\":\"blend\",\"name\":\"Blend\",\"type\":\"int\",\"min\":-63,\"max\":64,\"step\":1,\"default\":-63},"
            "{\"key\":\"a_algo\",\"name\":\"Algo A\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"b_algo\",\"name\":\"Algo B\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"reset_bars\",\"name\":\"Reset Both\",\"type\":\"enum\",\"options\":[\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\",\"Off\"],\"default\":4},"
            "{\"key\":\"swing\",\"name\":\"Swing\",\"type\":\"float\",\"min\":50,\"max\":75,\"step\":1,\"default\":50,\"display_format\":\".0f\"},"
            "{\"key\":\"a_offset\",\"name\":\"Offset A\",\"type\":\"float\",\"min\":0,\"max\":31,\"step\":1,\"default\":0,\"display_format\":\".0f\"},"
            "{\"key\":\"b_offset\",\"name\":\"Offset B\",\"type\":\"float\",\"min\":0,\"max\":31,\"step\":1,\"default\":0,\"display_format\":\".0f\"},"
            "{\"key\":\"a_dir\",\"name\":\"Direction A\",\"type\":\"enum\",\"options\":[\"Fwd\",\"Rev\",\"Pendulum\"],\"default\":0},"
            "{\"key\":\"b_dir\",\"name\":\"Direction B\",\"type\":\"enum\",\"options\":[\"Fwd\",\"Rev\",\"Pendulum\"],\"default\":0},"
            "{\"key\":\"jitter\",\"name\":\"Jitter\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.0,\"unit\":\"%\"},"
            "{\"key\":\"auto_gen\",\"name\":\"Auto Gen\",\"type\":\"enum\",\"options\":[\"Off\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\",\"16 bars\",\"32 bars\"],\"default\":0}"
            "]";
        n = snprintf(buf, buf_len, "%s", params);
    }
    else return -1;

    if (n < 0) return -1;
    if (n >= buf_len) n = buf_len - 1;
    return n;
}

static midi_fx_api_v1_t acid_api_v1 = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = acid_create_instance,
    .destroy_instance = acid_destroy_instance,
    .process_midi     = acid_process_midi,
    .tick             = acid_tick,
    .set_param        = acid_set_param,
    .get_param        = acid_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &acid_api_v1;
}
