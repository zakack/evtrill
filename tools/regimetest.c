#define _GNU_SOURCE
/*
 * regimetest - assertions over the `deep` latch and its mode transitions.
 *
 * Includes evtrill.c and stubs only the uinput writes, so it exercises
 * the daemon's own state machine rather than a copy. Depths are fed one
 * sample at a time exactly as analog_drain feeds them.
 *
 *   build:  cmake --build build --target regimetest
 *   run:    ./build/regimetest
 */
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

static void test_emit(unsigned type, unsigned code, int value);
#define libevdev_uinput_write_event(d, t, c, v) test_emit((t), (c), (v))

#define main evtrill_main
#include "evtrill.c"
#undef main

/* The codes the log names v1..v4: pair 0's keys, then pair 1's. A pair's
 * keys are re-emitted under their own codes, so these are the keys each case
 * configures - case X swaps in its own and puts these back. */
#define VCODES_DEFAULT { KEY_Z, KEY_X, KEY_A, KEY_S }
static int vcode[4] = VCODES_DEFAULT;

static char log_buf[512];
static int  fails;

/* What the kernel's input core would be holding for v1/v2. Writes that do
 * not change it are dropped there and never reach the game, so the harness
 * drops them too: every assertion below is about what a player actually
 * gets, not about what the daemon happened to write. */
static int vstate[4];

/* Counted here rather than by sampling vstate between reports: a handover
 * onto an already-down key cycles it (release then press) WITHIN one report,
 * so its rising edge is invisible to anything that only looks at the state
 * afterwards. That is the note case H is about, so the counter has to see
 * every transition as it is written. */
static unsigned vpresses;

static unsigned vrepeats;

static void test_emit(unsigned type, unsigned code, int value) {
    if (type != EV_KEY) return;                    /* ignore SYN */
    int k = 0;
    while (k < 4 && vcode[k] != (int)code)
        k++;
    if (k == 4)
        return;                         /* passthrough, not a pair key */
    /* Autorepeat is not a transition: the kernel forwards value 2 to
     * userspace and leaves the key down. Counting it separately keeps it out
     * of the press/release log while still letting a test assert it was
     * forwarded at all, which is the whole point in `off` mode. */
    if (value == 2) { vrepeats++; return; }
    if (vstate[k] == value) return;                /* no-change: dropped */
    vstate[k] = value;
    if (value) vpresses++;
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s%s%c",
             n ? " " : "", (const char *[]){ "v1", "v2", "v3", "v4" }[k],
             value ? '+' : '-');
}

static void reset_log(void) { log_buf[0] = '\0'; }

static void expect_log(const char *what, const char *want) {
    if (strcmp(log_buf, want) != 0) {
        printf("  FAIL %-48s emitted [%s] want [%s]\n", what, log_buf, want);
        fails++;
    } else {
        printf("  ok   %-48s [%s]\n", what, want[0] ? want : "nothing");
    }
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-48s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-48s\n", what);
    }
}

static oid_config_t cfg;
static analog_pair_t ad;   /* pair 0, the one every case before W drives */

/* One analog report, exactly as analog_drain processes it. */
static void sample(float d0, float d1) {
    float d[2] = { d0, d1 };
    reset_log();
    analog_report(&ad, &cfg, d, 0);
}

/* One analog report at a given time, for the latch debounce (case S).
 * Every other case passes 0 and runs with the debounce off, which is what
 * keeps them the instantaneous test they were written against. */
static void sample_at(double ms, float d0, float d1) {
    float d[2] = { d0, d1 };
    reset_log();
    analog_report(&ad, &cfg, d, (uint64_t)(ms * 1e6));
}

/* ---------------------------------------------------------------------- *
 * analog_drain, driven by real encoded reports over a pipe.
 *
 * Everything above calls analog_key_feed / analog_deep_update /
 * analog_socd_edge directly, and so does tools/replay.c. That leaves
 * analog_drain - the read, the decode, and the depth rebuild that feeds all
 * three - as the one link in the analog chain nothing exercises, which is
 * exactly where a daemon that disagrees with replay would have to differ.
 * Case K closes that hole by running one gesture down both paths and
 * demanding the same output.
 *
 * A v2 report is 16 four-byte slots, [pad, key, packed, value], with the
 * 10-bit depth split across `value` and the top two bits of `packed`; a key
 * at rest is ABSENT rather than zero, which is the property analog_drain
 * rebuilds each frame from. Writes are 64 bytes, well under PIPE_BUF, so
 * each read() lifts exactly one report the way it does from hidraw.
 * ---------------------------------------------------------------------- */
static void encode_v2n(unsigned char *b, const uint16_t *usage,
                       const float *depth, int nkeys, float travel) {
    int slot = 0;
    memset(b, 0, ANALOG_V2_REPORT);
    for (int k = 0; k < nkeys; k++) {
        if (depth[k] <= 0.0f) continue;          /* at rest: not reported */
        unsigned v10 = (unsigned)(depth[k] / travel * 1023.0f + 0.5f);
        if (v10 > 1023) v10 = 1023;
        b[slot * 4 + 1] = (unsigned char)(usage[k] & 0xFF);
        b[slot * 4 + 2] = (unsigned char)((v10 & 0x03) << 6);
        b[slot * 4 + 3] = (unsigned char)(v10 >> 2);
        slot++;
    }
}

static void encode_v2(unsigned char *b, const uint16_t *usage,
                      const float *depth, float travel) {
    encode_v2n(b, usage, depth, 2, travel);
}

static void fresh(void) {
    memset(&ad, 0, sizeof ad);
    memset(vstate, 0, sizeof vstate);
    vpresses  = 0;
    reset_log();
}

/* ---------------------------------------------------------------------- *
 * The SOCD core on its own (cases M and Q).
 *
 * Everything above reaches socd_apply through the analog front-end, which
 * only ever runs it as OFF or as the toggle and never sends autorepeat.
 * These call it directly, the way process_event does for the digital path.
 * ---------------------------------------------------------------------- */
static oid_config_t dcfg;
static socd_state_t ds;

static void dfresh(int socd) {
    memset(&dcfg, 0, sizeof dcfg);
    memset(&ds, 0, sizeof ds);
    dcfg.n_pairs      = 1;
    dcfg.pair[0].socd = socd;
    dcfg.pair[0].k1   = KEY_Z;
    dcfg.pair[0].k2   = KEY_X;
    memset(vstate, 0, sizeof vstate);
    vpresses  = 0;
    vrepeats  = 0;
    reset_log();
}

/* One physical key edge: k == 0 is k1, k == 1 is k2. */
static void dkey(int k, int value) {
    reset_log();
    socd_apply(&ds, k == 0, value, dcfg.pair[0].socd, &dcfg.pair[0]);
}

/* Press-then-release, for the common "tap a key on its own" shape. */
static void dtap(int k) {
    reset_log();
    socd_apply(&ds, k == 0, 1, dcfg.pair[0].socd, &dcfg.pair[0]);
    socd_apply(&ds, k == 0, 0, dcfg.pair[0].socd, &dcfg.pair[0]);
}


int main(void) {
    g_log_quiet = 1;   /* the assertions are the output; not the latch log */
    memset(&cfg, 0, sizeof cfg);
    cfg.n_pairs      = 1;
    cfg.pair[0].socd = SOCD_ANALOG;
    cfg.pair[0].k1   = KEY_Z;
    cfg.pair[0].k2   = KEY_X;
    /* The daemon defaults, so the numbers here are the ones a player sees:
     * backplate at 3.80mm (95% of travel), latch disarms at 1.00mm. */
    cfg.analog.travel_mm     = 4.00f;
    cfg.analog.actuation_mm  = 1.00f;
    cfg.analog.rt_enabled    = 1;
    cfg.analog.rt_press_mm   = 0.30f;
    cfg.analog.rt_release_mm = 0.30f;
    cfg.analog.bottom_out_mm = 0.20f;      /* backplate at 3.80mm */
    /* No riding profile exists any more: while the latch is armed the
     * backplate is the switch and no rapid trigger runs. */
    analog_config_resolve(&cfg.analog);    /* release_mm follows actuation */

    /* ------------------------------------------------------------------ *
     * The feature exists to keep alt-tapping out of the toggle. Cases A
     * and B are therefore the ones that matter most; everything after
     * them is about behaving correctly once a rock has actually started.
     * ------------------------------------------------------------------ */

    puts("A. alt-tapping never arms it - taps clear of each other");
    fresh();
    sample(4.00f, 0.00f);
    expect_log("k1 taps", "v1+");
    sample(0.00f, 0.00f);
    expect_log("k1 leaves", "v1-");
    sample(0.00f, 4.00f);
    expect_log("k2 taps", "v2+");
    sample(0.00f, 0.00f);
    expect_log("k2 leaves", "v2-");
    expect_int("  never armed", ad.deep, 0);

    puts("B. alt-tapping never arms it - taps OVERLAPPING (the headline)");
    /* This is the shape that makes plain `toggle` misfire: k2 actuates
     * while k1 is still emitting-pressed, so the toggle steals, and then
     * k1's release is answered by re-pressing v1 - a note nobody struck.
     * Under the latch, k1 never reaches the floor at the same instant k2
     * does, so the whole exchange stays in SOCD_OFF and the k1 release is
     * just a release. */
    fresh();
    sample(4.00f, 0.00f);
    expect_log("k1 bottoms", "v1+");
    sample(3.85f, 1.20f);
    expect_log("k2 actuates while k1 is still pressed", "v2+");
    expect_int("  both virtual keys down (plain remap)", vstate[0] + vstate[1], 2);
    expect_int("  still not armed", ad.deep, 0);
    sample(3.20f, 3.85f);
    expect_log("k1 lifts: a plain release, NOT a toggle revert", "v1-");
    sample(0.50f, 3.85f);
    expect_log("k1 gone", "");
    expect_int("  never armed", ad.deep, 0);

    puts("C. planting both keys on the backplate arms it, silently");
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed but not yet down", "v2+");
    sample(3.85f, 3.85f);
    expect_log("k2 reaches the floor: arms, v1 steps aside", "v1-");
    expect_int("  armed", ad.deep, 1);
    expect_int("  exactly one virtual key held", vstate[0] + vstate[1], 1);

    puts("D. while riding, the backplate IS the switch - leaving it is a beat");
    /* No rapid trigger runs here and no reversal distance is consulted: the
     * only line that means anything while riding is the floor. */
    sample(3.85f, 3.60f);
    expect_log("k2 lifts off the floor: one beat, onto v1", "v2- v1+");

    puts("E. rocking: one beat per crossing, alternating");
    sample(3.85f, 3.85f);
    expect_log("k2 returns to the floor", "v1- v2+");
    sample(3.60f, 3.85f);
    expect_log("k1 lifts", "v2- v1+");

    puts("F. a finger lifting CLEAR does not disarm - the other is still planted");
    /* The regression this rule exists for, from
     * tests/toggle_5burst_3phys_5virt.csv. The old rule disarmed as soon as
     * either key passed actuation, so a finger lifted clear mid-burst tore
     * the latch down while the other finger was still on the floor - and the
     * next note then fired off the TAPPING profile at actuation instead of
     * at the backplate. Two thresholds inside one burst is jitter. */
    sample(0.90f, 3.85f);
    expect_log("k1 past actuation - the old rule disarmed here", "");
    expect_int("  still armed: k2 never left the floor", ad.deep, 1);
    expect_int("  and its virtual is still held", vstate[0], 1);
    sample(0.00f, 3.85f);
    expect_log("k1 lifted clear off the switch entirely", "");
    expect_int("  still riding", ad.deep, 1);

    puts("G. and its return beats at the BACKPLATE, not at actuation");
    sample(1.50f, 3.85f);
    expect_log("k1 back past actuation: no beat here", "");
    sample(3.70f, 3.85f);
    expect_log("still short of the floor: still nothing", "");
    sample(3.85f, 3.85f);
    expect_log("reaches the floor - the beat lands there", "v1- v2+");

    puts("H. disarms only when BOTH are off the floor, and releases only");
    sample(3.60f, 3.60f);
    expect_log("both leave the backplate", "v2-");
    expect_int("  disarmed", ad.deep, 0);
    expect_int("  nothing left held", vstate[0] + vstate[1], 0);
    /* Both keys are now PARKED: live cleared, rt_extreme still at 3.60. The
     * tapping profile judges against actuation_mm (1.00) from here, so a key
     * sitting this deep is exactly where a broken handoff shows up - clear
     * rt_extreme too and rule 3 reads 3.60mm as a fresh actuation and presses
     * a finger that is on its way up. Hold them there for a sample to prove
     * it does not, then bring one back down to prove it still can. */
    sample(3.60f, 3.60f);
    expect_log("held deep after the disarm: no fresh actuation", "");
    sample(3.85f, 3.60f);
    expect_log("k1 bottoms out again: recovers on a real stroke", "v1+");

    puts("H2. plant, plant, rock: two physical presses, three notes");
    /* The count tests/toggle_triple_2phys_3virt.csv is named for. */
    fresh();
    {
        static const float rock[][2] = {
            { 2.00f, 0.00f }, { 3.85f, 0.00f },   /* plant k1  */
            { 3.85f, 2.00f }, { 3.85f, 3.85f },   /* plant k2 -> arms */
            { 3.60f, 3.85f },                     /* rock: k1 leaves floor */
            { 0.00f, 3.85f }, { 0.00f, 0.00f },   /* lift out */
        };
        for (size_t i = 0; i < sizeof rock / sizeof rock[0]; i++)
            sample(rock[i][0], rock[i][1]);
        expect_int("three virtual presses from two physical",
                   (int)vpresses, 3);
    }

    puts("H3. a slider held through a full lift, ending when both leave");
    /* circle, circle, slider, circle. The hold has to survive one finger
     * coming clear off the switch, end when the planted finger lifts, and
     * NOT emit a phantom press for the finger that is on its way up. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("circle 1", "v1+");
    sample(3.85f, 2.00f);
    expect_log("circle 2", "v2+");
    sample(3.85f, 3.85f);
    expect_log("both plant: arms", "v1-");
    sample(3.60f, 3.85f);
    expect_log("k1 leaves the floor: the slider's note", "v2- v1+");
    sample(0.00f, 3.85f);
    expect_log("k1 lifts clear - the hold must survive", "");
    expect_int("  v1 still holding the slider", vstate[0], 1);
    sample(0.00f, 3.60f);
    expect_log("k2 lifts at the end of the track: slider ends", "v1-");
    expect_int("  disarmed", ad.deep, 0);
    sample(1.50f, 0.00f);
    expect_log("the final circle, tapped at actuation", "v1+");

    puts("I. a hard tap short of the floor never arms it");
    fresh();
    sample(3.50f, 0.00f);
    expect_log("k1 pressed hard, 87.5% of travel", "v1+");
    sample(3.50f, 3.50f);
    expect_log("k2 too - still short of 3.80", "v2+");
    expect_int("  not armed", ad.deep, 0);

    puts("J. while riding, depth short of the floor never beats");
    /* What the old two-profile rule was protecting, now structural. An
     * earlier design ran a second rapid-trigger profile while riding and
     * picked between the two by the latch; a rock that overshot re-pressed
     * after a reversal distance instead of at the floor, so the beat landed
     * early on exactly the rocks that went high. With the backplate as the
     * only line, amplitude cannot reach the decision at all - there is
     * nothing left to select between. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms", "v1-");
    sample(3.85f, 3.60f);
    expect_log("k2 leaves the floor: beat", "v2- v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 travels far further up: no second beat", "");
    sample(3.85f, 3.70f);
    expect_log("and back down, still short of the floor: nothing", "");
    sample(3.85f, 3.85f);
    expect_log("reaches the floor - the beat lands there, not before",
               "v1- v2+");

    puts("K. analog_drain agrees with the direct calls, report for report");
    {
        /* One gesture, two paths: the direct calls every other case (and
         * replay) makes, then the same depths encoded as hardware reports
         * and pushed through analog_drain. Any divergence here is a daemon
         * that does not do what replay predicts. */
        static const float gesture[][2] = {
            { 0.00f, 0.00f }, { 2.00f, 0.00f }, { 3.85f, 0.00f },
            { 3.85f, 2.00f }, { 3.85f, 3.85f },            /* arms */
            { 3.85f, 3.20f }, { 3.85f, 3.82f },            /* rock */
            { 3.20f, 3.85f }, { 3.85f, 3.85f },
            { 0.00f, 3.85f }, { 0.00f, 0.00f },            /* lift out */
        };
        size_t nstep = sizeof gesture / sizeof gesture[0];
        char   direct[512] = "", drained[512] = "";

        unsigned arm_direct = 0, arm_drain = 0;

        fresh();
        for (size_t i = 0; i < nstep; i++) {
            int was = ad.deep;
            sample(gesture[i][0], gesture[i][1]);
            if (!was && ad.deep) arm_direct++;
            size_t n = strlen(direct);
            snprintf(direct + n, sizeof direct - n, "%s%s",
                     n && log_buf[0] ? "|" : "", log_buf);
        }

        int fd[2];
        if (pipe(fd) != 0) {
            printf("  FAIL %-48s pipe: %s\n", "pipe for the drain path",
                   strerror(errno));
            fails++;
        } else {
            analog_dev_t dd;
            memset(&dd, 0, sizeof dd);
            dd.kind = EP_ANALOG;
            dd.fd   = fd[0];
            dd.fmt  = ANALOG_FMT_V2;
            dd.n_pairs          = 1;
            dd.pair[0].usage[0] = 0x1d;    /* KEY_Z */
            dd.pair[0].usage[1] = 0x1b;    /* KEY_X */
            snprintf(dd.path, sizeof dd.path, "pipe");
            fcntl(fd[0], F_SETFL, O_NONBLOCK);
            vstate[0] = vstate[1] = 0;

            for (size_t i = 0; i < nstep; i++) {
                unsigned char rep[ANALOG_V2_REPORT];
                float         d[2] = { gesture[i][0], gesture[i][1] };
                encode_v2(rep, dd.pair[0].usage, d, cfg.analog.travel_mm);
                reset_log();
                if (write(fd[1], rep, sizeof rep) != (ssize_t)sizeof rep) {
                    printf("  FAIL short write to the drain pipe\n");
                    fails++;
                    break;
                }
                int was = dd.pair[0].deep;
                analog_drain(&dd, &cfg);
                if (!was && dd.pair[0].deep) arm_drain++;
                size_t n = strlen(drained);
                snprintf(drained + n, sizeof drained - n, "%s%s",
                         n && log_buf[0] ? "|" : "", log_buf);
            }
            close(fd[0]);
            close(fd[1]);

            if (strcmp(direct, drained) != 0) {
                printf("  FAIL %-48s\n    direct [%s]\n    drain  [%s]\n",
                       "same virtual output either way", direct, drained);
                fails++;
            } else {
                printf("  ok   %-48s [%s]\n",
                       "same virtual output either way", direct);
            }
            /* Compare the two paths against the EXPECTED count, not just
             * against each other: a regression that made the latch
             * unarmable would break both identically and still pass an
             * equivalence-only check, quietly reducing this case to a test
             * of the plain remap. */
            expect_int("  direct path armed exactly once", (int)arm_direct, 1);
            expect_int("  drain path armed exactly once", (int)arm_drain, 1);
            expect_int("  and both ended disarmed",
                       dd.pair[0].deep | ad.deep, 0);
        }
    }

    puts("L. the key that plants SECOND keeps its virtual - last input wins");
    /* Arming has to drop one of two held keys, and it keeps the most recently
     * pressed one. Not cosmetic: the second key presses at actuation_mm and
     * the latch arms when it reaches the BACKPLATE, so only the travel
     * between those two thresholds separates its key-down from the arming
     * release - 19ms and 24ms in tests/toggle_5burst and tests/toggle_triple.
     * Keep the first-planted key instead and you emit a key-up 19ms after the
     * key-down a note just landed on, short enough for a game to miss the
     * press outright. The older virtual has been down an order of magnitude
     * longer (101ms, 116ms) and costs nothing to release.
     *
     * Since the redesign this no longer affects whether a beat lands - every
     * handover is real either way - but it still decides which virtual key
     * leads a burst, so it must stay deterministic. */
    fresh();
    sample(3.85f, 0.00f);
    expect_log("k1 plants first", "v1+");
    sample(3.85f, 2.00f);
    expect_log("k2 pressed second", "v2+");
    sample(3.85f, 3.85f);
    expect_log("arms: the OLDER virtual steps aside", "v1-");
    expect_int("  the newer one keeps it", vstate[1], 1);

    /* Mirrored. Nothing privileges k1 over k2 anywhere in this path - the
     * rule is "planted second", not "k2". */
    fresh();
    sample(0.00f, 3.85f);
    expect_log("k2 plants first", "v2+");
    sample(2.00f, 3.85f);
    expect_log("k1 pressed second", "v1+");
    sample(3.85f, 3.85f);
    expect_log("arms: again the older one steps aside", "v2-");
    expect_int("  and again the newer keeps it", vstate[0], 1);

    puts("M. the digital core: toggle and snappy on k1/k2");
    dfresh(SOCD_TOGGLE);
    dtap(0);
    expect_log("k1 tapped alone", "v1+ v1-");
    dtap(0);
    expect_log("k1 again - same virtual", "v1+ v1-");
    dtap(1);
    expect_log("k2 tapped alone", "v2+ v2-");

    dfresh(SOCD_TOGGLE);
    dtap(1);
    expect_log("k2 pressed FIRST emits v2", "v2+ v2-");

    dfresh(SOCD_TOGGLE);
    dkey(0, 1); expect_log("k1 down", "v1+");
    dkey(1, 1); expect_log("k2 down: the toggle steals", "v1- v2+");
    dkey(0, 0); expect_log("k1 up: reverting toggle", "v2- v1+");
    dkey(1, 0); expect_log("k2 up: last key released", "v1-");

    /* snappy has no coverage anywhere else, so pin the one branch that
     * distinguishes it: S_RELEASE reverts only for the ACTIVE key. */
    dfresh(SOCD_SNAPPY);
    dkey(0, 1); expect_log("snappy: k1 down", "v1+");
    dkey(1, 1); expect_log("  k2 down steals", "v1- v2+");
    dkey(0, 0); expect_log("  releasing the SUPPRESSED key does nothing", "");
    dkey(1, 0); expect_log("  releasing the active key releases", "v2-");

    dfresh(SOCD_SNAPPY);
    dkey(0, 1); expect_log("snappy: k1 down", "v1+");
    dkey(1, 1); expect_log("  k2 down steals", "v1- v2+");
    dkey(1, 0); expect_log("  releasing the ACTIVE key reverts", "v2- v1+");
    dkey(0, 0); expect_log("  last key released", "v1-");

    puts("Q. autorepeat: forwarded in off mode, dropped by the toggle");
    /* off mode forwards autorepeat to v1/v2 verbatim and always has. The
     * state-machine modes drop it inside socd_apply. */
    dfresh(SOCD_OFF);
    dkey(0, 1); expect_log("k1 down", "v1+");
    dkey(0, 2);
    expect_log("k1 autorepeat: not a press", "");
    expect_int("  but it IS forwarded in off mode", (int)vrepeats, 1);
    expect_int("  and k1 is still tracked as held", ds.k1, 1);
    dkey(0, 0); expect_log("k1 up", "v1-");

    dfresh(SOCD_TOGGLE);
    dkey(0, 1); expect_log("k1 down", "v1+");
    dkey(0, 2); expect_log("autorepeat is dropped by the toggle", "");
    expect_int("  not forwarded either", (int)vrepeats, 0);
    expect_int("  and k1 is still tracked as held", ds.k1, 1);
    dkey(0, 0); expect_log("k1 up", "v1-");

    /* off is a plain 1:1 remap: both virtuals down together. */
    dfresh(SOCD_OFF);
    dkey(0, 1); expect_log("off: k1 down", "v1+");
    dkey(1, 1); expect_log("  k2 down - no stealing", "v2+");
    dkey(0, 0); expect_log("  k1 up", "v1-");
    dkey(1, 0); expect_log("  k2 up", "v2-");

    puts("S. latch_debounce_ms: both must STAY planted before it arms");
    /* Two hard taps that both graze the floor in one report are an
     * instant; a rock plants and stays. Until the debounce runs out the
     * pair is on the plain remap, so the only cost is that the toggle
     * engages later - nothing here may press on arming. */
    cfg.analog.latch_debounce_ms = 10.0f;
    fresh();
    sample_at(0.0, 3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample_at(1.0, 3.85f, 3.85f);
    expect_log("k2 slams onto the floor", "v2+");
    expect_int("  not armed on first contact", ad.deep, 0);
    sample_at(2.0, 0.00f, 0.00f);
    expect_log("both leave: a hard double tap", "v1- v2-");
    expect_int("  never armed", ad.deep, 0);

    fresh();
    sample_at(10.0, 3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample_at(11.0, 3.85f, 3.85f);
    expect_log("k2 planted", "v2+");
    sample_at(16.0, 3.85f, 3.85f);
    expect_int("  5ms in: still pending", ad.deep, 0);
    sample_at(17.0, 3.85f, 3.00f);
    expect_log("k2 leaves the floor mid-window", "v2-");
    sample_at(19.0, 3.85f, 3.85f);
    expect_log("k2 back down: window restarts", "v2+");
    sample_at(28.0, 3.85f, 3.85f);
    expect_int("  9ms after the restart: still pending", ad.deep, 0);
    sample_at(29.0, 3.85f, 3.85f);
    expect_log("10ms planted: arms, v1 steps aside", "v1-");
    expect_int("  armed", ad.deep, 1);
    sample_at(30.0, 3.85f, 3.60f);
    expect_log("riding works as before", "v2- v1+");
    sample_at(31.0, 0.00f, 0.00f);
    expect_log("both leave: disarm is NOT debounced", "v1-");
    expect_int("  disarmed", ad.deep, 0);
    cfg.analog.latch_debounce_ms = 0.0f;

    puts("T. latch_debounce_ms runs out on the CLOCK, not on the next report");
    /* The board reports on change: two planted, still fingers send
     * nothing. Case S feeds a report every millisecond, which the hardware
     * does not do; here the plant goes quiet and only the timer is left to
     * end the window (issue #12). */
    cfg.analog.latch_debounce_ms = 30.0f;
    uint64_t dl = 0;
    fresh();
    sample_at(0.0, 3.85f, 0.00f);
    expect_log("k1 planted", "v1+");
    sample_at(5.0, 3.85f, 3.85f);
    expect_log("k2 planted, then silence", "v2+");
    expect_int("  a deadline is running", analog_deep_deadline(&ad, &cfg, &dl), 1);
    expect_int("  at plant + 30ms", (int)(dl / 1000000ull), 35);
    reset_log();
    analog_deep_tick(&ad, &cfg, 34000000ull);
    expect_int("  timer early by 1ms: still pending", ad.deep, 0);
    analog_deep_tick(&ad, &cfg, 35000000ull);
    expect_log("timer at the deadline arms it, v1 steps aside", "v1-");
    expect_int("  armed with no report", ad.deep, 1);
    expect_int("  and the deadline is gone", analog_deep_deadline(&ad, &cfg, &dl), 0);
    sample_at(200.0, 3.85f, 3.00f);
    expect_log("the first lift of the rock is a beat", "v2- v1+");
    sample_at(210.0, 0.00f, 0.00f);
    expect_log("both leave: disarm", "v1-");
    expect_int("  disarmed", ad.deep, 0);

    /* No timer serviced before the next report - the tools, or an epoll
     * batch that drains the board before the timer. The window ran out
     * before this report, so the report must already run under the
     * toggle; judging the lift first would cancel the plant and lose the
     * rock, which is the original defect. */
    fresh();
    sample_at(0.0, 3.85f, 0.00f);
    sample_at(5.0, 3.85f, 3.85f);
    sample_at(200.0, 3.85f, 3.00f);
    expect_log("lift after the window: arms first, then beats", "v1- v2- v1+");
    expect_int("  armed", ad.deep, 1);

    /* A key leaving the floor ends the window: nothing for the timer. */
    fresh();
    sample_at(0.0, 3.85f, 3.85f);
    sample_at(10.0, 3.85f, 3.00f);
    expect_int("  one key left: no deadline",
               analog_deep_deadline(&ad, &cfg, &dl), 0);
    analog_deep_tick(&ad, &cfg, 100000000ull);
    expect_int("  and a late tick does not arm", ad.deep, 0);

    /* A tick exactly ON the deadline arms, for a value that is not exact
     * in a float too. replay fires its tick there; with the deadline
     * truncated and the test in floating point this came up short. */
    cfg.analog.latch_debounce_ms = 33.3f;
    fresh();
    sample_at(5.0, 3.85f, 3.85f);
    expect_int("33.3ms: a deadline is running",
               analog_deep_deadline(&ad, &cfg, &dl), 1);
    analog_deep_tick(&ad, &cfg, dl - 1);
    expect_int("  1ns early: still pending", ad.deep, 0);
    analog_deep_tick(&ad, &cfg, dl);
    expect_int("  on the deadline: armed", ad.deep, 1);
    cfg.analog.latch_debounce_ms = 0.0f;

    /* Debounce off: arming is instantaneous, so nothing ever waits. */
    fresh();
    sample_at(0.0, 3.85f, 3.85f);
    expect_int("debounce 0 arms on the report", ad.deep, 1);
    expect_int("  and never asks for a timer",
               analog_deep_deadline(&ad, &cfg, &dl), 0);

    puts("U. arm_on_report: the rule from before the timer, for old traces");
    /* replay sets this on a trace recorded by a daemon that had no debounce
     * timer, and it has to reproduce that daemon's defect exactly - the
     * point is what the player's virtual keys DID in that session, not what
     * they should have done. These are case T's first two gestures with the
     * opposite outcomes. */
    cfg.analog.latch_debounce_ms = 30.0f;
    fresh();
    ad.arm_on_report = 1;
    sample_at(0.0, 3.85f, 0.00f);
    sample_at(5.0, 3.85f, 3.85f);
    expect_int("planted, then silence: no deadline to wait for",
               analog_deep_deadline(&ad, &cfg, &dl), 0);
    reset_log();
    analog_deep_tick(&ad, &cfg, 100000000ull);
    expect_log("a tick long after the window does nothing", "");
    expect_int("  not armed", ad.deep, 0);
    sample_at(200.0, 3.85f, 3.00f);
    expect_log("the first lift is a plain release: the rock never armed",
               "v2-");
    expect_int("  still not armed", ad.deep, 0);

    /* It could still arm, but only from a report that found both keys on
     * the floor after the window - so late, and stamped on that report. */
    fresh();
    ad.arm_on_report = 1;
    sample_at(0.0, 3.85f, 0.00f);
    sample_at(5.0, 3.85f, 3.85f);
    sample_at(34.0, 3.85f, 3.86f);
    expect_int("a report inside the window: still pending", ad.deep, 0);
    sample_at(72.0, 3.85f, 3.87f);
    expect_log("a report after it, both still planted: arms there", "v1-");
    expect_int("  armed", ad.deep, 1);
    sample_at(200.0, 3.85f, 3.00f);
    expect_log("  and rides as usual from then on", "v2- v1+");

    /* The flag lives on the device, and a fresh one does not have it. */
    fresh();
    sample_at(0.0, 3.85f, 0.00f);
    sample_at(5.0, 3.85f, 3.85f);
    expect_int("a fresh device is back on the timer",
               analog_deep_deadline(&ad, &cfg, &dl), 1);
    cfg.analog.latch_debounce_ms = 0.0f;

    /* ------------------------------------------------------------------ *
     * Pairs (W, X, Y). Everything above runs one pair, which is the
     * regression gate; these are about two pairs being two machines that
     * never see each other. The taiko shape: dons on one pair, kats on the
     * other.
     * ------------------------------------------------------------------ */
    {
        oid_config_t pc;
        memset(&pc, 0, sizeof pc);
        pc.analog    = cfg.analog;
        pc.n_pairs   = 2;
        pc.pair[0]   = (keypair_t){ .k1 = KEY_Z, .k2 = KEY_X,
                                    .socd = SOCD_ANALOG,
                                    .hid_k1 = -1, .hid_k2 = -1 };
        pc.pair[1]   = (keypair_t){ .k1 = KEY_A, .k2 = KEY_S,
                                    .socd = SOCD_ANALOG,
                                    .hid_k1 = -1, .hid_k2 = -1 };

        int fd[2];
        if (pipe(fd) != 0) {
            printf("  FAIL pipe for case W: %s\n", strerror(errno));
            fails++;
            goto skip_w;
        }
        fcntl(fd[0], F_SETFL, O_NONBLOCK);
        analog_dev_t bd;
        memset(&bd, 0, sizeof bd);
        bd.kind    = EP_ANALOG;
        bd.fd      = fd[0];
        bd.fmt     = ANALOG_FMT_V2;
        bd.n_pairs = 2;
        bd.pair[0].idx = 0;
        bd.pair[1].idx = 1;
        static const uint16_t usage4[4] = { 0x1d, 0x1b, 0x04, 0x16 };
        memcpy(bd.pair[0].usage, usage4,     sizeof bd.pair[0].usage);
        memcpy(bd.pair[1].usage, usage4 + 2, sizeof bd.pair[1].usage);
        snprintf(bd.path, sizeof bd.path, "pipe");

        /* One hardware report carrying all four keys, through the real
         * read/decode/dispatch path. */
        #define BOARD(t, a, b, c, d) do {                                   \
            float dd4[4] = { a, b, c, d };                                  \
            unsigned char rep[ANALOG_V2_REPORT];                            \
            encode_v2n(rep, usage4, dd4, 4, pc.analog.travel_mm);           \
            reset_log();                                                    \
            if (write(fd[1], rep, sizeof rep) != (ssize_t)sizeof rep) {     \
                printf("  FAIL short write\n"); fails++;                    \
            }                                                               \
            analog_drain(&bd, &pc);                                         \
            (void)(t);                                                      \
        } while (0)

        puts("W. two analog pairs on one board: two latches, never one");
        memset(vstate, 0, sizeof vstate);
        BOARD(0, 3.85f, 0.00f, 0.00f, 0.00f);
        expect_log("pair 0 k1 plants", "v1+");
        BOARD(0, 3.85f, 3.85f, 0.00f, 0.00f);
        expect_log("pair 0 k2 slams down: arms, toggles to it", "v1- v2+");
        expect_int("  pair 0 armed", bd.pair[0].deep, 1);
        expect_int("  pair 1 untouched", bd.pair[1].deep, 0);
        BOARD(0, 3.85f, 3.85f, 2.00f, 0.00f);
        expect_log("pair 1 taps while pair 0 rides", "v3+");
        BOARD(0, 3.85f, 3.85f, 0.00f, 0.00f);
        expect_log("  and lifts: a plain release", "v3-");
        BOARD(0, 3.85f, 3.85f, 3.20f, 3.85f);
        expect_log("pair 1 overlaps short of the floor: plain remap",
                   "v3+ v4+");
        expect_int("  pair 1 still not armed", bd.pair[1].deep, 0);
        BOARD(0, 3.85f, 3.60f, 3.20f, 3.85f);
        expect_log("pair 0 rocks under it: one beat, pair 0 only", "v2- v1+");
        BOARD(0, 3.85f, 3.60f, 0.00f, 0.00f);
        expect_log("pair 1 releases both", "v3- v4-");

        /* The big note: both keys of one colour slammed onto the floor in
         * ONE report and released together. With no debounce it arms, and
         * it arms before either press edge has been routed - so the
         * reconciliation presses the survivor itself, and the other key's
         * press and the toggle's release of it land in the same report: a
         * ZERO-LENGTH press. Still two notes, never three, but one of them
         * is a down/up a game may not see. This is why taiko wants a
         * debounce; the next block is the same strike with one. */
        BOARD(0, 3.85f, 3.60f, 3.85f, 3.85f);
        expect_log("big note, no debounce: arms, one press is zero-length",
                   "v4+ v3+ v3-");
        BOARD(0, 3.85f, 3.60f, 0.00f, 0.00f);
        expect_log("  both lift together: one release, no phantom", "v4-");
        expect_int("  pair 1 disarmed", bd.pair[1].deep, 0);
        expect_int("  pair 0 never noticed", bd.pair[0].deep, 1);

        BOARD(0, 0.00f, 0.00f, 0.00f, 0.00f);
        expect_log("pair 0 leaves the floor: pair 0 disarms", "v1-");

        /* The same big note under a debounce: the strike is an instant, so
         * the window never runs out, the pair stays on the plain remap, and
         * both notes are ordinary presses and releases. */
        pc.analog.latch_debounce_ms = 30.0f;
        {
            float b0[2] = { 0.00f, 0.00f }, b1[2] = { 3.85f, 3.85f },
                  b2[2] = { 0.00f, 0.00f };
            reset_log();
            analog_report(&bd.pair[1], &pc, b1, 100000000ull);
            analog_report(&bd.pair[0], &pc, b0, 100000000ull);
            expect_log("big note, 30ms debounce: two plain presses",
                       "v3+ v4+");
            reset_log();
            analog_report(&bd.pair[1], &pc, b2, 112000000ull);
            expect_log("  released 12ms later: two plain releases",
                       "v3- v4-");
            expect_int("  never armed", bd.pair[1].deep, 0);
        }
        pc.analog.latch_debounce_ms = 0.0f;
        expect_int("  nothing held anywhere",
                   vstate[0] + vstate[1] + vstate[2] + vstate[3], 0);

        /* The shared timer: two plants pending at once, one deadline each,
         * and the timerfd follows the EARLIER. A tick arms only the pair
         * whose window has run out. */
        pc.analog.latch_debounce_ms = 30.0f;
        uint64_t wdl = 0;
        float a0[2] = { 3.85f, 3.85f }, a1[2] = { 3.85f, 3.85f };
        memset(vstate, 0, sizeof vstate);
        analog_report(&bd.pair[1], &pc, a1, 10000000ull);
        analog_report(&bd.pair[0], &pc, a0, 20000000ull);
        expect_int("two plants pending: one deadline",
                   analog_dev_deadline(&bd, &pc, &wdl), 1);
        expect_int("  it is pair 1's, the earlier (40ms)",
                   (int)(wdl / 1000000ull), 40);
        reset_log();
        analog_dev_tick(&bd, &pc, wdl);
        expect_int("  the tick arms pair 1", bd.pair[1].deep, 1);
        expect_int("  and not pair 0, 10ms short", bd.pair[0].deep, 0);
        expect_int("  the timer moves on to pair 0's (50ms)",
                   analog_dev_deadline(&bd, &pc, &wdl) &&
                   wdl / 1000000ull == 50, 1);
        analog_dev_tick(&bd, &pc, wdl);
        expect_int("  which arms pair 0 in turn", bd.pair[0].deep, 1);
        expect_int("  and nothing is left to wait for",
                   analog_dev_deadline(&bd, &pc, &wdl), 0);
        #undef BOARD
        close(fd[0]);
        close(fd[1]);
    }
skip_w:

    puts("X. two digital pairs: separate machines, and a mixed board");
    {
        oid_config_t pc;
        memset(&pc, 0, sizeof pc);
        pc.n_pairs = 2;
        pc.pair[0] = (keypair_t){ .k1 = KEY_D, .k2 = KEY_F,
                                  .socd = SOCD_TOGGLE };
        pc.pair[1] = (keypair_t){ .k1 = KEY_J, .k2 = KEY_K,
                                  .socd = SOCD_TOGGLE };
        const int xcodes[4] = { KEY_D, KEY_F, KEY_J, KEY_K };
        memcpy(vcode, xcodes, sizeof vcode);
        input_dev_t in;
        memset(&in, 0, sizeof in);
        memset(vstate, 0, sizeof vstate);
        #define KEY(c, v) do {                                              \
            struct input_event ie = { .type = EV_KEY, .code = (c),          \
                                      .value = (v) };                       \
            reset_log();                                                    \
            process_event(&in, &ie, &pc);                                   \
        } while (0)

        KEY(KEY_D, 1); expect_log("pair 0 k1 down", "v1+");
        KEY(KEY_J, 1); expect_log("pair 1 k1 down: no stealing across pairs",
                                  "v3+");
        KEY(KEY_F, 1); expect_log("pair 0 k2: toggles inside pair 0 only",
                                  "v1- v2+");
        expect_int("  pair 1's virtual still held", vstate[2], 1);
        KEY(KEY_D, 0); expect_log("pair 0 k1 up: pair 0 reverts", "v2- v1+");
        KEY(KEY_J, 0); expect_log("pair 1 k1 up: just a release", "v3-");
        KEY(KEY_F, 0); KEY(KEY_K, 1);
        expect_log("pair 1 k2 alone", "v4+");
        KEY(KEY_K, 0);
        KEY(KEY_A, 1); expect_log("a key in no pair passes through", "");

        /* Mixed board: dons analog, kats digital toggle. On the analog
         * board only the analog pair's digital copies are dropped - the
         * digital pair has nobody else to drive it. */
        pc.pair[0].socd = SOCD_ANALOG;
        memset(&in, 0, sizeof in);
        in.analog = 1;
        memset(vstate, 0, sizeof vstate);
        KEY(KEY_D, 1); expect_log("analog pair's digital copy: dropped", "");
        KEY(KEY_J, 1); expect_log("digital pair on the same board: driven",
                                  "v3+");
        KEY(KEY_K, 1); expect_log("  and toggles", "v3- v4+");
        KEY(KEY_J, 0); KEY(KEY_K, 0); KEY(KEY_D, 0);

        /* release_stuck on a dying device covers every pair. */
        memset(&in, 0, sizeof in);
        pc.pair[0].socd = SOCD_TOGGLE;
        memset(vstate, 0, sizeof vstate);
        KEY(KEY_D, 1); KEY(KEY_K, 1);
        reset_log();
        for (int p = 0; p < pc.n_pairs; p++)
            release_stuck(&in.socd[p], &pc.pair[p]);
        expect_log("device dies holding both pairs: both released",
                   "v1- v4-");
        #undef KEY
        const int dcodes[4] = VCODES_DEFAULT;
        memcpy(vcode, dcodes, sizeof vcode);
    }

    puts("Y. config: a closed schema, and pairs share nothing");
    {
        /* `msg` must appear in what a refusal logs, so a case cannot pass
         * by being refused for some unrelated reason. */
        #define PAIR_FJ "  - {keys: [KEY_F, KEY_J]}\n"
        static const struct { const char *what, *yaml; int ok, n;
                              const char *msg; } cf[] = {
            { "one pair, with its mode and a hid override",
              "pairs:\n  - {keys: [KEY_Z, KEY_X], mode: analog, "
              "hid: [0x1d, auto]}\n", 1, 1, NULL },
            { "two pairs, mode defaulted from the top",
              "mode: last\npairs:\n"
              "  - {keys: [KEY_F, KEY_J], mode: analog}\n"
              "  - {keys: [KEY_D, KEY_K]}\n", 1, 2, NULL },
            { "no pairs: is refused - there is no built-in pair",
              "mode: trill\n", 0, 0, "'pairs' is required" },
            { "a typo at the root is refused",
              "pair:\n" PAIR_FJ, 0, 0, "unknown key 'pair'" },
            { "the old socd: field is just an unknown key",
              "socd: toggle\npairs:\n" PAIR_FJ, 0, 0,
              "unknown key 'socd'" },
            { "the old keys: block is just an unknown key",
              "keys: {k1: KEY_Z, k2: KEY_X}\npairs:\n" PAIR_FJ, 0, 0,
              "unknown key 'keys'" },
            { "an old audio: block is just an unknown key",
              "audio: {enabled: true}\npairs:\n" PAIR_FJ, 0, 0,
              "unknown key 'audio'" },
            { "the old k1/v1 spelling on a pair is refused",
              "pairs:\n  - {k1: KEY_F, k2: KEY_J, v1: KEY_1, v2: KEY_2}\n",
              0, 0, "unknown key 'pairs[0].k1'" },
            { "an old mode name is refused, synonyms included",
              "pairs:\n  - {keys: [KEY_F, KEY_J], mode: toggle}\n", 0, 0,
              "'pairs[0].mode' must be" },
            { "'off' is not a mode any more",
              "mode: off\npairs:\n" PAIR_FJ, 0, 0, "'mode' must be" },
            { "keys must be exactly two",
              "pairs:\n  - {keys: [KEY_F, KEY_J, KEY_K]}\n", 0, 0,
              "'pairs[0].keys' must be a list of two keys" },
            { "an unknown key under analog is refused",
              "pairs:\n" PAIR_FJ "analog: {gate: true}\n", 0, 0,
              "unknown key 'analog.gate'" },
            { "an unknown key under rapid_trigger is refused",
              "pairs:\n" PAIR_FJ "analog: {rapid_trigger: "
              "{deep_press_mm: 1}}\n", 0, 0,
              "unknown key 'analog.rapid_trigger.deep_press_mm'" },
            { "an unknown key under uinput is refused",
              "pairs:\n" PAIR_FJ "uinput: {nmae: x}\n", 0, 0,
              "unknown key 'uinput.nmae'" },
            { "a key given twice is refused",
              "mode: last\nmode: trill\npairs:\n" PAIR_FJ, 0, 0,
              "'mode' is given more than once" },
            { "both keys of a pair the same is refused",
              "pairs:\n  - {keys: [KEY_F, KEY_F]}\n", 0, 0,
              "both keys are the same key" },
            { "a key in two pairs is refused",
              "pairs:\n" PAIR_FJ "  - {keys: [KEY_J, KEY_K]}\n", 0, 0,
              "share a key" },
            { "one hid override in two pairs is refused",
              "pairs:\n"
              "  - {keys: [KEY_F, KEY_J], hid: [9, auto]}\n"
              "  - {keys: [KEY_D, KEY_K], hid: [auto, 9]}\n",
              0, 0, "both override a key to hid 0x09" },
            { "hid usage 0 is refused",
              "pairs:\n  - {keys: [KEY_F, KEY_J], hid: [0, auto]}\n", 0, 0,
              "'pairs[0].hid' must be" },
            { "a pair entry without keys is refused",
              "pairs:\n  - {mode: last}\n", 0, 0,
              "'pairs[0].keys' is required" },
        };
        #undef PAIR_FJ
        char path[] = "/tmp/regimetest-XXXXXX";
        char errp[] = "/tmp/regimetest-err-XXXXXX";
        int  tfd = mkstemp(path);
        int  efd = mkstemp(errp);
        for (size_t i = 0; tfd >= 0 && efd >= 0 &&
                           i < sizeof cf / sizeof cf[0]; i++) {
            /* ftruncate leaves the offset where it was; rewind, or the next
             * log lands past a run of NULs and reads back as empty. */
            if (ftruncate(tfd, 0) != 0 || ftruncate(efd, 0) != 0 ||
                lseek(efd, 0, SEEK_SET) != 0 ||
                pwrite(tfd, cf[i].yaml, strlen(cf[i].yaml), 0) < 0) {
                printf("  FAIL could not write %s\n", path);
                fails++;
                break;
            }
            oid_config_t yc;
            config_init(&yc);
            g_log_quiet = 1;
            fflush(stderr);
            int saved = dup(2);
            dup2(efd, 2);                /* capture what the refusal says */
            int rc = load_config(path, &yc);
            fflush(stderr);
            dup2(saved, 2);
            close(saved);
            expect_int(cf[i].what, rc == 0, cf[i].ok);
            if (cf[i].msg) {
                char buf[2048] = "";
                ssize_t got = pread(efd, buf, sizeof buf - 1, 0);
                buf[got > 0 ? got : 0] = '\0';
                if (!strstr(buf, cf[i].msg)) {
                    printf("  FAIL   says \"%s\", logged: %s\n",
                           cf[i].msg, buf);
                    fails++;
                }
            }
            if (rc == 0 && cf[i].ok) {
                expect_int("  pair count", yc.n_pairs, cf[i].n);
                if (i == 0)
                    expect_int("  mode and hid on the pair",
                               yc.pair[0].hid_k1 == 0x1d &&
                               yc.pair[0].hid_k2 == -1 &&
                               yc.pair[0].socd == SOCD_ANALOG, 1);
                if (i == 1)
                    expect_int("  per-pair mode, and the default",
                               yc.pair[0].socd == SOCD_ANALOG &&
                               yc.pair[1].socd == SOCD_SNAPPY, 1);
                config_free(&yc);
            }
        }
        if (tfd >= 0) { close(tfd); unlink(path); }
        if (efd >= 0) { close(efd); unlink(errp); }
    }

    printf("\n%s\n", fails ? "FAILURES ABOVE" : "all assertions passed");
    return fails ? 1 : 0;
}
