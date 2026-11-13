/*
 * evtrill - two-key trill daemon for rhythm games
 *
 * The daemon directly opens (and exclusively grabs) one or more evdev
 * keyboard devices - either an explicit config list or, by default,
 * every keyboard-shaped device that advertises every configured key
 * (with inotify-driven hotplug either way) - runs a state machine over
 * each configured pair of keys, and mirrors every other event verbatim
 * through a single uinput virtual keyboard. A pair's keys keep their own
 * codes; what the machine changes is WHEN they go down and up. Modes,
 * per pair:
 *
 *   trill  - with both keys held, every stroke or release of either one
 *            is the next note, alternating
 *   last   - the newest stroke wins; releasing the active key falls back
 *            to the held one, releasing the suppressed one does nothing
 *   analog - each key is independent (by travel, with a software rapid
 *            trigger) until both are planted on the backplate, then they
 *            trill until both leave it
 *
 * In analog mode the keyboard's travel depth is read straight off its
 * vendor HID interface and the latch decides which rule is in
 * force: it arms when BOTH keys are on the backplate in one report and
 * disarms when BOTH are off it. See analog_latch_update.
 *
 * Copyright 2026 Zachary Kessler
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/timerfd.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <yaml.h>

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

#define DEF_K1     KEY_Z
#define DEF_K2     KEY_X
#define DEF_NAME   "evtrill virtual keyboard"

/* Where the config lives, and the packaged default used when it does not
 * exist. CMake overrides both to match the install prefix. */
#ifndef SYS_CONFIG
#define SYS_CONFIG "/etc/evtrill/config.yaml"
#endif
#ifndef DEF_CONFIG
#define DEF_CONFIG "/usr/share/evtrill/config.yaml"
#endif
#ifndef EVTRILL_VERSION
#define EVTRILL_VERSION "unknown"
#endif

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

/* Set by the offline tools (replay, machinetest) while they push samples
 * through the state machine. They run the same code the daemon does, so the
 * daemon's own progress chatter comes out of them too - and replay's
 * bottom_out sweep runs the whole trace nine times over, which turns one
 * latch log per rock into thousands of lines across a real recording.
 * Warnings and errors are never suppressed; only the running commentary. */
static int g_log_quiet;

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    if (g_log_quiet && strcmp(level, "info") == 0)
        return;
    fprintf(stderr, "[evtrill] %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(...) logf_("info",  __VA_ARGS__)
#define LOG_WARN(...) logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)  logf_("error", __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Signals                                                                   */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* A pair's mode (see machine_apply). TRILL, LAST and ANALOG are the config's
 * `trill`, `last` and `analog`. INDEPENDENT is not a mode a config can
 * name: it is an analog pair's state while its latch is disarmed. */
enum {
    MODE_TRILL       = 0, /* both held: every stroke or release is a note */
    MODE_LAST        = 1, /* the newest stroke wins */
    MODE_INDEPENDENT = 2, /* each key on its own; never configured */
    MODE_ANALOG      = 3, /* independent until planted, then trill */
};

static const char *mode_name(int m) {
    switch (m) {
    case MODE_TRILL: return "trill";
    case MODE_LAST: return "last";
    case MODE_ANALOG: return "analog";
    default:          return "independent";
    }
}

/* Analog mode tuning. Depths are millimetres of travel; see the banner in
 * the analog section for what each threshold gates. */
typedef struct {
    char  *device;        /* explicit /dev/hidrawN, NULL == auto-discover */
    float  travel_mm;     /* full key travel, for normalized -> mm */
    float  actuation_mm;  /* press threshold */
    float  release_mm;    /* DERIVED, not configured: actuation_mm scaled by
                           * ANALOG_RELEASE_RATIO. Hysteresis is not a taste
                           * - it only has to be far enough below actuation
                           * that a resting finger cannot straddle both, and
                           * far enough above the firmware's reporting floor
                           * that a key at rest still counts as released.
                           * Scaling beats a fixed offset because actuation
                           * ranges from 0.1mm to most of the travel, and a
                           * subtraction that suits one end goes negative at
                           * the other. */
    /* backplate_mm defines the backplate, and the backplate is the whole
     * gesture: a plant is what arms the latch and hands k1/k2 to the
     * trill. It does triple duty, deliberately - one number, so there is
     * one place to tune how far down "planted" means: the independent
     * bottom-out re-press, the latch threshold, and the only threshold in
     * force while armed. */
    int    rt_enabled;
    /* Rapid trigger runs only while INDEPENDENT. While the latch is armed
     * the backplate is the switch and neither reversal distance runs at
     * all - rocking bottoms out every stroke by definition, so every note
     * in a burst lands on the same hard physical line. */
    float  rt_press_mm;   /* downward reversal that re-presses */
    float  rt_release_mm; /* upward reversal that releases */
    float  backplate_mm;  /* within this of full travel == on the
                           * backplate: presses regardless of rt_press_mm,
                           * and, for both keys at once, arms the latch */
    float  latch_debounce_ms; /* how long BOTH keys must stay on the
                               * backplate before the latch arms. 0 == the
                               * first report with both planted arms it.
                               * Disarming is never debounced. */
} analog_config_t;

/* One pair: two keys run by one state machine, under its own mode.
 * A config carries up to MAX_PAIRS of them, all on the one
 * virtual keyboard and all independent - a pair never sees another pair's
 * keys, which is the point (taiko: dons on one pair, kats on the other, a
 * don and a kat together are two notes, not a rock). The analog thresholds
 * are NOT per pair: they describe the board, and there is one board. */
#define MAX_PAIRS 8

typedef struct {
    int k1, k2;
    int mode;             /* the pair\'s mode */
    int hid_k1;           /* HID usage override; -1 == derive from the keymap */
    int hid_k2;
} keypair_t;

typedef struct {
    char   **device_paths;
    size_t   n_devices;
    int      auto_discover; /* no 'devices' list: scan for matching keyboards */
    keypair_t pair[MAX_PAIRS];
    int      n_pairs;
    char    *uinput_name;
    analog_config_t analog;
} oid_config_t;

/* Any pair in analog mode? Opening the board, the debounce timer and the
 * analog config check all hang off this rather than one global mode. */
static int cfg_any_analog(const oid_config_t *c) {
    for (int i = 0; i < c->n_pairs; i++)
        if (c->pair[i].mode == MODE_ANALOG)
            return 1;
    return 0;
}

/* The pair a -T trace records and `replay` replays: the first analog pair,
 * or pair 0 when none is analog. A trace stays two keys wide - the corpus
 * format - so with several pairs it is one pair's session, not all of them. */
static int cfg_trace_pair(const oid_config_t *c) {
    for (int i = 0; i < c->n_pairs; i++)
        if (c->pair[i].mode == MODE_ANALOG)
            return i;
    return 0;
}

static void config_init(oid_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->n_pairs        = 1;
    c->pair[0].k1     = DEF_K1;
    c->pair[0].k2     = DEF_K2;
    c->pair[0].mode   = MODE_TRILL;
    c->pair[0].hid_k1 = -1;   /* sentinel: derive from the keymap */
    c->pair[0].hid_k2 = -1;
    c->analog.travel_mm     = 4.0f;
    c->analog.actuation_mm  = 1.0f;
    c->analog.rt_enabled    = 1;
    c->analog.rt_press_mm   = 0.3f;
    c->analog.rt_release_mm = 0.3f;
    c->analog.backplate_mm = 0.20f;  /* 95% of the 4mm default travel */
}

static void config_free(oid_config_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_devices; i++)
        free(c->device_paths[i]);
    free(c->device_paths);
    free(c->uinput_name);
    free(c->analog.device);
    memset(c, 0, sizeof(*c));
}

/* libyaml document-API helpers --------------------------------------------- */

static yaml_node_t* ynode(yaml_document_t *doc, int id) {
    return id ? yaml_document_get_node(doc, id) : NULL;
}

/* Look up a scalar key in a mapping node. Returns the value node or NULL. */
static yaml_node_t* map_get(yaml_document_t *doc, yaml_node_t *map, const char *key) {
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        if (strcmp((const char *)k->data.scalar.value, key) == 0)
            return ynode(doc, p->value);
    }
    return NULL;
}

/* The schema is closed: every mapping in the config is checked against the
 * keys it may hold, and anything else is an error, as is a key given twice
 * (libyaml keeps both and map_get would silently take the first). A typo
 * like `pair:` for `pairs:` must stop the daemon, not run it on defaults.
 * `allowed` is NULL-terminated; `where` names the mapping in the message
 * ("" for the root). */
static int map_check(yaml_document_t *doc, yaml_node_t *map,
                     const char *where, const char *const *allowed) {
    int bad = 0;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) {
            LOG_ERR("'%s' has a key that is not a plain string",
                    *where ? where : "config");
            bad = 1;
            continue;
        }
        const char *name = (const char *)k->data.scalar.value;
        int known = 0;
        for (const char *const *a = allowed; *a; a++)
            if (strcmp(*a, name) == 0) { known = 1; break; }
        if (!known) {
            LOG_ERR("unknown key '%s%s%s'", where, *where ? "." : "", name);
            bad = 1;
            continue;
        }
        for (yaml_node_pair_t *q = map->data.mapping.pairs.start; q < p; q++) {
            yaml_node_t *qk = ynode(doc, q->key);
            if (qk && qk->type == YAML_SCALAR_NODE &&
                strcmp((const char *)qk->data.scalar.value, name) == 0) {
                LOG_ERR("key '%s%s%s' is given more than once",
                        where, *where ? "." : "", name);
                bad = 1;
                break;
            }
        }
    }
    return bad ? -1 : 0;
}

static int scalar_dup(yaml_node_t *n, char **out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    char *s = strdup((const char *)n->data.scalar.value);
    if (!s) return -1;
    free(*out);
    *out = s;
    return 0;
}

static int parse_bool(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")    || !strcmp(s, "1"))    { *out = 1; return 0; }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")   || !strcmp(s, "0"))    { *out = 0; return 0; }
    return -1;
}

/* Parse a non-negative float. Rejects garbage and negatives. */
static int parse_nonneg(yaml_node_t *n, float *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || d < 0.0)
        return -1;
    *out = (float)d;
    return 0;
}

/* Hysteresis as a fraction of actuation, and the shallowest derived
 * release worth trusting: a Wooting's firmware stops reporting a key a few
 * hundredths of a millimetre from rest, and a release threshold underneath
 * that would never fire. */
#define ANALOG_RELEASE_RATIO 0.8f
#define ANALOG_RELEASE_FLOOR 0.04f

static void analog_config_resolve(analog_config_t *a) {
    a->release_mm = a->actuation_mm * ANALOG_RELEASE_RATIO;
}

/* Thresholds that are individually well-formed can still combine into a
 * broken machine, and the failure modes are hard to attribute while playing:
 * a key that self-oscillates at the report rate, or one whose latch can
 * never clear. Reject those outright rather than let them reach the RT path.
 * Only enforced with an analog pair - an unused analog block stays harmless. */
static int analog_config_check(const analog_config_t *a) {
    int bad = 0;

    if (a->travel_mm <= 0.0f) {
        LOG_ERR("'analog.travel_mm' must be greater than 0");
        bad = 1;
    }
    if (a->actuation_mm <= 0.0f) {
        LOG_ERR("'analog.actuation_mm' must be greater than 0");
        bad = 1;
    }
    if (a->rt_enabled && a->rt_press_mm <= 0.0f) {
        LOG_ERR("'analog.rapid_trigger.press_mm' must be greater than 0");
        bad = 1;
    }
    if (a->rt_enabled && a->rt_release_mm <= 0.0f) {
        LOG_ERR("'analog.rapid_trigger.release_mm' must be greater than 0, "
                "otherwise a held key releases itself");
        bad = 1;
    }
    /* The backplate is load-bearing now: it defines the gesture, not just
     * a rapid-trigger corner case. */
    if (a->backplate_mm <= 0.0f) {
        LOG_ERR("'analog.backplate_mm' must be greater than "
                "0: it defines where the backplate starts, and both keys "
                "reaching it is what arms the latch");
        bad = 1;
    }
    /* Actuation has to sit above the backplate: with the line at or past
     * the backplate a key could reach the backplate without ever having
     * actuated, so independent would never press it, and with it past
     * travel_mm the key could never actuate at all. Both are silent
     * bricks. */
    if (a->travel_mm > 0.0f && a->backplate_mm > 0.0f &&
        a->actuation_mm >= a->travel_mm - a->backplate_mm) {
        LOG_ERR("'analog.actuation_mm' (%.3f) must be shallower than the "
                "backplate at %.3fmm (travel_mm - backplate_mm), otherwise "
                "a key can reach the backplate without ever actuating",
                (double)a->actuation_mm,
                (double)(a->travel_mm - a->backplate_mm));
        bad = 1;
    }
    if (a->backplate_mm >= a->travel_mm) {
        LOG_ERR("'analog.backplate_mm' (%.3f) must be less "
                "than travel_mm (%.3f): every sample would count as "
                "bottomed",
                (double)a->backplate_mm, (double)a->travel_mm);
        bad = 1;
    }
    if (bad)
        return -1;

    /* Survivable, but almost certainly not what was meant. */
    if (a->release_mm < ANALOG_RELEASE_FLOOR)
        LOG_WARN("'analog.actuation_mm' (%.3f) puts the derived release at "
                 "%.3fmm, under the ~%.2fmm the firmware stops reporting at: "
                 "a resting key may never count as released",
                 (double)a->actuation_mm, (double)a->release_mm,
                 (double)ANALOG_RELEASE_FLOOR);
    /* A backplate under 90% of travel is reachable by a firm tap, and a tap
     * that reaches it is a tap that can arm the latch - which is exactly the
     * alt-tapping misfire the latch exists to avoid. */
    if (a->travel_mm > 0.0f && a->backplate_mm > 0.10f * a->travel_mm)
        LOG_WARN("'analog.backplate_mm' (%.3f) puts the "
                 "backplate at %.1f%% of travel: below ~90%% a firm stroke "
                 "can reach it, and an overlap while alt-tapping could arm "
                 "the latch. Record a session with -T and check it with `replay`",
                 (double)a->backplate_mm,
                 (double)((a->travel_mm - a->backplate_mm)
                          / a->travel_mm * 100.0f));
    return 0;
}

/* Resolve a key code from a scalar - either a symbolic name ("KEY_Z") or
 * a decimal/hex integer. Returns 0 on success. */
static int parse_key_code(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;

    /* Try numeric first. */
    if (s[0] != '\0') {
        char *end = NULL;
        long v = strtol(s, &end, 0);
        if (end && *end == '\0' && end != s &&
            v >= 0 && v < KEY_MAX) {
            *out = (int)v;
            return 0;
        }
    }

    int code = libevdev_event_code_from_name(EV_KEY, s);
    if (code < 0) return -1;
    *out = code;
    return 0;
}

/* mode: "trill", "last" or "analog". */
#define MODE_VALUES "\"trill\", \"last\" or \"analog\""
static int parse_mode(yaml_node_t *n, int *out) {
    const char *s = n && n->type == YAML_SCALAR_NODE
                    ? (const char *)n->data.scalar.value : "";
    if (!strcmp(s, "trill"))
        *out = MODE_TRILL;
    else if (!strcmp(s, "last"))
        *out = MODE_LAST;
    else if (!strcmp(s, "analog"))
        *out = MODE_ANALOG;
    else
        return -1;
    return 0;
}

/* A sequence of exactly two scalars - a pair's `keys` and `hid`. */
static int parse_two(yaml_document_t *doc, yaml_node_t *n,
                     yaml_node_t *out[2]) {
    if (!n || n->type != YAML_SEQUENCE_NODE ||
        n->data.sequence.items.top - n->data.sequence.items.start != 2)
        return -1;
    for (int i = 0; i < 2; i++) {
        out[i] = ynode(doc, n->data.sequence.items.start[i]);
        if (!out[i] || out[i]->type != YAML_SCALAR_NODE)
            return -1;
    }
    return 0;
}

/* A HID usage override: "auto" (leave the -1 sentinel) or a usage id.
 * Usage 0 is HID's "no event" and never names a key. */
static int parse_hid(yaml_node_t *n, int *out) {
    const char *s = n && n->type == YAML_SCALAR_NODE
                    ? (const char *)n->data.scalar.value : "";
    if (!strcasecmp(s, "auto"))
        return 0;
    char *end = NULL;
    errno = 0;
    long u = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0 || u <= 0 || u > 0xFFFF)
        return -1;
    *out = (int)u;
    return 0;
}

/* One entry of `pairs:`. `keys` is required - there is no sensible default
 * for a pair's keys - `mode` falls back to the top-level one, and `hid`
 * pins the keys' HID usage ids for analog mode. */
static int parse_pair_entry(yaml_document_t *doc, yaml_node_t *map,
                            keypair_t *p, int idx, int def_mode) {
    char where[32];
    snprintf(where, sizeof(where), "pairs[%d]", idx);
    if (!map || map->type != YAML_MAPPING_NODE) {
        LOG_ERR("'%s' must be a mapping", where);
        return -1;
    }
    static const char *const allowed[] = { "keys", "mode", "hid", NULL };
    if (map_check(doc, map, where, allowed) != 0)
        return -1;
    p->mode   = def_mode;
    p->hid_k1 = p->hid_k2 = -1;

    yaml_node_t *two[2];
    yaml_node_t *kn = map_get(doc, map, "keys");
    if (!kn) {
        LOG_ERR("'%s.keys' is required", where);
        return -1;
    }
    if (parse_two(doc, kn, two) != 0) {
        LOG_ERR("'%s.keys' must be a list of two keys", where);
        return -1;
    }
    int *kout[2] = { &p->k1, &p->k2 };
    for (int i = 0; i < 2; i++)
        if (parse_key_code(two[i], kout[i]) != 0) {
            LOG_ERR("invalid key code for '%s.keys[%d]'", where, i);
            return -1;
        }

    yaml_node_t *mn = map_get(doc, map, "mode");
    if (mn && parse_mode(mn, &p->mode) != 0) {
        LOG_ERR("'%s.mode' must be " MODE_VALUES, where);
        return -1;
    }

    yaml_node_t *hn = map_get(doc, map, "hid");
    if (hn) {
        int *hout[2] = { &p->hid_k1, &p->hid_k2 };
        if (parse_two(doc, hn, two) != 0 ||
            parse_hid(two[0], hout[0]) != 0 ||
            parse_hid(two[1], hout[1]) != 0) {
            LOG_ERR("'%s.hid' must be a list of two HID usage ids, either "
                    "of which may be \"auto\"", where);
            return -1;
        }
    }
    return 0;
}

/* Pairs are independent only if they share nothing. A key in two pairs would
 * feed two machines, each releasing what the other holds - a silent
 * cross-talk bug, so it is refused. Within a pair the same holds: one key
 * cannot play both halves of a trill. */
static int pairs_check(const oid_config_t *c) {
    int bad = 0;
    for (int i = 0; i < c->n_pairs; i++) {
        const keypair_t *a = &c->pair[i];
        if (a->k1 == a->k2) {
            LOG_ERR("pair %d: both keys are the same key", i);
            bad = 1;
        }
        if (a->hid_k1 >= 0 && a->hid_k1 == a->hid_k2) {
            LOG_ERR("pair %d: both hid overrides are 0x%02x: one physical "
                    "key would play both halves of the pair",
                    i, a->hid_k1);
            bad = 1;
        }
        for (int j = i + 1; j < c->n_pairs; j++) {
            const keypair_t *b = &c->pair[j];
            /* Overrides only; a collision involving a keymap-derived usage
             * can only be seen once the board is open (analog_dev_open). */
            const int ha[2] = { a->hid_k1, a->hid_k2 };
            const int hb[2] = { b->hid_k1, b->hid_k2 };
            for (int x = 0; x < 2; x++)
                for (int y = 0; y < 2; y++)
                    if (ha[x] >= 0 && ha[x] == hb[y]) {
                        LOG_ERR("pairs %d and %d both override a key to hid "
                                "0x%02x", i, j, ha[x]);
                        bad = 1;
                    }
            if (a->k1 == b->k1 || a->k1 == b->k2 ||
                a->k2 == b->k1 || a->k2 == b->k2) {
                LOG_ERR("pairs %d and %d share a key", i, j);
                bad = 1;
            }
        }
    }
    return bad ? -1 : 0;
}

static int load_config(const char *path, oid_config_t *c) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    memset(&doc, 0, sizeof(doc));
    int rc = -1;
    int doc_loaded = 0;

    if (!yaml_parser_initialize(&parser)) {
        LOG_ERR("yaml_parser_initialize failed");
        fclose(fp);
        return -1;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML parse error in %s: %s (line %zu, col %zu)",
                path, parser.problem ? parser.problem : "unknown",
                (size_t)parser.problem_mark.line + 1,
                (size_t)parser.problem_mark.column + 1);
        goto out;
    }
    doc_loaded = 1;

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root) {
        LOG_ERR("config %s is empty", path);
        goto out;
    }
    if (root->type != YAML_MAPPING_NODE) {
        LOG_ERR("config root must be a mapping");
        goto out;
    }
    static const char *const root_keys[] = {
        "devices", "mode", "pairs", "analog", "uinput", NULL
    };
    if (map_check(&doc, root, "", root_keys) != 0)
        goto out;

    /* devices: optional sequence of scalar paths. Omitted (or the scalar
     * "auto") means auto-discovery: grab every keyboard-shaped device that
     * advertises every pair's keys (see auto_grab_ok). --------------- */
    yaml_node_t *devs = map_get(&doc, root, "devices");
    if (!devs ||
        (devs->type == YAML_SCALAR_NODE &&
         !strcasecmp((const char *)devs->data.scalar.value, "auto"))) {
        c->auto_discover = 1;
    } else if (devs->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'devices' must be a sequence of paths, or \"auto\"");
        goto out;
    } else {
        size_t n = (size_t)(devs->data.sequence.items.top -
                            devs->data.sequence.items.start);
        if (n == 0) {
            LOG_ERR("'devices' list is empty");
            goto out;
        }
        c->device_paths = calloc(n, sizeof(char *));
        if (!c->device_paths) { LOG_ERR("oom"); goto out; }
        for (yaml_node_item_t *it = devs->data.sequence.items.start;
             it < devs->data.sequence.items.top; it++) {
            yaml_node_t *item = ynode(&doc, *it);
            if (scalar_dup(item, &c->device_paths[c->n_devices]) != 0) {
                LOG_ERR("'devices' entry must be a scalar string");
                goto out;
            }
            c->n_devices++;
        }
    }

    /* mode: optional scalar - the default for every `pairs:` entry that
     * does not name its own. */
    int def_mode = MODE_TRILL;
    yaml_node_t *mode = map_get(&doc, root, "mode");
    if (mode && parse_mode(mode, &def_mode) != 0) {
        LOG_ERR("'mode' must be " MODE_VALUES);
        goto out;
    }

    /* pairs: required. There is no built-in pair. */
    yaml_node_t *pairs = map_get(&doc, root, "pairs");
    if (!pairs) {
        LOG_ERR("'pairs' is required: list at least one pair of keys");
        goto out;
    }
    if (pairs->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'pairs' must be a sequence of mappings");
        goto out;
    }
    size_t n_pairs = (size_t)(pairs->data.sequence.items.top -
                              pairs->data.sequence.items.start);
    if (n_pairs == 0 || n_pairs > MAX_PAIRS) {
        LOG_ERR("'pairs' must have between 1 and %d entries", MAX_PAIRS);
        goto out;
    }
    c->n_pairs = 0;
    for (yaml_node_item_t *it = pairs->data.sequence.items.start;
         it < pairs->data.sequence.items.top; it++) {
        if (parse_pair_entry(&doc, ynode(&doc, *it), &c->pair[c->n_pairs],
                             c->n_pairs, def_mode) != 0)
            goto out;
        c->n_pairs++;
    }

    /* analog: optional mapping */
    yaml_node_t *an = map_get(&doc, root, "analog");
    if (an) {
        if (an->type != YAML_MAPPING_NODE) {
            LOG_ERR("'analog' must be a mapping");
            goto out;
        }
        static const char *const analog_keys[] = {
            "device", "travel_mm", "actuation_mm", "backplate_mm",
            "latch_debounce_ms", "rapid_trigger", NULL
        };
        if (map_check(&doc, an, "analog", analog_keys) != 0)
            goto out;
        yaml_node_t *dv = map_get(&doc, an, "device");
        if (dv) {
            const char *s = dv->type == YAML_SCALAR_NODE
                            ? (const char *)dv->data.scalar.value : "";
            if (strcasecmp(s, "auto") != 0 &&
                scalar_dup(dv, &c->analog.device) != 0) {
                LOG_ERR("'analog.device' must be a scalar string");
                goto out;
            }
        }
        static const struct { const char *key; size_t off; } mm[] = {
            { "travel_mm",     offsetof(analog_config_t, travel_mm)     },
            { "actuation_mm",  offsetof(analog_config_t, actuation_mm)  },
            { "backplate_mm",  offsetof(analog_config_t, backplate_mm)  },
            { "latch_debounce_ms",
              offsetof(analog_config_t, latch_debounce_ms) },
        };
        for (size_t i = 0; i < sizeof(mm) / sizeof(mm[0]); i++) {
            yaml_node_t *v = map_get(&doc, an, mm[i].key);
            if (v && parse_nonneg(v, (float *)((char *)&c->analog +
                                              mm[i].off))) {
                LOG_ERR("'analog.%s' must be a non-negative number", mm[i].key);
                goto out;
            }
        }
        yaml_node_t *rt = map_get(&doc, an, "rapid_trigger");
        if (rt) {
            if (rt->type != YAML_MAPPING_NODE) {
                LOG_ERR("'analog.rapid_trigger' must be a mapping");
                goto out;
            }
            static const char *const rt_keys[] = {
                "enabled", "press_mm", "release_mm", NULL
            };
            if (map_check(&doc, rt, "analog.rapid_trigger", rt_keys) != 0)
                goto out;
            yaml_node_t *en = map_get(&doc, rt, "enabled");
            if (en && parse_bool(en, &c->analog.rt_enabled) != 0) {
                LOG_ERR("'analog.rapid_trigger.enabled' must be a boolean");
                goto out;
            }
            yaml_node_t *pm = map_get(&doc, rt, "press_mm");
            if (pm && parse_nonneg(pm, &c->analog.rt_press_mm) != 0) {
                LOG_ERR("'analog.rapid_trigger.press_mm' must be a "
                        "non-negative number");
                goto out;
            }
            yaml_node_t *rm = map_get(&doc, rt, "release_mm");
            if (rm && parse_nonneg(rm, &c->analog.rt_release_mm) != 0) {
                LOG_ERR("'analog.rapid_trigger.release_mm' must be a "
                        "non-negative number");
                goto out;
            }
        }
    }

    /* uinput: optional mapping */
    yaml_node_t *ui = map_get(&doc, root, "uinput");
    if (ui) {
        if (ui->type != YAML_MAPPING_NODE) {
            LOG_ERR("'uinput' must be a mapping");
            goto out;
        }
        static const char *const ui_keys[] = { "name", NULL };
        if (map_check(&doc, ui, "uinput", ui_keys) != 0)
            goto out;
        yaml_node_t *nm = map_get(&doc, ui, "name");
        if (nm && scalar_dup(nm, &c->uinput_name) != 0) {
            LOG_ERR("'uinput.name' must be a scalar string");
            goto out;
        }
    }
    if (!c->uinput_name) {
        c->uinput_name = strdup(DEF_NAME);
        if (!c->uinput_name) { LOG_ERR("oom"); goto out; }
    }

    if (pairs_check(c) != 0)
        goto out;
    analog_config_resolve(&c->analog);
    if (cfg_any_analog(c) && analog_config_check(&c->analog) != 0)
        goto out;

    rc = 0;

out:
    if (doc_loaded) yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    if (rc != 0) config_free(c);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Input devices                                                             */
/* ------------------------------------------------------------------------- */

/* epoll user-data tag. The inotify fd is still identified by a NULL ptr;
 * everything else leads with one of these so run_loop can tell an evdev
 * device from the analog hidraw node. */
enum { EP_INPUT = 1, EP_ANALOG = 2, EP_LATCH = 3 };

/* The latch-debounce timerfd has no struct of its own; the epoll set is
 * keyed on a leading discriminator, so it gets a standing one (see
 * latch_timer_sync). */
static int g_ep_latch = EP_LATCH;

/* One pair's state machine for one input source. Per-source, not global:
 * several keyboards are tracked independently even though they share one
 * virtual keyboard, and the analog path carries its own instance too. */
typedef struct {
    int k1;   /* k1 down? */
    int k2;   /* k2 down? */
    int act;  /* 1 == k1 active, 0 == k2 (or none) */
    int mode; /* mode of the last edge applied; release_stuck needs the
               * EFFECTIVE mode, which for the analog path is not cfg->mode */
} machine_t;

typedef struct {
    int               kind;    /* EP_INPUT */
    struct libevdev  *dev;
    int               fd;
    char             *path;
    dev_t             rdev; /* st_rdev, dedupes nodes reached via symlinks */
    int               grabbed; /* 0 = open but grab deferred (keys held) */
    int               analog;  /* this is the analog board: its analog-mode
                                * pairs come from hidraw, so drop their
                                * digital copies */
    machine_t      machine[MAX_PAIRS]; /* one machine per pair, as configured */
} input_dev_t;

/* Grabbed devices as stable heap pointers: epoll user data points at the
 * entries, so the list may grow and shrink but entries never move. */
typedef struct {
    input_dev_t **v;
    size_t        n, cap;
} dev_list_t;

static int dev_list_add(dev_list_t *l, input_dev_t *in) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        input_dev_t **v = realloc(l->v, cap * sizeof(*v));
        if (!v) return -1;
        l->v   = v;
        l->cap = cap;
    }
    l->v[l->n++] = in;
    return 0;
}

static void dev_list_remove(dev_list_t *l, input_dev_t *in) {
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i] == in) {
            l->v[i] = l->v[--l->n];
            return;
        }
    }
}

static int dev_list_has_rdev(const dev_list_t *l, dev_t rdev) {
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i]->rdev == rdev) return 1;
    return 0;
}

/* Vendor/product of the keyboard whose k1/k2 the analog path owns, or 0
 * when nothing does. Devices matching it get in->analog set, which makes
 * process_event drop their digital k1/k2 so each press is emitted once. */
static uint16_t g_analog_vid, g_analog_pid;

/* Never grab an evtrill output device. Prevents feedback loops. */
static int is_evtrill_output(struct libevdev *dev, const oid_config_t *cfg) {
    const char *uniq = libevdev_get_uniq(dev);
    const char *name = libevdev_get_name(dev);
    if (uniq && strcmp(uniq, "evtrill") == 0) return 1;
    if (name && strcmp(name, cfg->uinput_name) == 0) return 1;
    return 0;
}

/* Auto-discovery filter: keyboard-shaped devices that advertise every
 * physical key of every pair. Devices with pointer/absolute axes are
 * rejected because the virtual device is key-only. Virtual devices
 * (ours, keyd's, ...) are rejected to stay loop-free among remappers. */
static int auto_grab_ok(struct libevdev *dev, const oid_config_t *cfg) {
    if (libevdev_get_id_bustype(dev) == BUS_VIRTUAL) return 0;
    for (int i = 0; i < cfg->n_pairs; i++)
        if (!libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->pair[i].k1) ||
            !libevdev_has_event_code(dev, EV_KEY, (unsigned)cfg->pair[i].k2))
            return 0;
    if (libevdev_has_event_type(dev, EV_REL) ||
        libevdev_has_event_type(dev, EV_ABS))
        return 0;
    return 1;
}

/* Any key physically down right now? libevdev syncs key state at init and
 * keeps it current as events are read, so this needs no extra ioctl. */
static int any_key_down(const struct libevdev *dev) {
    for (unsigned int code = 1; code <= KEY_MAX; code++)
        if (libevdev_get_event_value(dev, EV_KEY, code))
            return 1;
    return 0;
}

/* Open, vet, and grab one event node. Returns NULL (silently, for the
 * expected cases) when the device shouldn't or can't be grabbed. */
static input_dev_t *input_try_open(const char *path, const oid_config_t *cfg,
                                   int auto_mode, int quiet) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* EACCES right after a hotplug is expected - udev hasn't applied
         * the input-group permissions yet; the IN_ATTRIB watch retriggers
         * reconcile_devices once it does. */
        if (!quiet)
            LOG_ERR("open(%s): %s", path, strerror(errno));
        else if (errno != EACCES && errno != ENOENT)
            LOG_WARN("open(%s): %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        if (!quiet)
            LOG_ERR("libevdev_new_from_fd(%s): %s", path, strerror(-rc));
        close(fd);
        return NULL;
    }

    if (is_evtrill_output(dev, cfg) ||
        (auto_mode && !auto_grab_ok(dev, cfg))) {
        libevdev_free(dev);
        close(fd);
        return NULL;
    }

    /* Grabbing while a key is physically down would swallow its release:
     * the press already reached the display server through the raw device,
     * but the release would arrive only here and be re-emitted on the
     * virtual keyboard, leaving the raw key logically stuck (typing the
     * Enter that starts the daemon is enough to hit this). Defer instead:
     * keep the fd open ungrabbed, discard its events (the system still
     * gets them directly), and let drain_device finish the grab once
     * every key is up. */
    int defer = any_key_down(dev);
    if (!defer) {
        rc = libevdev_grab(dev, LIBEVDEV_GRAB);
        if (rc < 0) {
            LOG_WARN("libevdev_grab(%s \"%s\"): %s", path,
                     libevdev_get_name(dev) ? libevdev_get_name(dev) : "?",
                     strerror(-rc));
            libevdev_free(dev);
            close(fd);
            return NULL;
        }
    }

    input_dev_t *in = calloc(1, sizeof(*in));
    if (!in) {
        LOG_ERR("oom");
        if (!defer) libevdev_grab(dev, LIBEVDEV_UNGRAB);
        libevdev_free(dev);
        close(fd);
        return NULL;
    }
    in->kind    = EP_INPUT;
    in->analog  = g_analog_vid &&
                  libevdev_get_id_vendor(dev)  == (int)g_analog_vid &&
                  libevdev_get_id_product(dev) == (int)g_analog_pid;
    in->fd      = fd;
    in->dev     = dev;
    in->path    = strdup(path);
    in->rdev    = st.st_rdev;
    in->grabbed = !defer;
    if (defer)
        LOG_INFO("Opened %s (\"%s\") - keys held, deferring grab until "
                 "all released", path,
                 libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    else
        LOG_INFO("Opened and grabbed %s (\"%s\")",
                 path, libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    return in;
}

static void input_close(input_dev_t *in) {
    if (!in) return;
    if (in->dev) {
        libevdev_grab(in->dev, LIBEVDEV_UNGRAB);
        libevdev_free(in->dev);
        in->dev = NULL;
    }
    if (in->fd >= 0) {
        close(in->fd);
        in->fd = -1;
    }
    free(in->path);
    in->path = NULL;
}

static int is_event_node(const struct dirent *d) {
    return strncmp(d->d_name, "event", 5) == 0;
}

static void try_grab(dev_list_t *devs, const char *path,
                     const oid_config_t *cfg, int auto_mode, int loud,
                     int epfd) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (loud && !auto_mode)
            LOG_WARN("stat(%s): %s", path, strerror(errno));
        return;
    }
    if (!S_ISCHR(st.st_mode))
        return;
    if (dev_list_has_rdev(devs, st.st_rdev))
        return; /* already grabbed (possibly via another path/symlink) */

    input_dev_t *in = input_try_open(path, cfg, auto_mode, !loud);
    if (!in)
        return;

    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = in } };
    if (dev_list_add(devs, in) != 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD, in->fd, &ev) < 0) {
        LOG_ERR("failed to register %s: %s", path, strerror(errno));
        dev_list_remove(devs, in);
        input_close(in);
        free(in);
    }
}

/* (Re)open whatever should be grabbed but currently isn't: every configured
 * path in explicit mode, every matching event node under input_dir in auto
 * mode. Runs at startup (loud) and again on every inotify event under
 * input_dir (quiet - the same non-matching nodes get revisited each time). */
static void reconcile_devices(dev_list_t *devs, const oid_config_t *cfg,
                              const char *input_dir, int epfd, int loud) {
    if (!cfg->auto_discover) {
        for (size_t i = 0; i < cfg->n_devices; i++)
            try_grab(devs, cfg->device_paths[i], cfg, 0, loud, epfd);
        return;
    }

    struct dirent **ents = NULL;
    int n = scandir(input_dir, &ents, is_event_node, alphasort);
    if (n < 0) {
        LOG_WARN("scandir(%s): %s", input_dir, strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", input_dir, ents[i]->d_name);
        try_grab(devs, path, cfg, 1, loud, epfd);
        free(ents[i]);
    }
    free(ents);
}

/* ------------------------------------------------------------------------- */
/* Analog input (hidraw)                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Analog keyboards report per-key travel depth on a vendor-defined HID
 * interface, separate from the keyboard interface evdev binds to. The
 * kernel creates no input node for a vendor usage page, so that interface
 * exists purely as a /dev/hidrawN character device: a pollable fd that
 * drops straight into the epoll set in run_loop(), and one that EVIOCGRAB
 * on the keyboard node has no bearing on. We can grab the keyboard and
 * read its analog stream at the same time.
 *
 * Two report formats are in the wild:
 *
 *   v2 (usage page 0xFF53) - 64 bytes, sixteen 4-byte slots, 10-bit depth
 *       [matrix_pos][key][packed][value]
 *   v1 (usage page 0xFF54) - 48 bytes, sixteen 3-byte slots, 8-bit depth
 *       [code_hi][code_lo][value]
 *
 * Both carry only keys that are off their rest position. A key that has
 * just been released appears once with a zero value and then vanishes, so
 * "absent from the report" means "at rest" - see analog_feed().
 */

#define WOOTING_VID         0x31E3
#define WOOTING_VID_LEGACY  0x03EB

#define ANALOG_SLOTS        16
#define ANALOG_V1_REPORT    (ANALOG_SLOTS * 3)  /* 48 */
#define ANALOG_V2_REPORT    (ANALOG_SLOTS * 4)  /* 64 */
#define ANALOG_DESC_MAX     4096

enum { ANALOG_FMT_NONE = 0, ANALOG_FMT_V1, ANALOG_FMT_V2 };

/* One decoded key from an analog report. */
typedef struct {
    uint16_t code;   /* HID usage id; namespace 0 == ordinary keyboard key */
    float    depth;  /* 0.0 at rest .. 1.0 bottomed out */
} analog_sample_t;

/* HID_ID=0003:000031E3:00001320 -> bus, vendor, product. */
static int analog_read_ids(const char *sysdir, uint16_t *vid, uint16_t *pid) {
    char path[PATH_MAX], line[256];
    int  found = -1;

    int n = snprintf(path, sizeof(path), "%s/device/uevent", sysdir);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned bus, v, p;
        if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &v, &p) == 3) {
            *vid = (uint16_t)v;
            *pid = (uint16_t)p;
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

/* The analog interface is identified by its vendor usage page, which uevent
 * doesn't expose - so read the report descriptor and look for a Usage Page
 * item (0x06, two-byte payload) carrying 0xFF53 or 0xFF54. This is a byte
 * scan rather than a real HID item walk; paired with the vendor check in
 * analog_scan() that's specific enough, and it keeps this to a few lines. */
static int analog_probe_fmt(const char *sysdir) {
    char          path[PATH_MAX];
    unsigned char buf[ANALOG_DESC_MAX];

    snprintf(path, sizeof(path), "%s/device/report_descriptor", sysdir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ANALOG_FMT_NONE;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    for (ssize_t i = 0; i + 2 < n; i++) {
        if (buf[i] != 0x06) continue;
        if (buf[i + 1] == 0x53 && buf[i + 2] == 0xFF) return ANALOG_FMT_V2;
        if (buf[i + 1] == 0x54 && buf[i + 2] == 0xFF) return ANALOG_FMT_V1;
    }
    return ANALOG_FMT_NONE;
}

/* Decode one raw report into samples; returns the number of live slots.
 * Slots that are entirely empty are skipped, but a key reporting zero is
 * kept - that's the one-shot release notification. */
static int analog_decode(int fmt, const unsigned char *buf, size_t len,
                         analog_sample_t *out, size_t max) {
    size_t n = 0;

    if (fmt == ANALOG_FMT_V2) {
        for (size_t i = 0; i + 4 <= len && n < max; i += 4) {
            unsigned key    = buf[i + 1];
            unsigned packed = buf[i + 2];
            unsigned value  = buf[i + 3];
            unsigned ns     = (packed >> 2) & 0x0F;
            unsigned lo     = (packed >> 6) & 0x03;
            unsigned v10    = (value << 2) | lo;   /* 10-bit, 0..1023 */
            if (!key && !v10) continue;            /* empty slot */
            out[n].code  = (uint16_t)((ns << 8) | key);
            out[n].depth = v10 >= 1023 ? 1.0f : (float)v10 / 1023.0f;
            n++;
        }
    } else if (fmt == ANALOG_FMT_V1) {
        for (size_t i = 0; i + 3 <= len && n < max; i += 3) {
            uint16_t code = (uint16_t)((buf[i] << 8) | buf[i + 1]);
            unsigned v    = buf[i + 2];
            if (!code) continue;                   /* empty slot */
            out[n].code  = code;
            out[n].depth = v >= 255 ? 1.0f : (float)v / 255.0f;
            n++;
        }
    }
    return (int)n;
}

/*
 * Analog front-end for one physical key: turns a stream of travel-depth
 * samples (mm) into the same press/release edges the digital evdev path
 * feeds the state machine.
 *
 * Two thresholds instead of one is the whole feature, but they answer
 * different questions. actuation_mm means "this is a keypress" and is the
 * only thing that decides whether an edge is emitted here. The backplate
 * (travel_mm - backplate_mm) means "this key is on the backplate", and
 * this function uses it for one thing only: the bottom-out re-press rule.
 * Whether the PAIR of keys is on the backplate is analog_latch_update's
 * business, and it reads the two depths directly rather than any latch
 * kept here - which is why this struct holds no commitment flag.
 *
 * `rt_extreme` is continuous across rapid-trigger edges: a rapid-trigger release
 * does not zero it, so a micro-reversal re-press measures its travel from
 * the right anchor rather than a stale actuation point. Since every path
 * that sets `live` also anchors `rt_extreme` to a real sample depth
 * (always > 0), "engaged but not yet fully released" reads straight off
 * `rt_extreme != 0.0f` with no extra flag - and rule 1 is the only place
 * that zeroes it.
 *
 * Ordering within one sample, which is what makes a contradictory
 * press+release pair for one key impossible:
 *
 *   1. Full release (depth < release_mm) is a master reset. It overrides
 *      everything else unconditionally - the key left the switch - emits
 *      the release if one is owed, and returns.
 *   2. Otherwise, if the key is still engaged (rt_extreme != 0) and rapid
 *      trigger is on, ONLY the reversal check runs. A sample that both
 *      reaches the backplate and constitutes a reversal thus resolves
 *      unambiguously as the reversal; an engaged key never re-runs the
 *      fresh-actuation logic.
 *   3. Otherwise (never actuated this engagement) the fresh-actuation
 *      check runs instead.
 *
 * Every branch that flips `live` is the branch that appends the matching
 * edge, so `live` cannot desynchronize from what was emitted - machine_apply
 * downstream assumes strict press/release alternation per key.
 */
typedef struct {
    int   live;        /* an edge has been emitted; key counts as pressed */
    float rt_extreme;  /* local extreme for rapid trigger, mm */
} analog_key_t;

typedef struct {
    analog_key_t key[2];   /* [0] == k1, [1] == k2 */
} analog_state_t;

/* Feed one sample for one key. idx is 0 (k1) or 1 (k2); a key absent from
 * the hardware report is fed depth_mm == 0.0f. Writes at most ONE edge to
 * out[] (1 == press, 0 == release) and returns the count: rule 1 returns
 * early and rules 2/3 are mutually exclusive, so no path emits two. The
 * array is kept at two so callers need not change if that ever does. */
static int analog_key_feed(analog_state_t *st, int idx, float depth_mm,
                           const analog_config_t *cfg, int armed, int *out) {
    analog_key_t *self = &st->key[idx];
    int           n    = 0;

    /* 0. Armed: the backplate IS the switch, and rapid trigger does not run
     * at all.
     *
     * This is the whole simplification. While the latch is armed the player
     * is bottoming out every stroke by definition - that is what rocking is -
     * so the only threshold that means anything is the backplate.
     *
     * No hysteresis is needed and none is wanted: the backplate is a
     * physical stop, so a finger is either against it (reading full travel)
     * or off it.
     *
     * `rt_extreme` is kept at the current depth purely so the handoff OUT of
     * the armed state has a sane anchor - see analog_latch_set, which clears
     * `live` and leaves this parked, putting the key in the "engaged but not
     * live" state the independent rules understand. */
    if (armed) {
        int on = depth_mm >= cfg->travel_mm - cfg->backplate_mm;
        self->rt_extreme = depth_mm;
        if (on != self->live) {
            self->live = on;
            out[n++]   = on;
        }
        return n;
    }

    /* 1. Full release: master reset. The only place that zeroes
     * `rt_extreme`. */
    if (depth_mm < cfg->release_mm) {
        if (self->live)
            out[n++] = 0;
        self->live       = 0;
        self->rt_extreme = 0.0f;
        return n;
    }

    /* Only an independent key reaches here: rule 0 returned for every
     * armed report, so there is one set of distances and nothing selects
     * between them. */
    float press   = cfg->rt_press_mm;
    float release = cfg->rt_release_mm;

    /* Against the backplate: the re-press rule below. The same line is
     * what analog_latch_update tests for BOTH keys at once, but it reads
     * the depths itself - nothing about the pair is decided here. */
    int bottomed = depth_mm >= cfg->travel_mm - cfg->backplate_mm;

    if (cfg->rt_enabled && self->rt_extreme != 0.0f) {
        /* 2. Already engaged: rapid trigger owns this sample exclusively,
         * tracking the local extreme and firing on a large enough
         * reversal in either direction. A zero distance disables that
         * zone's edge outright - the key then holds until rule 1. */
        if (self->live) {
            if (depth_mm > self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (release > 0.0f &&
                       self->rt_extreme - depth_mm >= release) {
                out[n++]         = 0;
                self->live       = 0;
                self->rt_extreme = depth_mm;
            }
        } else {
            /* Bottoming out always presses, however small the down-travel
             * since the last reversal - the mirror of rule 1, where a full
             * release always releases however small the up-travel. Resting on
             * the backplate as the neutral position depends on this: with a
             * large press distance, a stroke that ends against the bottom
             * never travels far enough to satisfy the reversal test on its
             * own. (While ARMED this no longer applies at all - rule 0
             * returns before any of this.) */
            /* A re-press still has to clear actuation_mm: without that
             * floor, a reversal entirely within the top fraction of travel
             * synthesizes whole keystrokes where the sensor is least
             * linear. */
            if (depth_mm < self->rt_extreme) {
                self->rt_extreme = depth_mm;
            } else if (depth_mm >= cfg->actuation_mm &&
                       (bottomed ||
                        (press > 0.0f &&
                         depth_mm - self->rt_extreme >= press))) {
                out[n++]         = 1;
                self->live       = 1;
                self->rt_extreme = depth_mm;
            }
        }
    } else if (!self->live && depth_mm >= cfg->actuation_mm) {
        /* 3. Fresh actuation. */
        self->live       = 1;
        self->rt_extreme = depth_mm;
        out[n++]         = 1;
    }

    return n;
}


/* Find the analog interface of an analog-capable keyboard. Returns the fd
 * (and fills path/fmt), or -1 when there's nothing to open. An explicit
 * path skips the vendor check but still has to look like an analog node.
 * `quiet` silences every failure: the reconnect retry runs on each rescan
 * of the input dir, and a board that is still unplugged is not news. */
static int analog_open(const char *forced, char *path_out, size_t path_sz,
                       int *fmt_out, uint16_t *vid_out, uint16_t *pid_out,
                       int quiet) {
    if (forced) {
        const char *base = strrchr(forced, '/');
        char sysdir[PATH_MAX];
        snprintf(sysdir, sizeof(sysdir), "/sys/class/hidraw/%s",
                 base ? base + 1 : forced);
        int fmt = analog_probe_fmt(sysdir);
        if (fmt == ANALOG_FMT_NONE) {
            if (!quiet)
                LOG_ERR("%s is not an analog interface "
                        "(no 0xFF53/0xFF54 usage page)", forced);
            return -1;
        }
        int fd = open(forced, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (!quiet)
                LOG_ERR("open(%s): %s", forced, strerror(errno));
            return -1;
        }
        analog_read_ids(sysdir, vid_out, pid_out);
        snprintf(path_out, path_sz, "%s", forced);
        *fmt_out = fmt;
        return fd;
    }

    struct dirent **ents;
    int n = scandir("/sys/class/hidraw", &ents, NULL, alphasort);
    if (n < 0) return -1;

    int fd = -1;
    for (int i = 0; i < n; i++) {
        if (fd < 0 && strncmp(ents[i]->d_name, "hidraw", 6) == 0) {
            char     sysdir[PATH_MAX], dev[PATH_MAX];
            uint16_t vid = 0, pid = 0;

            snprintf(sysdir, sizeof(sysdir), "/sys/class/hidraw/%s",
                     ents[i]->d_name);
            if (analog_read_ids(sysdir, &vid, &pid) == 0 &&
                (vid == WOOTING_VID || vid == WOOTING_VID_LEGACY)) {
                int fmt = analog_probe_fmt(sysdir);
                if (fmt != ANALOG_FMT_NONE) {
                    snprintf(dev, sizeof(dev), "/dev/%s", ents[i]->d_name);
                    int f = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                    if (f < 0) {
                        if (!quiet)
                            LOG_WARN("open(%s): %s - analog unavailable "
                                     "(udev rules? see README)",
                                     dev, strerror(errno));
                    } else {
                        fd = f;
                        *fmt_out = fmt;
                        *vid_out = vid;
                        *pid_out = pid;
                        snprintf(path_out, path_sz, "%s", dev);
                    }
                }
            }
        }
        free(ents[i]);
    }
    free(ents);
    return fd;
}

/* hid-input stores a key's HID usage as its evdev scancode (usage page 0x07
 * for ordinary keyboard keys), so the keyboard itself carries the mapping
 * between config KEY_* codes and analog report usage ids. Walking its keymap
 * beats hardcoding a table: it stays correct across layouts and firmware. */
#define HID_PAGE_KEYBOARD 0x07

static int hid_map_build(int fd, uint16_t *usage_of_key) {
    int found = 0;
    for (unsigned idx = 0; idx < KEY_MAX * 2; idx++) {
        struct input_keymap_entry e;
        uint32_t sc;

        memset(&e, 0, sizeof(e));
        e.flags = INPUT_KEYMAP_BY_INDEX;
        e.index = idx;
        if (ioctl(fd, EVIOCGKEYCODE_V2, &e) < 0)
            break;                    /* EINVAL == walked off the end */
        if (e.len != sizeof(sc) || e.keycode > KEY_MAX)
            continue;
        memcpy(&sc, e.scancode, sizeof(sc));
        if ((sc >> 16) != HID_PAGE_KEYBOARD)
            continue;
        usage_of_key[e.keycode] = (uint16_t)(sc & 0xFFFF);
        found++;
    }
    return found;
}

/* Walk every keyboard-shaped evdev node until one yields a usable keymap.
 * Used both to resolve k1/k2 and to label codes in monitor mode. */
static int hid_map_from_any_keyboard(const char *input_dir,
                                     uint16_t *usage_of_key) {
    struct dirent **ents;
    int n = scandir(input_dir, &ents, is_event_node, alphasort);
    if (n < 0) return 0;

    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!found) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", input_dir, ents[i]->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                found = hid_map_build(fd, usage_of_key);
                close(fd);
            }
        }
        free(ents[i]);
    }
    free(ents);
    return found;
}

/* One analog pair: its two keys' front-ends, its state machine and its
 * latch. Everything the latch functions touch lives here, so each pair
 * arms, trills and disarms on its own two keys and nothing else -
 * the latch rule stays a two-key test however many pairs there are. `idx`
 * names the pair in cfg->pair[]; a memset one is pair 0, which is what the
 * tools rely on. */
typedef struct {
    int            idx;         /* which cfg->pair[] this is */
    uint16_t       usage[2];    /* HID usage id of k1, k2 */
    analog_state_t keys;
    machine_t      machine;
    int            armed;       /* the latch: 1 == armed by a plant, k1/k2
                                 * trill; 0 == independent. See
                                 * analog_latch_update. */
    int            last_press;  /* 0 == k1, 1 == k2: most recent press edge.
                                 * Picks which key stays down when
                                 * the latch arms with both already
                                 * down - last input wins. */
    int            plant_pending; /* both keys are on the backplate and the
                                   * debounce is running */
    uint64_t       plant_ns;    /* report time that started the debounce.
                                 * Not a sentinel: machinetest passes 0 on
                                 * every report, so `plant_pending` is. */
    float          plant_mm[2]; /* latch depths in the latest report while
                                 * the debounce runs - only for the ARMED
                                 * log line when the window expires between
                                 * reports (analog_latch_tick) */
    int            arm_on_report; /* REPLAY ONLY, never set by the daemon:
                                   * judge the debounce on reports alone, as
                                   * the daemon did before it had a timer.
                                   * See analog_latch_tick. */
} analog_pair_t;

/* An open analog interface plus the pairs it drives. One per daemon: the
 * board is one device whatever is mapped on it, and a second analog board
 * would only fight the first over the same keys. Each report is read
 * and decoded once here and handed to every pair. */
typedef struct {
    int            kind;        /* EP_ANALOG */
    int            fd;
    int            fmt;
    uint16_t       vid, pid;
    char           path[PATH_MAX];
    analog_pair_t  pair[MAX_PAIRS];
    int            n_pairs;
} analog_dev_t;

/* "pair N: " for log lines when there is more than one pair, and nothing
 * when there is one, so a single-pair daemon logs exactly as it did. */
static const char *pair_tag(const oid_config_t *cfg, int idx) {
    static char buf[16];
    if (cfg->n_pairs <= 1)
        return "";
    snprintf(buf, sizeof(buf), "pair %d: ", idx);
    return buf;
}

/* Open the board and resolve the pairs it drives: every analog-mode pair, or
 * with `only` >= 0 that one pair whatever its mode (-T records one pair, and
 * passively, so its mode does not matter). `quiet` is analog_open's: the
 * reconnect retry stays silent until it succeeds. */
static analog_dev_t *analog_dev_open(const oid_config_t *cfg,
                                     const char *input_dir, int only,
                                     int quiet) {
    analog_dev_t *ad = calloc(1, sizeof(*ad));
    if (!ad) {
        LOG_ERR("oom");
        return NULL;
    }
    ad->kind = EP_ANALOG;
    ad->fd   = analog_open(cfg->analog.device, ad->path, sizeof(ad->path),
                           &ad->fmt, &ad->vid, &ad->pid, quiet);
    if (ad->fd < 0) {
        free(ad);
        return NULL;
    }

    /* Resolve each pair's k1/k2 to HID usage ids: an explicit config
     * override wins, otherwise walk a keyboard's keymap for the mapping. */
    static uint16_t usage_of_key[KEY_MAX + 1];
    int             walked = 0;
    for (int i = 0; i < cfg->n_pairs; i++) {
        const keypair_t *pc = &cfg->pair[i];
        if (only >= 0 ? i != only : pc->mode != MODE_ANALOG)
            continue;
        if ((pc->hid_k1 < 0 || pc->hid_k2 < 0) && !walked) {
            hid_map_from_any_keyboard(input_dir, usage_of_key);
            walked = 1;
        }
        analog_pair_t *ap = &ad->pair[ad->n_pairs];
        ap->idx      = i;
        ap->usage[0] = pc->hid_k1 >= 0 ? (uint16_t)pc->hid_k1
                                       : usage_of_key[pc->k1];
        ap->usage[1] = pc->hid_k2 >= 0 ? (uint16_t)pc->hid_k2
                                       : usage_of_key[pc->k2];
        if (!ap->usage[0] || !ap->usage[1]) {
            if (!quiet)
                LOG_ERR("%scould not resolve k1/k2 to HID usage ids - run "
                        "with -A to find them, then set the pair's hid",
                        pair_tag(cfg, i));
            close(ad->fd);
            free(ad);
            return NULL;
        }
        ad->n_pairs++;
    }

    /* One usage, one machine. pairs_check refused colliding overrides; this
     * catches the rest - an override landing on a key another pair resolved
     * from the keymap - which only exists once the keymap has been read. */
    for (int a = 0; a < ad->n_pairs * 2; a++)
        for (int b = a + 1; b < ad->n_pairs * 2; b++) {
            const analog_pair_t *pa = &ad->pair[a / 2], *pb = &ad->pair[b / 2];
            if (pa->usage[a % 2] != pb->usage[b % 2])
                continue;
            if (!quiet)
                LOG_ERR("pairs[%d].keys[%d] and pairs[%d].keys[%d] both read "
                        "hid 0x%02x: one physical key would drive two "
                        "machines", pa->idx, a % 2, pb->idx, b % 2,
                        pa->usage[a % 2]);
            close(ad->fd);
            free(ad);
            return NULL;
        }

    g_analog_vid = ad->vid;
    g_analog_pid = ad->pid;

    LOG_INFO("Analog input on %s (%s, %04x:%04x)", ad->path,
             ad->fmt == ANALOG_FMT_V2 ? "v2" : "v1", ad->vid, ad->pid);
    for (int i = 0; i < ad->n_pairs; i++)
        LOG_INFO("  %sk1 -> hid 0x%02x, k2 -> hid 0x%02x",
                 pair_tag(cfg, ad->pair[i].idx),
                 ad->pair[i].usage[0], ad->pair[i].usage[1]);
    LOG_INFO("Analog thresholds: actuation %.2fmm, release %.2fmm "
             "(derived), backplate at %.2fmm, rapid trigger %s, latch "
             "debounce %.1fms",
             (double)cfg->analog.actuation_mm, (double)cfg->analog.release_mm,
             (double)(cfg->analog.travel_mm - cfg->analog.backplate_mm),
             cfg->analog.rt_enabled ? "on" : "off",
             (double)cfg->analog.latch_debounce_ms);
    LOG_INFO("The latch arms on a plant (both keys on the backplate); "
             "while armed the pair trills, the backplate is the switch and "
             "rapid trigger does not run; it disarms when BOTH keys leave "
             "the backplate - one finger lifting is a stroke, not an exit");
    if (cfg->analog.rt_enabled) {
        LOG_INFO("Rapid trigger (independent only): press %.2fmm "
                 "release %.2fmm - while armed, the backplate is the switch",
                 (double)cfg->analog.rt_press_mm,
                 (double)cfg->analog.rt_release_mm);
    }
    return ad;
}

static void analog_dev_close(analog_dev_t *ad) {
    if (!ad) return;
    if (ad->fd >= 0) close(ad->fd);
    free(ad);
}

/* -A: dump live travel depth so thresholds can be picked against the real
 * board, and so a key's usage id can be discovered when the keymap walk
 * comes up empty. Deliberately touches nothing else - no grab, no uinput -
 * so it is safe to run alongside a live daemon. */
/* -T: dump the travel of k1/k2 to stdout, one line per hardware report,
 * so a play session can be replayed through the state machine offline.
 *
 * Passive, exactly like -A: this opens the hidraw node and resolves the
 * two HID usages, and does nothing else. No EVIOCGRAB, no uinput device,
 * nothing in the input path - whoever is recording plays on their own
 * setup, with their own keyboard behaving normally. That is what makes it
 * reasonable to ask someone else to run it.
 *
 * Both keys are emitted on every line even when only one moved, because a
 * released key vanishes from the report rather than reporting a zero, so
 * the absent one really is at rest. Timestamps are monotonic microseconds
 * since `t0_monotonic_ns` in the header, which is taken just before it is
 * written. The header carries that origin in BOTH clocks so a trace can be
 * lined up with anything else on the machine: monotonic against whatever
 * else is stamped on that clock, realtime against logs that print wall
 * time. The two reads are back to back, so the pair agrees to a few
 * microseconds. Readers that predate the anchors skip them as comments.
 *
 * The header also records every threshold the analog front-end reads, so
 * `replay` can run a trace under the settings it was PLAYED under rather
 * than whatever the config says by the time someone replays it. Without
 * that, a trace is only meaningful alongside a copy of the config it was
 * recorded with. The keys are spelled as their config paths, and release_mm
 * is left out because it is derived from actuation_mm, not configured.
 *
 * `# latch_arm=deadline` is the same idea for a rule instead of a number: it
 * says the daemon this binary is arms the latch when the debounce runs out on
 * the clock (analog_latch_tick). A trace WITHOUT the line was recorded before
 * the timer existed, by a daemon that armed on the next report, and `replay`
 * reproduces it that way. It describes the binary, not the config, so it is
 * true of the session only while `-T` and the running daemon are the same
 * build - which is how they are installed. It goes AFTER the `# analog` line
 * on purpose: readers that only look at the first few header lines keep
 * finding everything where it was. If the arm rule ever changes again, give
 * it a new value here rather than reusing this one. */
static int analog_trace(const oid_config_t *cfg, const char *config_path,
                        const char *input_dir) {
    analog_dev_t *ad = analog_dev_open(cfg, input_dir, cfg_trace_pair(cfg), 0);
    if (!ad)
        return EXIT_FAILURE;
    const analog_pair_t *tp = &ad->pair[0];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct timespec t0, t0_real;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME,  &t0_real);

    printf("# evtrill trace v1\n");
    printf("# travel_mm=%.4f\n", (double)cfg->analog.travel_mm);
    printf("# t0_monotonic_ns=%lld\n",
           (long long)t0.tv_sec * 1000000000LL + t0.tv_nsec);
    printf("# t0_realtime_ns=%lld\n",
           (long long)t0_real.tv_sec * 1000000000LL + t0_real.tv_nsec);
    /* Absolute, so a trace recorded with `-c config.yaml` still says which
     * file it came from once it has left the directory it was taken in. */
    char *abs = realpath(config_path, NULL);
    printf("# config_path=%s\n", abs ? abs : config_path);
    free(abs);
    /* %g, not a fixed precision: any value a person typed into the config
     * prints back exactly, so replay reads the same float the daemon used. */
    const analog_config_t *a = &cfg->analog;
    printf("# analog travel_mm=%g actuation_mm=%g rapid_trigger.enabled=%d "
           "rapid_trigger.press_mm=%g rapid_trigger.release_mm=%g "
           "backplate_mm=%g latch_debounce_ms=%g\n",
           (double)a->travel_mm, (double)a->actuation_mm, a->rt_enabled,
           (double)a->rt_press_mm, (double)a->rt_release_mm,
           (double)a->backplate_mm, (double)a->latch_debounce_ms);
    printf("# latch_arm=deadline\n");
    printf("us,k1_mm,k2_mm\n");
    fflush(stdout);

    LOG_INFO("Tracing %sk1/k2 travel to stdout. Play normally; Ctrl-C to "
             "stop.", pair_tag(cfg, tp->idx));

    unsigned long   lines   = 0;

    while (g_running) {
        struct pollfd pfd = { .fd = ad->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(ad->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            LOG_ERR("read(%s): %s", ad->path, strerror(errno));
            break;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long us = (long long)(now.tv_sec - t0.tv_sec) * 1000000
                     + (now.tv_nsec - t0.tv_nsec) / 1000;

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(ad->fmt, buf, (size_t)n, s,
                                           ANALOG_SLOTS);
        float depth[2] = { 0.0f, 0.0f };
        for (int i = 0; i < ns; i++)
            for (int k = 0; k < 2; k++)
                if (s[i].code == tp->usage[k])
                    depth[k] = s[i].depth * cfg->analog.travel_mm;

        printf("%lld,%.4f,%.4f\n", us, (double)depth[0], (double)depth[1]);
        lines++;
    }

    fflush(stdout);
    LOG_INFO("Traced %lu reports.", lines);
    analog_dev_close(ad);
    return EXIT_SUCCESS;
}

static int analog_monitor(const char *forced, const char *input_dir,
                          float travel_mm) {
    char     path[PATH_MAX];
    int      fmt = ANALOG_FMT_NONE;
    uint16_t vid = 0, pid = 0;

    int fd = analog_open(forced, path, sizeof(path), &fmt, &vid, &pid, 0);
    if (fd < 0) {
        LOG_ERR("no analog interface found%s",
                forced ? "" : " (is an analog keyboard connected?)");
        return EXIT_FAILURE;
    }

    static uint16_t usage_of_key[KEY_MAX + 1];
    char            name_of_usage[256][32];

    memset(name_of_usage, 0, sizeof(name_of_usage));
    hid_map_from_any_keyboard(input_dir, usage_of_key);
    for (int code = 1; code <= KEY_MAX; code++) {
        uint16_t u = usage_of_key[code];
        if (!u || u >= 256 || name_of_usage[u][0])
            continue;
        const char *nm = libevdev_event_code_get_name(EV_KEY, code);
        if (nm)
            snprintf(name_of_usage[u], sizeof(name_of_usage[u]), "%s", nm);
    }

    LOG_INFO("Analog monitor on %s (%s, %04x:%04x), travel %.1fmm",
             path, fmt == ANALOG_FMT_V2 ? "v2" : "v1", vid, pid, travel_mm);
    LOG_INFO("Press keys to see live depth; peak is reported on release. "
             "Ctrl-C to exit.");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    float peak[256], cur[256];
    memset(peak, 0, sizeof(peak));
    memset(cur,  0, sizeof(cur));

    while (g_running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            LOG_ERR("read(%s): %s", path, strerror(errno));
            break;
        }

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(fmt, buf, (size_t)n, s, ANALOG_SLOTS);

        /* A released key drops out of the report entirely on at least some
         * firmware, rather than reporting a final zero - so rest is inferred
         * from absence, by diffing this frame against the last. */
        float now[256];
        memset(now, 0, sizeof(now));
        for (int i = 0; i < ns; i++)
            if (s[i].code < 256)
                now[s[i].code] = s[i].depth;

        for (int c = 0; c < 256; c++) {
            if (now[c] > peak[c])
                peak[c] = now[c];
            if (now[c] > 0.0f || cur[c] <= 0.0f)
                continue;
            /* was down last frame, absent now: report and clear the peak */
            const char *nm = name_of_usage[c][0] ? name_of_usage[c] : "?";
            printf("\33[2K\r  %-14s hid 0x%02x   peak %5.2f mm  (%.3f)\n",
                   nm, c, peak[c] * travel_mm, peak[c]);
            peak[c] = 0.0f;
        }
        memcpy(cur, now, sizeof(cur));

        /* Live line: everything currently off its rest position. */
        char line[512];
        int  off = 0, shown = 0;
        for (int i = 0; i < ns && shown < 4; i++) {
            if (s[i].depth <= 0.0f || s[i].code >= 256) continue;
            const char *nm = name_of_usage[s[i].code][0]
                           ? name_of_usage[s[i].code] : "?";
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "  %s 0x%02x %5.2fmm", nm, s[i].code,
                            s[i].depth * travel_mm);
            shown++;
            if (off >= (int)sizeof(line)) break;
        }
        printf("\33[2K\r%s", shown ? line : "  (all keys at rest)");
        fflush(stdout);
    }

    printf("\33[2K\r");
    fflush(stdout);
    close(fd);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Virtual uinput device                                                     */
/* ------------------------------------------------------------------------- */

static struct libevdev         *vdev  = NULL;
static struct libevdev_uinput  *uidev = NULL;

/* Virtual keyboard with a fixed keyboard-wide key set plus configured
 * pair keys. uinput capabilities are immutable after creation and hotplugged
 * keyboards may carry codes a startup-time union wouldn't have, so every
 * KEY_* code is enabled up front; the BTN_* pointer/gamepad ranges are
 * skipped so desktops keep classifying the device as a keyboard.
 * EV_SYN always enabled. Not cloning EV_REL,EV_ABS,etc */
static int build_virtual(const oid_config_t *cfg) {
    vdev = libevdev_new();
    if (!vdev) {
        LOG_ERR("libevdev_new failed");
        return -1;
    }
    libevdev_set_name(vdev, cfg->uinput_name);
    libevdev_set_uniq(vdev, "evtrill");
    libevdev_set_id_bustype(vdev, BUS_VIRTUAL);
    libevdev_set_id_vendor (vdev, 0x0001);
    libevdev_set_id_product(vdev, 0x0001);
    libevdev_set_id_version(vdev, 0x0001);

    libevdev_enable_event_type(vdev, EV_SYN);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_REPORT,    NULL);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_MT_REPORT, NULL);

    libevdev_enable_event_type(vdev, EV_KEY);
    for (unsigned int code = 1; code < KEY_MAX; code++) {
        if (code >= BTN_MISC && code < KEY_OK)
            continue; /* mouse/joystick/gamepad/digitizer buttons */
        if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
            continue;
        if (code >= BTN_TRIGGER_HAPPY)
            continue;
        libevdev_enable_event_code(vdev, EV_KEY, code, NULL);
    }
    for (int i = 0; i < cfg->n_pairs; i++) {
        libevdev_enable_event_code(vdev, EV_KEY, cfg->pair[i].k1, NULL);
        libevdev_enable_event_code(vdev, EV_KEY, cfg->pair[i].k2, NULL);
    }

    int rc = libevdev_uinput_create_from_device(
        vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
    if (rc < 0) {
        LOG_ERR("libevdev_uinput_create_from_device: %s", strerror(-rc));
        libevdev_free(vdev);
        vdev = NULL;
        return -1;
    }

    const char *node = libevdev_uinput_get_devnode(uidev);
    LOG_INFO("Created virtual keyboard \"%s\" at %s",
             cfg->uinput_name, node ? node : "(unknown)");
    return 0;
}

static void destroy_virtual(void) {
    if (uidev) { libevdev_uinput_destroy(uidev); uidev = NULL; }
    if (vdev)  { libevdev_free(vdev);            vdev  = NULL; }
}

/* ------------------------------------------------------------------------- */
/* Radio button state machine                                                */
/* ------------------------------------------------------------------------- */

/*
 *   state = k1 + k2 + ie.value   (k1, k2 post-update)
 *
 *     0 = NONE     last key released
 *     1 = RELEASE  two keys held -> one released
 *     2 = SINGLE   first key pressed (zero -> one)
 *     3 = PRESS    one key held -> second pressed (one -> two)
 *
 * RELEASE depends on cfg->mode: MODE_TRILL reverts to the other key no
 * matter which of the two was released (latching), MODE_LAST ("last
 * input wins") only falls through to the still-held key when the active
 * key is the one released. MODE_INDEPENDENT skips the state machine entirely:
 * each key passes through one-to-one, nothing else.
 */
enum { S_NONE = 0, S_RELEASE = 1, S_SINGLE = 2, S_PRESS = 3 };

/* The state machine. Takes a synthesized (is_k1, value) edge rather than a raw
 * evdev event so that both the evdev path and the analog path can drive it:
 * process_event() below feeds it real key events, analog_drive() feeds it
 * edges derived from travel depth. The transition logic is identical either
 * way, which is the point. */
static void machine_apply(machine_t *in, int is_k1, int value, int mode,
                          const keypair_t *cfg) {
    in->mode = mode;

    if (mode == MODE_INDEPENDENT) {
        /* k1/k2 still tracked so release_stuck can clean up a dying device. */
        if (value != 2) {
            if (is_k1) in->k1 = value;
            else       in->k2 = value;
        }
        libevdev_uinput_write_event(uidev, EV_KEY,
                                    is_k1 ? cfg->k1 : cfg->k2, value);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        return;
    }

    if (value == 2) /* autorepeat messes up state*/
        return;

    if (is_k1) in->k1 = value;
    else       in->k2 = value;

    int state = in->k1 + in->k2 + value;

    switch (state) {
        case S_SINGLE: {
        int code = is_k1 ? cfg->k1 : cfg->k2;
        in->act = is_k1;
        libevdev_uinput_write_event(uidev, EV_KEY, code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        break;
        }
        case S_RELEASE:
        /* Last: releasing the non-active (already suppressed) key
         * changes nothing; only the active key's release falls through to
         * the still-held one. Trill falls back on either release. */
        if (mode == MODE_LAST && is_k1 != in->act)
            break;
        /* fallthrough */
        case S_PRESS: {
        int up_code   = in->act ? cfg->k1 : cfg->k2;
        int down_code = in->act ? cfg->k2 : cfg->k1;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        libevdev_uinput_write_event(uidev, EV_KEY, down_code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = !in->act;
        break;
        }
        case S_NONE: {
        int up_code = in->act ? cfg->k1 : cfg->k2;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = 0;
        break;
        }
        default:
        /* shouldn't make it here w/ sane inputs. */
        break;
    }
}

/* Which pair a key code belongs to, and whether it is that pair's k1.
 * -1 when it is in none. pairs_check guarantees at most one match. */
static int pair_of_key(const oid_config_t *cfg, unsigned code, int *is_k1) {
    for (int i = 0; i < cfg->n_pairs; i++) {
        if (code == (unsigned)cfg->pair[i].k1) { *is_k1 = 1; return i; }
        if (code == (unsigned)cfg->pair[i].k2) { *is_k1 = 0; return i; }
    }
    return -1;
}

/* evdev entry point: mirror everything that isn't a pair key, and hand pair
 * keys to that pair's state machine. On the analog board, an analog-mode pair is
 * driven from travel depth instead, so its digital copies are dropped here to
 * avoid emitting each press twice - but ONLY that pair's: a digital-mode pair
 * on the same board has nobody else to drive it. */
static void process_event(input_dev_t *in, const struct input_event *ie,
                          const oid_config_t *cfg) {
    if (ie->type == EV_MSC && ie->code == MSC_SCAN)
        return;

    int is_k1 = 0;
    int p = ie->type == EV_KEY ? pair_of_key(cfg, ie->code, &is_k1) : -1;
    if (p < 0) {
        libevdev_uinput_write_event(uidev, ie->type, ie->code, ie->value);
        return;
    }

    if (in->analog && cfg->pair[p].mode == MODE_ANALOG)
        return;

    machine_apply(&in->machine[p], is_k1, ie->value, cfg->pair[p].mode,
                  &cfg->pair[p]);
}

/* ------------------------------------------------------------------------- */
/* loop                                                                      */
/* ------------------------------------------------------------------------- */

/* drain everything libevdev has buffered, looping over SYNC drops
 * as needed. return 0 on normal EAGAIN, -1 on fatal error. */
static int drain_device(input_dev_t *in, const oid_config_t *cfg) {
    unsigned int flag = LIBEVDEV_READ_FLAG_NORMAL;
    for (;;) {
        struct input_event ie;
        int rc = libevdev_next_event(in->dev, flag, &ie);
        if (rc == -EAGAIN)
            break;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            flag = LIBEVDEV_READ_FLAG_SYNC;
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            LOG_WARN("libevdev_next_event(%s): %s",
                     in->path, strerror(-rc));
            return -1;
        }
        flag = LIBEVDEV_READ_FLAG_NORMAL;
        /* Grab still deferred: the system receives these events directly
         * through the ungrabbed device, so mirroring or filtering them
         * would double them up - just let libevdev track the key state. */
        if (in->grabbed)
            process_event(in, &ie, cfg);
    }

    if (!in->grabbed && !any_key_down(in->dev)) {
        int rc = libevdev_grab(in->dev, LIBEVDEV_GRAB);
        if (rc < 0) {
            LOG_WARN("libevdev_grab(%s): %s", in->path, strerror(-rc));
            return -1; /* dropped; inotify-driven reconcile retries later */
        }
        in->grabbed = 1;
        LOG_INFO("All keys released - grabbed %s", in->path);
    }
    return 0;
}

/* Which keys `mode` would be holding down on the virtual keyboard, given
 * which physical keys `held` says are down. MODE_INDEPENDENT mirrors
 * one-to-one; the trill holds exactly one - the `act` one - whenever
 * anything at all is held. Used to diff the two against each other when the
 * latch changes under held fingers. */
static void machine_outputs(const int *held, int act, int mode,
                            int *down) {
    down[0] = down[1] = 0;
    if (mode == MODE_INDEPENDENT) {
        down[0] = held[0];
        down[1] = held[1];
    } else if (held[0] || held[1]) {
        down[act ? 0 : 1] = 1;
    }
}

/* Arm or disarm the latch, reconciling the virtual keyboard as we go.
 *
 * The two rules disagree about what should be held: independent wants a key
 * down per held physical key, the trill wants exactly one. Flipping the flag
 * without settling that difference is what strands a key - the trill can
 * legitimately leave `act` on a finger that has already lifted, and a later
 * release routed through independent writes only its own code, so whatever
 * the trill pressed is never released. So compute both pictures and emit the
 * difference.
 *
 * Arming with both keys already down - which is always, since arming means
 * both are on the backplate and independent has pressed both - means one of
 * them has to let go. The most recently pressed key keeps it: the newest
 * stroke wins, the same principle last runs on. Which one keeps it does NOT
 * decide `act`; see analog_machine_edge. */
static void analog_latch_set(analog_pair_t *ad, int on,
                            const oid_config_t *cfg) {
    /* Where the keys stand comes from the machine, which reflects the
     * edges already APPLIED. Where they should end up comes from the
     * front-end's `live`, which is this report's truth - the machine still
     * lags it by whatever edges have not been routed yet. Targeting it
     * would press a key for a finger that left on this very sample, then
     * release it again when the edge lands: a phantom note on the way out
     * of a rock. */
    const keypair_t *pc = &cfg->pair[ad->idx];
    int cur[2]  = { ad->machine.k1, ad->machine.k2 };
    int next[2] = { ad->keys.key[0].live, ad->keys.key[1].live };
    int from[2], to[2];

    machine_outputs(cur, ad->machine.act,
                    ad->armed ? pc->mode : MODE_INDEPENDENT, from);

    if (on) {
        int keep = ad->last_press;
        if (!next[keep]) keep = !keep;      /* it lifted; the other has it */
        ad->machine.act = (keep == 0);
    } else {
        /* PARK both front ends on the way out. Disarming means neither key
         * is on the backplate, so nothing is held - but a key can still be
         * well down (it need only have left the backplate), and the
         * independent rules are about to judge it against actuation_mm.
         *
         * Clearing `live` alone would let rule 3 read that depth as a FRESH
         * actuation and press a finger that is on its way up - a phantom
         * note landing exactly where a slider ends. Leaving `live` set is no
         * better: its eventual release emits an up with no matching down,
         * and machine_apply assumes strict alternation per key.
         *
         * `live = 0` with `rt_extreme` still holding the current depth is
         * neither: it is rule 2's "engaged but not live" state, where the
         * key tracks its extreme on the way up, emits nothing, and re-presses
         * only on a real stroke - a bottom-out, or press_mm of down-travel.
         * analog_key_feed's armed branch leaves rt_extreme parked for
         * exactly this handoff. */
        for (int k = 0; k < 2; k++)
            ad->keys.key[k].live = 0;
        next[0] = next[1] = 0;
    }
    ad->armed = on;
    machine_outputs(next, ad->machine.act, on ? pc->mode : MODE_INDEPENDENT, to);

    /* Emit the difference. */
    for (int k = 0; k < 2; k++) {
        if (from[k] == to[k]) continue;
        libevdev_uinput_write_event(uidev, EV_KEY,
                                    k == 0 ? pc->k1 : pc->k2, to[k]);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
    }
}

/* The debounce window in whole nanoseconds, rounded UP, and the one place
 * it is computed. The deadline and the arm test must agree to the
 * nanosecond: with the deadline truncated and the test left in floating
 * point, a tick landing exactly on the deadline came up a fraction of a
 * nanosecond short for any value that is not exact in a float (33.3ms,
 * 16.7ms) and did not arm. The daemon's timer always fires a little late,
 * so it never showed there; replay ticks AT the deadline, and stamped the
 * arm on the next report instead. */
static uint64_t analog_debounce_ns(const analog_config_t *a) {
    if (a->latch_debounce_ms <= 0.0f)
        return 0;
    double   want = (double)a->latch_debounce_ms * 1e6;
    uint64_t ns   = (uint64_t)want;
    return (double)ns < want ? ns + 1 : ns;     /* ceil, without libm */
}

/* When a running latch debounce runs out, or 0 if none is running (no
 * plant pending, already armed, or the debounce is off and so never waits).
 * The daemon arms a timerfd on this; see analog_latch_tick. */
static int analog_latch_deadline(const analog_pair_t *ad, const oid_config_t *cfg,
                                uint64_t *deadline_ns) {
    if (ad->arm_on_report)
        return 0;
    if (ad->armed || !ad->plant_pending || cfg->analog.latch_debounce_ms <= 0.0f)
        return 0;
    *deadline_ns = ad->plant_ns + analog_debounce_ns(&cfg->analog);
    return 1;
}

/* Arm the latch if the debounce has run out at `t_ns`. Shared by the
 * per-report test and the timer, so both arm identically. */
static void analog_latch_try_arm(analog_pair_t *ad, const oid_config_t *cfg,
                                uint64_t t_ns) {
    const analog_config_t *a = &cfg->analog;
    if (ad->armed || !ad->plant_pending)
        return;
    if (t_ns - ad->plant_ns < analog_debounce_ns(a))
        return;
    ad->plant_pending = 0;
    /* Logged because arming is otherwise INVISIBLE: it is release-only
     * by construction, it never presses, and the first dip out of it
     * reverts through two writes the kernel drops. With no trace of it
     * anywhere, a latch that never arms and a latch that arms and works
     * look identical from outside - which is exactly the question you
     * are asking when the pair seems stuck independent.
     * Transitions are once per rock, not per report. */
    LOG_INFO("%sLatch ARMED: k1 %.2fmm, k2 %.2fmm both at or past "
             "the backplate (%.2fmm) for %.1fms - trilling",
             pair_tag(cfg, ad->idx), (double)ad->plant_mm[0], (double)ad->plant_mm[1],
             (double)(a->travel_mm - a->backplate_mm),
             (double)(t_ns - ad->plant_ns) / 1e6);
    analog_latch_set(ad, 1, cfg);
}

/* The debounce window, judged on the clock rather than on the next report.
 *
 * The board reports on CHANGE, so two planted, still fingers send nothing at
 * all. Judging the window only when a report arrived meant a rock that
 * planted and held never armed until something moved - and if the thing that
 * moved was the first lift of the rock, one key was already off the backplate,
 * the pending plant was cancelled, and the latch never armed for that rock.
 * Measured on a user journal at 30ms: mean arm delay 68.6ms, 5% within 5ms of
 * the setting, plus every rock that silently never armed (issue #12).
 *
 * `plant_pending` can only be standing here if the latest report had both
 * keys on the backplate, and any movement off it would itself have been a report,
 * so "no report since" means "still planted". The daemon calls this from a
 * timerfd at the deadline; analog_report also calls it with the report's own
 * time before feeding, so a report that lands after a window the timer has
 * not yet been serviced for - or the tools, which have no timer - arm first
 * and then run the report armed, exactly as the daemon would have.
 * Nothing is emitted here except what analog_latch_set reconciles.
 *
 * `arm_on_report` turns all of this off, and exists for one caller: replay,
 * on a trace recorded by a daemon that had no timer. That daemon judged the
 * window only inside analog_latch_update, so a still plant armed on the next
 * report and a rock whose next report was its first lift never armed at all.
 * That was the defect - but it is also what the virtual keyboard DID in
 * those sessions, and replaying them under this rule instead moves every arm
 * earlier and adds latches that never happened. A `-T` trace says which rule
 * it was played under (`# latch_arm=deadline`, see analog_trace); one that
 * does not say predates the timer. Both callers come through here, and
 * analog_latch_deadline reports nothing to wait for, so the flag is the whole
 * of the old behaviour - analog_latch_update is the same code either way. */
static void analog_latch_tick(analog_pair_t *ad, const oid_config_t *cfg,
                             uint64_t t_ns) {
    if (ad->arm_on_report)
        return;
    analog_latch_try_arm(ad, cfg, t_ns);
}

/* Resolve the latch for this report, before the report's edges are
 * applied.
 *
 * This is the whole discriminator between the two play patterns, and both
 * halves are pure depth tests on the CURRENT frame - no per-key latch, no
 * comparison of one key's depth against the other's. That last point is not
 * an accident: every abandoned version of this feature gated on relative
 * depth, trying to decide whether a shallow stroke "deserved" to steal from
 * a deeper one, and that is not a problem that exists. Do not reintroduce it.
 *
 * Deciding before the edges is what stops the lift that ends a rock from
 * answered with a fallback on its way out: the release then lands as a
 * plain release, over keys the reconciliation has already put where
 * MODE_INDEPENDENT expects them. */
static void analog_latch_update(analog_pair_t *ad, const oid_config_t *cfg,
                               const float *depth, uint64_t t_ns) {
    const analog_config_t *a = &cfg->analog;

    float backplate_depth = a->travel_mm - a->backplate_mm;

    if (ad->armed) {
        ad->plant_pending = 0;
        /* BOTH fingers off the backplate ends it. */
        if (depth[0] < backplate_depth && depth[1] < backplate_depth) {
            LOG_INFO("%sLatch disarmed: k1 %.2fmm, k2 %.2fmm - both off "
                     "the backplate (%.2fmm), independent again",
                     pair_tag(cfg, ad->idx), (double)depth[0], (double)depth[1], (double)backplate_depth);
            analog_latch_set(ad, 0, cfg);
        }
        return;
    }

    /* Both against the backplate in ONE sample.
     *
     * Alt-tapping cannot satisfy this. Its defining shape is one finger
     * leaving as the other arrives, so the two are never both on the backplate
     * together, however fast it is played and however much the presses
     * overlap in the middle of travel. Rocking, by contrast, BEGINS by
     * planting both - you cannot start one any other way. That asymmetry is
     * the entire signal, and unlike a depth in the middle of travel the
     * backplate is a hard physical stop: you can hit it without
     * proprioception, and "plant both keys to start rocking" is an
     * instruction a player can actually follow.
     *
     * Hysteresis is enormous by construction, so there is no chatter to
     * debounce in the usual sense. What `latch_debounce_ms` filters is a
     * different thing: a 2B or two hard alt-taps that BOTH happen to touch
     * the backplate in the same report, which is an instant, against a rock,
     * which plants and stays. It wraps this test and nothing else - the
     * depths are still judged on the current sample alone, nothing is
     * compared across keys, and while it runs the pair stays independent,
     * so the cost of a debounce is that the trill engages that much later,
     * never a phantom note. Either key leaving the backplate restarts it. */
    if (depth[0] < backplate_depth || depth[1] < backplate_depth) {
        ad->plant_pending = 0;
        return;
    }
    if (!ad->plant_pending) {
        ad->plant_pending = 1;
        ad->plant_ns      = t_ns;
    }
    ad->plant_mm[0] = depth[0];
    ad->plant_mm[1] = depth[1];
    analog_latch_try_arm(ad, cfg, t_ns);
}

/* Route one synthesized edge into the state machine under whatever rule
 * the latch selects. The latch itself is resolved once per sample in
 * analog_drain, before any of that sample's edges are applied. */
static void analog_machine_edge(analog_pair_t *ad, int is_k1, int value,
                                const oid_config_t *cfg) {
    /* cfg->mode is MODE_ANALOG here; machine_apply special-cases only
     * INDEPENDENT and LAST, so ANALOG runs the trill. Deliberate, not
     * accidental. */
    const keypair_t *pc = &cfg->pair[ad->idx];
    int mode = ad->armed ? pc->mode : MODE_INDEPENDENT;

     /* Arming leaves the picture in the trill's canonical form - exactly
     * one key down, and `act` names it truthfully - and every later edge
     * is a plain backplate crossing. up(act) is always a real release and
     * down(!act) always a real press, whichever finger moves. */
    if (value == 1)
        ad->last_press = is_k1 ? 0 : 1;

    machine_apply(&ad->machine, is_k1, value, mode, pc);
}

/* Turn ONE report's worth of depths into edges. Split out of
 * analog_drain so that tools/replay.c and tools/machinetest.c drive this
 * exact function instead of reimplementing the ordering - the latch
 * resolution and the edge routing have to happen in this order, and a copy
 * in the tools would be free to drift from the daemon while still passing
 * its own assertions. */
static void analog_report(analog_pair_t *ad, const oid_config_t *cfg,
                          const float *depth, uint64_t t_ns) {
    /* Feed both keys before judging either: engagement and lapse read the
     * depth/live state of BOTH, which is only complete once both have been
     * fed. The latch is then resolved BEFORE this sample's edges are
     * applied, so every edge runs under the mode actually in force for it.
     * That ordering is what stops the lift ending a rock from being
     * answered with a fallback on its way out - the release lands as a
     * plain release, and the reconciliation in analog_latch_set has already
     * put the keys where independent expects them. */
    /* A debounce that ran out since the last report arms first, as of now,
     * so this report is judged under the mode that was already in force for
     * it. See analog_latch_tick. */
    analog_latch_tick(ad, cfg, t_ns);

    int edges[2][2], ne[2];
    for (int k = 0; k < 2; k++)
        ne[k] = analog_key_feed(&ad->keys, k, depth[k], &cfg->analog,
                                ad->armed, edges[k]);

    analog_latch_update(ad, cfg, depth, t_ns);

    for (int k = 0; k < 2; k++)
        for (int e = 0; e < ne[k]; e++)
            analog_machine_edge(ad, k == 0, edges[k][e], cfg);
}

/* Read every queued analog report and turn travel depth into edges.
 * A key absent from a report is at rest, so each frame is a complete
 * picture rather than a delta: resolve the current depth of k1/k2, feed
 * both to the front-end, and route whatever edges come back through the
 * same core the evdev path uses. Returns -1 if the device died. */
static int analog_drain(analog_dev_t *ad, const oid_config_t *cfg) {
    for (;;) {
        unsigned char buf[ANALOG_V2_REPORT];
        ssize_t       n = read(ad->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_WARN("read(%s): %s", ad->path, strerror(errno));
            return -1;
        }
        if (n == 0)
            return -1;

        /* hidraw carries no timestamp, so the report is stamped on arrival
         * - the latch debounce in analog_latch_update is judged on these.
         * One vDSO read on a path that already syscalls per report. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t t_ns = (uint64_t)now.tv_sec * 1000000000ull +
                        (uint64_t)now.tv_nsec;

        analog_sample_t s[ANALOG_SLOTS];
        int             ns = analog_decode(ad->fmt, buf, (size_t)n, s,
                                           ANALOG_SLOTS);

        /* One report, every pair - in pair order, each on its own two
         * keys. Pairs share no keys, so the order between
         * them decides nothing but the order of writes within a report. */
        for (int p = 0; p < ad->n_pairs; p++) {
            analog_pair_t *ap = &ad->pair[p];
            float depth[2] = { 0.0f, 0.0f };
            for (int i = 0; i < ns; i++)
                for (int k = 0; k < 2; k++)
                    if (s[i].code == ap->usage[k])
                        depth[k] = s[i].depth * cfg->analog.travel_mm;
            analog_report(ap, cfg, depth, t_ns);
        }
    }
    return 0;
}

/* The earliest running debounce deadline across the board's pairs - one
 * timerfd serves them all - or 0 if none is running. */
static int analog_dev_deadline(const analog_dev_t *ad, const oid_config_t *cfg,
                               uint64_t *deadline_ns) {
    int any = 0;
    for (int p = 0; p < ad->n_pairs; p++) {
        uint64_t dl;
        if (analog_latch_deadline(&ad->pair[p], cfg, &dl) &&
            (!any || dl < *deadline_ns)) {
            *deadline_ns = dl;
            any = 1;
        }
    }
    return any;
}

/* The timer fired: tick every pair. A pair whose window has not run out
 * yet ignores it, exactly as a single pair ignores an early tick. */
static void analog_dev_tick(analog_dev_t *ad, const oid_config_t *cfg,
                            uint64_t t_ns) {
    for (int p = 0; p < ad->n_pairs; p++)
        analog_latch_tick(&ad->pair[p], cfg, t_ns);
}

/* A device that dies while it holds k1/k2 would leave its active key
 * stuck down forever; release it before dropping the device. */
static void release_stuck(machine_t *in, const keypair_t *cfg) {
    if (!(in->k1 || in->k2))
        return;
    if (in->mode == MODE_INDEPENDENT) {
        /* no active key while independent; both may be down */
        if (in->k1) {
            libevdev_uinput_write_event(uidev, EV_KEY, cfg->k1, 0);
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
        if (in->k2) {
            libevdev_uinput_write_event(uidev, EV_KEY, cfg->k2, 0);
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
        return;
    }
    libevdev_uinput_write_event(uidev, EV_KEY,
                                in->act ? cfg->k1 : cfg->k2, 0);
    libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
}

/* Bring the analog interface back after it has gone away.
 *
 * Losing it is survivable - the daemon drops to the evdev path rather than
 * dying - but it used to be PERMANENT: the evdev node returns through the
 * inotify reconcile while nothing ever retried the hidraw one, so a replug,
 * a USB glitch or a suspend left the daemon quietly running the digital
 * trill under an analog config until someone restarted it. Silent, and
 * indistinguishable from the state machine misbehaving.
 *
 * Nothing watches /sys/class/hidraw, so the evdev node coming back is the
 * signal that the board is back; the analog node is retried alongside it.
 * A failed retry is quiet (analog_dev_open's `quiet`), so this can run on
 * every reconcile pass; startup already said why the board was missing.
 *
 * The re-marking at the end is the part that is easy to miss. Devices that
 * survived the outage had in->analog cleared when the board went away, and
 * reconcile_devices only sets it on devices it OPENS - so without this the
 * analog keyboard's digital k1/k2 would flow through on top of the analog
 * path and every press would be emitted twice. */
static void analog_reconcile(analog_dev_t **ad, analog_dev_t **adp,
                             const oid_config_t *cfg, dev_list_t *devs,
                             const char *input_dir, int epfd) {
    if (!cfg_any_analog(cfg) || *ad || devs->n == 0)
        return;

    analog_dev_t *n = analog_dev_open(cfg, input_dir, -1, 1);
    if (!n)
        return;

    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = n } };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, n->fd, &ev) < 0) {
        LOG_WARN("epoll_ctl ADD analog: %s - staying on the digital path",
                 strerror(errno));
        analog_dev_close(n);
        return;
    }
    *ad = *adp = n;

    for (size_t d = 0; d < devs->n; d++)
        devs->v[d]->analog =
            libevdev_get_id_vendor(devs->v[d]->dev)  == (int)g_analog_vid &&
            libevdev_get_id_product(devs->v[d]->dev) == (int)g_analog_pid;

    LOG_INFO("Analog interface recovered - back on the analog path");
}

/* Point the latch-debounce timerfd at the analog device's current deadline,
 * or disarm it. Called after anything that can start, restart or end the
 * window: every analog drain, every timer expiry, and the device going
 * away. The deadline is on CLOCK_MONOTONIC, the clock analog_drain stamps
 * reports with, so the window measured here is the one analog_latch_update
 * started. */
static void latch_timer_sync(int tfd, const analog_dev_t *ad,
                             const oid_config_t *cfg) {
    if (tfd < 0)
        return;
    struct itimerspec its = { 0 };
    uint64_t          dl;
    if (ad && analog_dev_deadline(ad, cfg, &dl)) {
        its.it_value.tv_sec  = (time_t)(dl / 1000000000ull);
        its.it_value.tv_nsec = (long)(dl % 1000000000ull);
        if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
            its.it_value.tv_nsec = 1;      /* zero would disarm it */
    }
    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
        LOG_WARN("timerfd_settime: %s", strerror(errno));
}

static int run_loop(dev_list_t *devs, const oid_config_t *cfg,
                    const char *input_dir, analog_dev_t **adp) {
    analog_dev_t *ad = *adp;   /* cleared through adp if it disappears */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOG_ERR("epoll_create1: %s", strerror(errno));
        return -1;
    }

    /* Hotplug: any create/attrib change under input_dir (or its by-id /
     * by-path symlink dirs) triggers a reconcile pass. IN_ATTRIB matters:
     * nodes are typically root-only at IN_CREATE time and only become
     * readable once udev applies the input-group permissions. */
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) {
        LOG_WARN("inotify_init1: %s - hotplug disabled", strerror(errno));
    } else {
        static const char *subs[] = { "", "/by-id", "/by-path" };
        for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s%s", input_dir, subs[i]);
            inotify_add_watch(ifd, p, IN_CREATE | IN_ATTRIB | IN_MOVED_TO);
        }
        struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = NULL } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, ifd, &ev) < 0) {
            LOG_WARN("epoll_ctl ADD inotify: %s - hotplug disabled",
                     strerror(errno));
            close(ifd);
            ifd = -1;
        }
    }

    if (ad) {
        struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = ad } };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, ad->fd, &ev) < 0) {
            LOG_ERR("epoll_ctl ADD analog: %s", strerror(errno));
            close(epfd);
            return -1;
        }
    }

    /* The latch debounce has to run out on the clock: the board reports on
     * change, so planted, still fingers send nothing to judge it by. Only
     * created with a debounce configured in analog mode, but then kept for
     * the life of the loop whether or not the board is present, so a
     * reconnected board needs no setup. Losing
     * it is not fatal - a still plant then arms on the next report, as it
     * used to. */
    int tfd = -1;
    if (cfg_any_analog(cfg) && cfg->analog.latch_debounce_ms > 0.0f) {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        struct epoll_event ev = { .events = EPOLLIN,
                                  .data = { .ptr = &g_ep_latch } };
        if (tfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
            LOG_WARN("latch debounce timer: %s - a still plant will arm on "
                     "the next report instead", strerror(errno));
            if (tfd >= 0) close(tfd);
            tfd = -1;
        }
    }

    reconcile_devices(devs, cfg, input_dir, epfd, 1);
    if (devs->n == 0) {
        if (ifd < 0) {
            LOG_ERR("Failed to open any input device - aborting");
            if (tfd >= 0) close(tfd);
            close(epfd);
            return -1;
        }
        LOG_WARN("No %s present; waiting for hotplug",
                 cfg->auto_discover ? "matching keyboard"
                                    : "configured device");
    }

    struct epoll_event events[16];

    while (g_running) {
        int nfd = epoll_wait(epfd, events, 16, -1);
        if (nfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait: %s", strerror(errno));
            break;
        }
        int rescan = 0;
        for (int i = 0; i < nfd; i++) {
            if (events[i].data.ptr == NULL) { /* inotify fd */
                char buf[4096];
                while (read(ifd, buf, sizeof(buf)) > 0)
                    ;
                rescan = 1;
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_LATCH) {
                uint64_t expirations;
                while (read(tfd, &expirations, sizeof(expirations)) > 0)
                    ;
                if (ad) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    analog_dev_tick(ad, cfg,
                                    (uint64_t)now.tv_sec * 1000000000ull +
                                    (uint64_t)now.tv_nsec);
                }
                latch_timer_sync(tfd, ad, cfg);
                continue;
            }

            if (*(int *)events[i].data.ptr == EP_ANALOG) {
                /* The analog board going away is not fatal: release
                 * whatever it held and carry on with the evdev path. */
                if ((events[i].events & (EPOLLERR | EPOLLHUP)) ||
                    analog_drain(ad, cfg) < 0) {
                    LOG_WARN("analog device %s gone; falling back to the "
                             "digital path", ad->path);
                    for (int p = 0; p < ad->n_pairs; p++)
                        release_stuck(&ad->pair[p].machine,
                                      &cfg->pair[ad->pair[p].idx]);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ad->fd, NULL);
                    g_analog_vid = g_analog_pid = 0;
                    for (size_t d = 0; d < devs->n; d++)
                        devs->v[d]->analog = 0;
                    analog_dev_close(ad);
                    ad = *adp = NULL;
                }
                latch_timer_sync(tfd, ad, cfg);
                continue;
            }

            input_dev_t *in = events[i].data.ptr;
            int dead = 0;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("device %s gone (EPOLLERR/HUP), removing",
                         in->path);
                dead = 1;
            } else if (drain_device(in, cfg) < 0) {
                LOG_WARN("dropping %s", in->path);
                dead = 1;
            }

            if (dead) {
                for (int p = 0; p < cfg->n_pairs; p++)
                    release_stuck(&in->machine[p], &cfg->pair[p]);
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                dev_list_remove(devs, in);
                input_close(in);
                free(in);
            }
        }
        if (rescan) {
            reconcile_devices(devs, cfg, input_dir, epfd, 0);
            analog_reconcile(&ad, adp, cfg, devs, input_dir, epfd);
        }
        if (devs->n == 0 && ifd < 0) {
            LOG_ERR("No devices left and hotplug unavailable - exiting");
            break;
        }
    }

    if (tfd >= 0) close(tfd);
    if (ifd >= 0) close(ifd);
    close(epfd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void print_usage(FILE *s, const char *prog) {
    fprintf(s,
        "evtrill " EVTRILL_VERSION " - two-key trill daemon for rhythm games\n"
        "\n"
        "Grabs evdev keyboard devices, runs a state machine over each configured\n"
        "pair of keys, and re-emits everything via a single uinput virtual\n"
        "keyboard. Each pair's 'mode':\n"
        "\n"
        "    trill   with both keys held, every stroke or release of either is\n"
        "            the next note, alternating\n"
        "    last    the newest stroke wins (as sold as Snap Tap / Snappy Tappy)\n"
        "    analog  needs an analog keyboard: each key is independent, with a\n"
        "            software rapid trigger, until both are planted on the\n"
        "            backplate; then they trill until both leave it\n"
        "\n"
        "usage: %s [-h] [-A|-T] [-c CONFIG] [-i DIR]\n"
        "\n"
        "options:\n"
        "    -h          show this help and exit\n"
        "    -A          analog monitor: print live key travel depth and exit\n"
        "                (for picking thresholds; grabs nothing)\n"
        "    -T          trace one pair's travel as CSV on stdout until Ctrl-C\n"
        "                (the first analog pair, else the first pair)\n"
        "                (grabs nothing; replay it with the `replay` tool)\n"
        "    -c CONFIG   path to YAML config\n"
        "    -i DIR      directory to scan/watch for event devices\n"
        "                (default /dev/input; mainly for testing)\n"
        "\n"
        "Without -c, the config is %s, or the packaged\n"
        "default %s if that does not exist.\n",
        prog, SYS_CONFIG, DEF_CONFIG);
}

/* Resolve the config path when -c wasn't given. evtrill is a system
 * daemon, so there is one config, in /etc, and no per-user lookup: a
 * daemon reading a home directory would be configured by whoever happened
 * to own it. The packaged default applies only when the file does not
 * EXIST - one that exists but cannot be read is still the config, and
 * load_config says why it failed rather than quietly running the default. */
static const char *default_config_path(void) {
    struct stat st;
    if (stat(SYS_CONFIG, &st) == 0 || errno != ENOENT)
        return SYS_CONFIG;
    return DEF_CONFIG;
}

int
main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *input_dir   = "/dev/input";
    int         monitor     = 0;
    int         trace       = 0;

    for (int opt; (opt = getopt(argc, argv, "hATc:i:")) != -1; ) {
        switch (opt) {
            case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
            case 'A':
            monitor = 1;
            break;
            case 'T':
            trace = 1;
            break;
            case 'c':
            config_path = optarg;
            break;
            case 'i':
            input_dir = optarg;
            break;
            default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!config_path)
        config_path = default_config_path();
    LOG_INFO("evtrill %s, using config %s", EVTRILL_VERSION, config_path);

    oid_config_t cfg;
    config_init(&cfg);
    if (load_config(config_path, &cfg) != 0)
        return EXIT_FAILURE;

    if (trace) {
        int rc = analog_trace(&cfg, config_path, input_dir);
        config_free(&cfg);
        return rc;
    }

    if (monitor) {
        int rc = analog_monitor(cfg.analog.device, input_dir,
                                cfg.analog.travel_mm);
        config_free(&cfg);
        return rc;
    }

    char devdesc[32];
    if (cfg.auto_discover)
        snprintf(devdesc, sizeof(devdesc), "auto-discover");
    else
        snprintf(devdesc, sizeof(devdesc), "%zu device(s)", cfg.n_devices);
    LOG_INFO("Config: %s, %d pair%s", devdesc, cfg.n_pairs,
             cfg.n_pairs == 1 ? "" : "s");
    for (int i = 0; i < cfg.n_pairs; i++) {
        const keypair_t *pc = &cfg.pair[i];
        const char *k1n = libevdev_event_code_get_name(EV_KEY, pc->k1);
        const char *k2n = libevdev_event_code_get_name(EV_KEY, pc->k2);
        LOG_INFO("  %skeys %s(%d) %s(%d), mode %s", pair_tag(&cfg, i),
                 k1n ? k1n : "?", pc->k1, k2n ? k2n : "?", pc->k2,
                 mode_name(pc->mode));
    }

    /* Realtime priority. Under the packaged unit systemd has already set
     * it (CPUSchedulingPolicy=), so the unit needs no capability for it and
     * this only reports. Run by hand, it is still worth asking for. */
    if (sched_getscheduler(0) == SCHED_FIFO) {
        LOG_INFO("Running with SCHED_FIFO realtime priority");
    } else {
        struct sched_param sp = { .sched_priority = 90 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
            LOG_WARN("Could not set SCHED_FIFO (%s); running with the "
                     "standard scheduler", strerror(errno));
        else
            LOG_INFO("Running with SCHED_FIFO realtime priority");
    }

    /* Single virtual keyboard. Created before any grabs: device discovery
     * runs inside run_loop and skips it via is_evtrill_output. */
    if (build_virtual(&cfg) != 0) {
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    /* handlers for graceful shutdown. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Open the analog interface before the first reconcile pass, so newly
     * grabbed keyboards already know their k1/k2 are analog-driven. */
    analog_dev_t *ad = NULL;
    if (cfg_any_analog(&cfg)) {
        ad = analog_dev_open(&cfg, input_dir, -1, 0);
        if (!ad)
            LOG_WARN("Analog input unavailable - analog pairs fall back "
                     "to a digital trill");
    }

    /* run_loop opens/grabs devices itself (initial reconcile + hotplug). */
    dev_list_t devs = { 0 };
    LOG_INFO("Running.");
    int rc = run_loop(&devs, &cfg, input_dir, &ad);

    analog_dev_close(ad);
    LOG_INFO("Shutting down");

    destroy_virtual();
    for (size_t i = 0; i < devs.n; i++) {
        input_close(devs.v[i]);
        free(devs.v[i]);
    }
    free(devs.v);
    config_free(&cfg);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
