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
 * (-63..64) using the same velocity-crossfade shape as sd88me's own Maze Lite
 * "Trig Mix": -63 = A only @127, 0 = both @100, +64 = B only @127.
 *
 * No banks, no undo, no persistence in this version -- Generate/Mutate only.
 * An incoming note-on sets the shared root live (hybrid trigger model);
 * sequencing itself keeps running regardless of note input.
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

typedef enum { STEP_REST = 0, STEP_NOTE = 1, STEP_ACCENT = 2, STEP_SLIDE = 3 } step_kind_t;

/* Scale degrees in semitones from root -- ported verbatim from tb3po. */
typedef struct { const char *name; int degrees[12]; int len; } scale_t;
static const scale_t SCALES[] = {
    { "Minor",     {0, 2, 3, 5, 7, 8, 10},         7 },
    { "Phrygian",  {0, 1, 3, 5, 7, 8, 10},         7 },
    { "HarmMinor", {0, 2, 3, 5, 7, 8, 11},         7 },
    { "MinPent",   {0, 3, 5, 7, 10, 0, 0},         5 },
    { "Dorian",    {0, 2, 3, 5, 7, 9, 10},         7 },
    { "Major",     {0, 2, 4, 5, 7, 9, 11},         7 }
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

    /* Playback */
    int last_note_on;   /* -1 = none */
    int portamento_on;
    long gate_samples_remaining;
} acid_seq_t;

typedef struct {
    acid_seq_t seq[NUM_SEQS];
    int seq_b_enabled;

    int root;             /* 0-11 */
    int scale;            /* index into SCALES */
    int blend;             /* -63..64, Trig-Mix-style crossfade */
    int reset_bars_idx;   /* 0..3 -> {1,2,4,8} bars, 4 = Off */

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

static int note_for_step(const acid_seq_t *s, int scale_idx, int root, int step_idx) {
    const scale_t *sc = &SCALES[scale_idx];
    /* Base in the C1 octave, matching tb3po -- keeps the emitted range well
     * clear of Move's pad-LED note range. */
    int base = 24 + root;
    int note = base + sc->degrees[s->degrees[step_idx]] + 12 * s->octaves[step_idx];
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

static int next_position(const acid_seq_t *s) {
    return (s->position + 1) % s->length;
}

/* Blend ("Trig Mix") scales each sequencer's OWN velocity by a 0-100%
 * multiplier -- it never substitutes in an absolute target velocity. That
 * keeps each sequence's internal accent/normal ratio (118 vs 72, see
 * emit_step_for_seq) intact; only the relative balance between A and B
 * moves. Full CCW (-63) = A at 100%, B at 0%. Centre (0) = both at 100%.
 * Full CW (+64) = A at 0%, B at 100%. Each side only ever pulls down the
 * OPPOSITE sequence -- the "home" side for a given half stays at 100%
 * throughout that half, ramping linearly from 100% to 0% only as the knob
 * crosses from centre to its far extreme. */
static void compute_blend_velocities(int blend, int *vel_a, int *vel_b) {
    float va, vb;
    if (blend <= 0) {
        float t = (float)(-blend) / 63.0f; /* 0 at centre .. 1 at -63 */
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        va = 127.0f;              /* A stays full on this half */
        vb = 127.0f * (1.0f - t); /* B ramps 100% -> 0% */
    } else {
        float t = (float)blend / 64.0f; /* 0 at centre .. 1 at +64 */
        if (t > 1.0f) t = 1.0f;
        va = 127.0f * (1.0f - t); /* A ramps 100% -> 0% */
        vb = 127.0f;               /* B stays full on this half */
    }
    *vel_a = (int)(va + 0.5f);
    *vel_b = (int)(vb + 0.5f);
    if (*vel_a < 0) *vel_a = 0;
    if (*vel_a > 127) *vel_a = 127;
    if (*vel_b < 0) *vel_b = 0;
    if (*vel_b > 127) *vel_b = 127;
}

static void recompute_step_length(acid_inst_t *t, int sample_rate) {
    double sixteenths_per_sec = (double)t->bpm / 60.0 * 4.0;
    if (sixteenths_per_sec <= 0) sixteenths_per_sec = 8.0;
    t->samples_per_step = (double)sample_rate / sixteenths_per_sec;
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
 * this sequencer's Blend-derived level; the step's own accent/normal
 * velocity is scaled by it, so Blend acts as an overall mix level per
 * generator rather than overriding accent dynamics. vel_scale == 0 mutes
 * the sequencer outright for this step (true "only" semantics at the
 * Blend extremes, not just quiet). */
static int emit_step_for_seq(acid_inst_t *t, int seq_idx, int prev_pos, int vel_scale,
                              uint8_t out_msgs[][3], int out_lens[], int max_out) {
    acid_seq_t *s = &t->seq[seq_idx];
    int count = 0;
    uint8_t kind = s->steps[s->position];

    if (kind == STEP_REST || vel_scale <= 0) {
        if (count < max_out) count += kill_seq_note(t, seq_idx, &out_msgs[count], &out_lens[count], max_out - count);
        s->gate_samples_remaining = 0;
        return count;
    }

    int note = note_for_step(s, t->scale, t->root, s->position);
    int is_accent = (kind == STEP_ACCENT);
    int base_vel = is_accent ? 118 : 72;
    int vel = (base_vel * vel_scale) / 127;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;

    int was_slide = (prev_pos >= 0 && s->steps[prev_pos] == STEP_SLIDE);

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

    int vel_a, vel_b;
    compute_blend_velocities(t->blend, &vel_a, &vel_b);
    if (!t->seq_b_enabled) vel_b = 0;

    for (int i = 0; i < NUM_SEQS; i++) {
        if (i == SEQ_B && !t->seq_b_enabled) continue;
        acid_seq_t *s = &t->seq[i];
        int prev = s->position;
        s->position = forced_reset ? 0 : next_position(s);
        int vel_scale = (i == SEQ_A) ? vel_a : vel_b;
        if (count < max_out) {
            count += emit_step_for_seq(t, i, prev, vel_scale, &out_msgs[count], &out_lens[count], max_out - count);
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
        s->last_note_on = -1;
    }
    t->seq_b_enabled = 1;
    t->root = 9; /* A, matches tb3po's default */
    t->scale = 0;
    t->blend = 0;
    t->reset_bars_idx = 4; /* Off */
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
            t->clock_pulses = (t->clock_pulses + 1) % 6; /* 6 pulses per 16th */
            if (t->clock_pulses == 0) return advance_all(t, out_msgs, out_lens, max_out);
        }
        return 0;
    }
    if (status == 0xFA) { /* Start */
        if (t->follow_transport) {
            t->running = 1;
            for (int i = 0; i < NUM_SEQS; i++) t->seq[i].position = t->seq[i].length - 1;
            t->clock_pulses = 5; /* first 0xF8 bumps to 0 -> fires step 0 on the downbeat */
            t->sample_accum = 0;
            t->bar_step_count = 0;
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

    /* Hybrid trigger model: a held/incoming note sets the shared root live;
     * sequencing itself is not gated by it. Note-on/off are both swallowed
     * (Acid generates its own stream, it does not pass notes through). */
    if (type == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        t->root = in_msg[1] % 12;
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

    /* A sequencer disabled mid-note needs its note-off cleaned up somewhere;
     * this is the one place that runs unconditionally every block. */
    if (!t->seq_b_enabled && t->seq[SEQ_B].last_note_on >= 0 && count < max_out) {
        count += kill_seq_note(t, SEQ_B, &out_msgs[count], &out_lens[count], max_out - count);
    }

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
                for (int i = 0; i < NUM_SEQS; i++) t->seq[i].position = t->seq[i].length - 1;
                t->clock_pulses = 5;
                t->sample_accum = 0;
                t->bar_step_count = 0;
            } else if (cs == MOVE_CLOCK_STATUS_STOPPED && t->running) {
                t->running = 0;
                if (count < max_out) count += kill_all_notes(t, &out_msgs[count], &out_lens[count], max_out - count);
            }
        }
    }

    if (!t->running) return count;

    if (!t->pulse_sync_active) {
        t->sample_accum += (double)frames;
        while (t->sample_accum >= t->samples_per_step && count < max_out - 4) {
            t->sample_accum -= t->samples_per_step;
            count += advance_all(t, &out_msgs[count], &out_lens[count], max_out - count);
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
                if (s->steps[s->position] != STEP_SLIDE && count < max_out) {
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

static void acid_set_param(void *instance, const char *key, const char *val) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key) return;

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
            int v = parse_int(val, 16);
            if (v < MIN_LENGTH) v = MIN_LENGTH;
            if (v > MAX_STEPS) v = MAX_STEPS;
            s->length = v;
            if (s->position >= v) s->position = v - 1;
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
        }
        return;
    }

    if (strcmp(key, "root") == 0) {
        int v = parse_int(val, 9) % 12; if (v < 0) v += 12; t->root = v;
    } else if (strcmp(key, "scale") == 0) {
        int v = parse_int(val, 0); if (v < 0 || v >= NUM_SCALES) v = 0; t->scale = v;
    } else if (strcmp(key, "seq_b_enable") == 0) {
        t->seq_b_enabled = parse_int(val, 1) ? 1 : 0;
    } else if (strcmp(key, "blend") == 0) {
        int v = parse_int(val, 0);
        if (v < -63) v = -63;
        if (v > 64) v = 64;
        t->blend = v;
    } else if (strcmp(key, "reset_bars") == 0) {
        int v = parse_int(val, 4); if (v < 0 || v > 4) v = 4;
        t->reset_bars_idx = v;
        t->bar_step_count = 0;
    }
}

static int acid_get_param(void *instance, const char *key, char *buf, int buf_len) {
    acid_inst_t *t = (acid_inst_t *)instance;
    if (!t || !key || !buf || buf_len < 2) return -1;

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
        else if (strcmp(k, "generate") == 0 || strcmp(k, "mutate") == 0) n = snprintf(buf, buf_len, "off");
        else return -1;
        if (n < 0) return -1;
        if (n >= buf_len) n = buf_len - 1;
        return n;
    }

    if (strcmp(key, "root") == 0) n = snprintf(buf, buf_len, "%d", t->root);
    else if (strcmp(key, "scale") == 0) n = snprintf(buf, buf_len, "%d", t->scale);
    else if (strcmp(key, "seq_b_enable") == 0) n = snprintf(buf, buf_len, "%d", t->seq_b_enabled);
    else if (strcmp(key, "blend") == 0) n = snprintf(buf, buf_len, "%d", t->blend);
    else if (strcmp(key, "reset_bars") == 0) n = snprintf(buf, buf_len, "%d", t->reset_bars_idx);
    else if (strcmp(key, "chain_params") == 0) {
        /* Not actually consulted for midi_fx loading -- chain_midi.c reads
         * chain_params straight out of module.json on disk (parse_chain_params),
         * falling back to it precisely because our ui_hierarchy uses bare
         * string param refs with no inline type info. Kept here anyway for
         * parity with the rest of the ecosystem (arp.c does the same) and
         * any other caller that does ask the loaded plugin directly. Must be
         * kept in sync with module.json's chain_params by hand. */
        static const char params[] =
            "["
            "{\"key\":\"a_generate\",\"name\":\"A Generate\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_mutate\",\"name\":\"A Mutate\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"a_density\",\"name\":\"A Density\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7},"
            "{\"key\":\"a_accent\",\"name\":\"A Accent\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4},"
            "{\"key\":\"a_slide\",\"name\":\"A Slide\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25},"
            "{\"key\":\"a_octaves\",\"name\":\"A Octaves\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"a_length\",\"name\":\"A Length\",\"type\":\"int\",\"min\":2,\"max\":32,\"step\":1,\"default\":16},"
            "{\"key\":\"a_gate\",\"name\":\"A Gate\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"b_generate\",\"name\":\"B Generate\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_mutate\",\"name\":\"B Mutate\",\"type\":\"enum\",\"options\":[\"off\",\"go\"],\"access\":\"write\"},"
            "{\"key\":\"b_density\",\"name\":\"B Density\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.7},"
            "{\"key\":\"b_accent\",\"name\":\"B Accent\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.4},"
            "{\"key\":\"b_slide\",\"name\":\"B Slide\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"step\":0.01,\"default\":0.25},"
            "{\"key\":\"b_octaves\",\"name\":\"B Octaves\",\"type\":\"int\",\"min\":1,\"max\":3,\"step\":1,\"default\":2},"
            "{\"key\":\"b_length\",\"name\":\"B Length\",\"type\":\"int\",\"min\":2,\"max\":32,\"step\":1,\"default\":16},"
            "{\"key\":\"b_gate\",\"name\":\"B Gate\",\"type\":\"float\",\"min\":0.05,\"max\":1.0,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"root\",\"name\":\"Root\",\"type\":\"enum\",\"options\":[\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"],\"default\":9},"
            "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\",\"options\":[\"Minor\",\"Phrygian\",\"HarmMinor\",\"MinPent\",\"Dorian\",\"Major\"],\"default\":0},"
            "{\"key\":\"seq_b_enable\",\"name\":\"Seq B\",\"type\":\"enum\",\"options\":[\"off\",\"on\"],\"default\":1},"
            "{\"key\":\"a_algo\",\"name\":\"A Algo\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"b_algo\",\"name\":\"B Algo\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1,\"default\":1},"
            "{\"key\":\"blend\",\"name\":\"Blend\",\"type\":\"int\",\"min\":-63,\"max\":64,\"step\":1,\"default\":0},"
            "{\"key\":\"reset_bars\",\"name\":\"Reset Both\",\"type\":\"enum\",\"options\":[\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\",\"Off\"],\"default\":4}"
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
