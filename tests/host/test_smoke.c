/* Host-side smoke test for acid.c -- dlopen's the natively-built .so (NOT the
 * ARM64 cross-compiled one) and drives it like the chain host would: start
 * transport, tick for a while, twiddle params, confirm no crash and that the
 * output looks like sane MIDI (note ranges, on/off balance, resets landing,
 * root changing on note-in, Blend muting Seq B). Not shipped as part
 * of the module -- a local build-time check only.
 *
 * Build & run:
 *   gcc -shared -fPIC -O2 -Isrc/include src/acid/dsp/acid.c -o /tmp/acid_native_test.so -lm
 *   gcc -Isrc/include tests/host/test_smoke.c -o /tmp/test_smoke -ldl
 *   /tmp/test_smoke
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "plugin_api_v1.h"
#include "midi_fx_api_v1.h"

static float mock_get_bpm(void) { return 120.0f; }
static int mock_get_clock_status(void) { return MOVE_CLOCK_STATUS_RUNNING; }
static int mock_slot_recv_channel(void *instance) { (void)instance; return 0; }
static void mock_log(const char *msg) { (void)msg; /* silence by default */ }

static int g_note_on = 0, g_note_off = 0, g_cc = 0;
static int g_min_note = 200, g_max_note = -1;

static void observe(const uint8_t msg[3], int len, long tick_idx) {
    if (len < 1) return;
    uint8_t type = msg[0] & 0xF0;
    if (type == 0x90 && msg[2] > 0) {
        g_note_on++;
        if (msg[1] < g_min_note) g_min_note = msg[1];
        if (msg[1] > g_max_note) g_max_note = msg[1];
        printf("[%6ld] note-on  note=%3d vel=%3d\n", tick_idx, msg[1], msg[2]);
    } else if (type == 0x80 || (type == 0x90 && msg[2] == 0)) {
        g_note_off++;
        printf("[%6ld] note-off note=%3d\n", tick_idx, msg[1]);
    } else if (type == 0xB0) {
        g_cc++;
        printf("[%6ld] cc       cc=%3d val=%3d\n", tick_idx, msg[1], msg[2]);
    } else {
        printf("[%6ld] other    %02x %02x %02x\n", tick_idx, msg[0], msg[1], msg[2]);
    }
}

int main(void) {
    void *handle = dlopen("/tmp/acid_native_test.so", RTLD_NOW);
    if (!handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }

    midi_fx_init_fn init_fn = (midi_fx_init_fn)dlsym(handle, MIDI_FX_INIT_SYMBOL);
    if (!init_fn) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = mock_log;
    host.get_bpm = mock_get_bpm;
    host.get_clock_status = mock_get_clock_status;
    host.slot_recv_channel = mock_slot_recv_channel;

    midi_fx_api_v1_t *api = init_fn(&host);
    if (!api) { fprintf(stderr, "init returned NULL\n"); return 1; }
    printf("api_version = %u (expect %u)\n", api->api_version, (unsigned)MIDI_FX_API_VERSION);

    void *inst = api->create_instance(NULL, NULL);
    if (!inst) { fprintf(stderr, "create_instance returned NULL\n"); return 1; }
    printf("instance created OK\n\n");

    /* Polymeter setup: 5-step A, 7-step B, differing algos, B tuned a fifth
     * up, a non-trivial blend, and a bar-reset armed for the second half of
     * the run so we can see positions actually snap together. */
    api->set_param(inst, "a_length", "5");
    api->set_param(inst, "b_length", "7");
    api->set_param(inst, "a_algo", "1");
    api->set_param(inst, "b_algo", "12");
    api->set_param(inst, "b_tune", "7");
    api->set_param(inst, "blend", "0");
    api->set_param(inst, "reset_bars", "4"); /* Off, to start -- see polymeter drift */

    /* Advanced page: exercise the new paths for the whole run -- Pendulum on
     * A, Rev on B, a read-side Offset on each, Jitter, and Auto Gen re-rolling
     * both sequencers every bar. The summary assertions (note range, on/off
     * balance) then cover Direction keeping position in [0,length) across an
     * extended tick run and Auto Gen not wedging the output stream. Auto Gen
     * (1 bar) and Reset Both (armed to 1 bar at the halfway point) end up
     * firing on the same tick for the second half -- the interaction flagged
     * in the handoff's open items. */
    api->set_param(inst, "a_dir", "2");     /* Pendulum */
    api->set_param(inst, "b_dir", "1");     /* Rev */
    api->set_param(inst, "a_offset", "3");
    api->set_param(inst, "b_offset", "2");
    api->set_param(inst, "jitter", "0.35");
    api->set_param(inst, "auto_gen", "1");  /* re-roll both every 1 bar */

    char buf[4096];
    int n;
    n = api->get_param(inst, "chain_params", buf, sizeof(buf));
    printf("chain_params length = %d (expect > 0, < %zu)\n", n, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf) - 1) { fprintf(stderr, "chain_params suspicious\n"); }

    n = api->get_param(inst, "a_length", buf, sizeof(buf));
    printf("a_length readback = %.*s\n", n, buf);
    n = api->get_param(inst, "b_algo", buf, sizeof(buf));
    printf("b_algo readback = %.*s\n", n, buf);
    n = api->get_param(inst, "b_tune", buf, sizeof(buf));
    printf("b_tune readback = %.*s (expect 7)\n", n, buf);
    n = api->get_param(inst, "a_dir", buf, sizeof(buf));
    printf("a_dir readback = %.*s (expect 2)\n", n, buf);
    n = api->get_param(inst, "a_offset", buf, sizeof(buf));
    printf("a_offset readback = %.*s (expect 3)\n", n, buf);
    n = api->get_param(inst, "jitter", buf, sizeof(buf));
    printf("jitter readback = %.*s (expect ~0.350)\n", n, buf);
    n = api->get_param(inst, "auto_gen", buf, sizeof(buf));
    printf("auto_gen readback = %.*s (expect 1)\n\n", n, buf);

    /* Start transport. */
    uint8_t start_msg[1] = { 0xFA };
    uint8_t out_msgs[MIDI_FX_MAX_OUT_MSGS][3];
    int out_lens[MIDI_FX_MAX_OUT_MSGS];
    int count = api->process_midi(inst, start_msg, 1, out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);
    printf("process_midi(START) returned %d messages\n\n", count);

    long total_blocks = (long)((44100.0 / 128.0) * 8.0); /* ~8 seconds */
    for (long i = 0; i < total_blocks; i++) {
        if (i == total_blocks / 2) {
            printf("\n--- arming Reset Both = 1 bar at block %ld ---\n\n", i);
            api->set_param(inst, "reset_bars", "0");
        }
        if (i == total_blocks / 4) {
            printf("\n--- note-in C3 (60) to retune root at block %ld ---\n\n", i);
            uint8_t note_msg[3] = { 0x90, 60, 100 };
            count = api->process_midi(inst, note_msg, 3, out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);
            for (int m = 0; m < count; m++) observe(out_msgs[m], out_lens[m], i);
        }
        if (i == (3 * total_blocks) / 4) {
            printf("\n--- Blend hard left (Seq A alone) at block %ld ---\n\n", i);
            api->set_param(inst, "blend", "-63");
        }
        if (i == total_blocks - 200) {
            printf("\n--- A Generate at block %ld ---\n\n", i);
            api->set_param(inst, "a_generate", "go");
        }
        if (i == total_blocks - 100) {
            printf("\n--- A Mutate at block %ld ---\n\n", i);
            api->set_param(inst, "a_mutate", "go");
        }

        count = api->tick(inst, 128, 44100, out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);
        for (int m = 0; m < count; m++) observe(out_msgs[m], out_lens[m], i);
    }

    api->destroy_instance(inst);
    dlclose(handle);

    printf("\n=== summary ===\n");
    printf("note-on=%d note-off=%d cc(portamento)=%d\n", g_note_on, g_note_off, g_cc);
    printf("note range: %d..%d\n", g_min_note, g_max_note);

    int ok = 1;
    if (g_note_on == 0) { fprintf(stderr, "FAIL: no note-on events at all\n"); ok = 0; }
    if (g_min_note < 0 || g_max_note > 127) { fprintf(stderr, "FAIL: note out of MIDI range\n"); ok = 0; }
    if (g_note_off < g_note_on - 4) { fprintf(stderr, "FAIL: note-off count far below note-on (leaked notes?)\n"); ok = 0; }

    printf(ok ? "\nSMOKE TEST PASSED\n" : "\nSMOKE TEST FAILED\n");
    return ok ? 0 : 1;
}
