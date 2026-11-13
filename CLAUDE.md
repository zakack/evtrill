# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`evtrill` is a single-file C11 daemon that acts as a SOCD (Simultaneous
Opposing Cardinal Directions) cleaner for PAIRS of keyboard keys — one pair
by default, up to `MAX_PAIRS` (8), each an independent machine. It exclusively
grabs evdev keyboard devices at the kernel level (before Xorg/Wayland see
them) — either an explicit config list or, by default, every
keyboard-shaped device advertising every configured key, with
inotify-driven hotplug in both cases — applies a SOCD state machine to each
pair of configured physical keys — "toggle" (last-input + reverting toggle; "on"
is a synonym), "snappy" (last input wins, no latching), "analog" (toggle,
but driven by travel depth read from an analog keyboard), or "off" (no
cleaning, just the k1/k2 -> v1/v2 remap), selected per pair (defaulting to
the top-level `socd` config field) — and re-emits all input through a
single uinput virtual keyboard. Built for rhythm games (osu!) where fast key
alternation needs a "rocking" input pattern instead of naive SOCD handling.

There is NO audio in this daemon. The PipeWire hitsound subsystem (sampler,
voice pool, trigger ring) was excised on this branch along with everything
that existed to serve it — see "What the audio excision took with it" below
before adding anything that looks like it is missing.

The entire implementation lives in `evtrill.c` (~2900 lines) — there is
no multi-module structure.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Produces `build/evtrill`. Requires pkg-config-visible dev packages for
`libevdev` and `yaml-0.1` (libyaml), and nothing else.

There is no test suite, linter, or CI config in this repo — validate changes
by building and manually exercising the daemon (see Running below).

## Running

The daemon is designed to run as an unprivileged user in the `input` group:
that covers read access to `/dev/input/event*`, and the packaged udev rule
(`70-evtrill-uinput.rules`) opens `/dev/uinput` to the same group. It
has no need of root and should not be run as a root system service. The
systemd unit (`evtrill.service.in`, configured by CMake) is a *user* unit:
`systemctl --user enable --now evtrill`.

```sh
./build/evtrill -c config.yaml
```

Without `-c`, the daemon looks for
`$XDG_CONFIG_HOME/evtrill/config.yaml` (`~/.config` if unset), then
falls back to the installed default (`/usr/share/evtrill/config.yaml`;
CMake bakes the real prefix in via the `DEF_CONFIG` compile definition). See `config.yaml` for the schema: `devices` (optional list of
`/dev/input/by-id/*` paths; omitted or `auto` means auto-discovery), `mode`
(the default for every pair: `trill`, `last` or `analog`; default `trill`),
`pairs` (REQUIRED, 1-8 entries of `{keys: [a, b], mode?, hid?: [a, b]}` —
see "Pairs" below), `uinput` (virtual device name), `analog` (analog-mode
thresholds; see below). THE SCHEMA IS CLOSED: `map_check` runs on every
mapping, and an unknown key or a key given twice is a load error. There are
no synonyms and no legacy spellings (`socd`, `keys:`, `k1`/`v1`,
`analog.hid_k1`, `toggle`/`snappy`/`off`/`on`); those are just unknown keys
or bad values now. Adding a config field means adding it to that mapping's
allow-list as well as parsing it, or every config that uses it is refused.

VOCABULARY, mid-migration: the config says `trill`/`last`/`analog`; the
code still says `SOCD_TOGGLE`/`SOCD_SNAPPY`/`SOCD_ANALOG`, `socd_apply`,
`keypair_t.socd`, and "toggle" in comments, until the rename pass.
`SOCD_OFF` is not a config mode: it is the analog path's unarmed state, each
key independent and one-to-one. A pair's keys are emitted under their OWN
codes — there are no separate virtual codes (v1/v2 were removed: the
physical keys are consumed, so v != k was only ever a key remapper, which
the game's keybinds and keyd/kanata already are). Wherever this file says
"virtual key" or v1/v2, read "the pair's key as emitted". After
editing config, restart via
`systemctl --user restart evtrill`. The `-i DIR` flag overrides the
scanned/watched device directory (default `/dev/input`) — mainly for
integration testing against a directory of symlinks to synthetic uinput
nodes. The `-A` flag runs the analog monitor (live travel depth, no grab,
no uinput device) for picking thresholds and discovering HID usage ids.
`-T` is the same passivity with machine-readable output: CSV of ONE pair's
k1/k2 travel per report (`cfg_trace_pair`: the first analog pair, else pair
0 — `replay` replays the same one), for replaying a session offline — the header is
`us,k1_mm,k2_mm` and every recorded trace under `tests/` is in that format.
The `#` header anchors the trace in time: `t0_monotonic_ns` and
`t0_realtime_ns` are read back to back just before it is written, and `us`
counts from the monotonic one. So the first `us` is small but not 0, and the
corpus traces simply have no anchor. Anything that reports a time within a
trace should measure it from the first report, as `replay` does.
The header also carries `config_path` and a `# analog k=v ...` line of
every threshold the front-end reads, spelled as config paths. With no `-c`
and a COMPLETE set, `replay` builds its config from `config_init` plus those
values and never opens a config file; `-c` overrides them and notes the
differences. A partial set is ignored rather than mixed with defaults. If
the analog front-end starts reading a new config field, add it to both
`analog_trace` and `rec_keys` in `tools/replay.c`, or recorded traces will
quietly replay it at its default. A field added AFTER the `# analog` line
existed goes past `REC_REQUIRED` and is optional: an older trace lacking it
played under its default, so absence is a known value, and requiring it
would make every such trace a partial set and discard its thresholds
(`latch_debounce_ms` is the first). The corpus has no `# analog` line, so it
still reads the config exactly as before.
The header ends with `# latch_arm=deadline`, which records a RULE rather than
a threshold: this binary arms a debounced latch when the window runs out on
the clock. A trace without the line was played by a daemon that had no timer
and armed on the next report, and `replay` reproduces THAT when it replays a
session as played (recorded thresholds, no `-c`) — otherwise every arm moves
earlier and latches appear that never happened (27 of the 56 traces recorded
before the marker existed, all of those with a non-zero debounce). With `-c`,
or with no recorded thresholds, the question is about the daemon as it stands
and the rule is today's. `-a deadline|report` overrides either, and the `-e`
stream says which ran (`# latch_arm=`). The switch is `arm_on_report` on
`analog_dev_t`: it makes `analog_deep_tick` a no-op and `analog_deep_deadline`
report nothing, which is the whole of the old behaviour, and ONLY `replay`
sets it. If the arm rule changes again, give the header a new value; do not
reuse `deadline`. The line sits after `# analog` deliberately — outside
readers take the first eight header lines, and it must not push anything
they look for past that.
`tools/replay.c` consumes it — built as part of `all` (see below), and NOT
installed. It `#include`s `evtrill.c` and stubs only the uinput writes, so
it exercises the daemon's real state machine rather than a copy that could
drift; keep it that way.
`analog_report` is the per-report path — feed, resolve the latch, route
the edges — split out of `analog_drain` so BOTH tools call the real
thing. They used to restate that ordering inline, which is precisely the
drift the `#include` is meant to prevent; keep it that way. The only thing
either tool stubs is `libevdev_uinput_write_event`.

`tools/regimetest.c` is the same trick as a unit test — assertions over the
`deep` latch and its mode transitions, run with `./build/regimetest`; case K
drives `analog_drain` itself from encoded HID reports over a pipe, since
every other case (and `replay`) calls the front-end directly and would not
catch a fault in the read/decode path. Cases M and Q call `socd_apply`
directly, the way `process_event` does: M is the digital toggle and the only
`snappy` coverage, Q is autorepeat (forwarded in `off`, dropped by the
state-machine modes). `tests/*.csv` are recorded `-T`
sessions named `<style>_<name>_Nphys_Mvirt.csv`, where N is the physical
presses played and M the virtual presses the daemon must emit — `replay`
prints M, so the filename is the expected result. The `tap_*` traces must
report ZERO latches; the `toggle_*` traces must latch and must emit MORE
virtual presses than physical.

`replay -e FILE` (`-` for stdout, which moves the report to stderr) also
writes the session as CSV `us,event,value`: every virtual key transition
AFTER the kernel's no-change filter, so it is what a reader of the device
received, plus `deep,1`/`deep,0` rows for the latch. It is hooked in
`replay_emit` and needs nothing from `evtrill.c` — `analog_deep_set` flips
`deep` before writing its reconciling releases, so checking the field ahead
of each write puts the latch row first. Times count from the first report;
for an anchored trace the header's `origin_*_ns` place that report on each
clock. The sweep runs with the stream closed.

Both tools build as part of `all`, deliberately: they `#include evtrill.c`, and
that guarantee is worthless if the binaries can go stale.

## Architecture

Everything is in `evtrill.c`, organized into clearly delimited
sections (search for the `/* --- */` banner comments):

1. **Config** (`load_config` and friends) — parses YAML via libyaml's
   document API (not the streaming API) into an `oid_config_t`. Key codes
   accept either symbolic names (resolved via
   `libevdev_event_code_from_name`) or raw integers.

2. **Input devices** (`input_try_open`/`input_close`/`reconcile_devices`) —
   devices are opened, wrapped in `libevdev` handles, and exclusively
   grabbed (`LIBEVDEV_GRAB`) so events don't leak to the rest of the
   system. `reconcile_devices` is the single (re)open path, run at startup
   and again on every inotify event under the input dir: in explicit mode
   it retries any configured path not currently open; in auto-discovery
   mode it scans `event*` nodes and grabs those passing `auto_grab_ok`
   (has both k1 and k2, no `EV_REL`/`EV_ABS`, not `BUS_VIRTUAL`). In BOTH
   modes `is_evtrill_output` refuses any device whose uniq is
   `"evtrill"` or whose name matches the configured uinput name —
   grabbing our own output would be an instant feedback loop. If any key
   is physically down at open time (`any_key_down` — e.g. the Enter that
   launched the daemon), the grab is *deferred*: the fd stays open
   ungrabbed (`in->grabbed == 0`), `drain_device` discards its events
   (the system still receives them directly), and the grab completes as
   soon as the device reports all keys up — grabbing mid-press would
   swallow the release and leave the raw key stuck. Open devices
   are deduped by `st_rdev` (a node reached via two symlinks is grabbed
   once) and tracked in a `dev_list_t` of stable heap pointers (epoll user
   data points at the entries).

3. **Virtual uinput device** (`build_virtual`) — a single synthetic
   keyboard is created with a fixed keyboard-wide key set (every `KEY_*`
   code, skipping the `BTN_*` pointer/gamepad ranges) plus the two
   configured virtual keys (v1/v2). The set is fixed rather than a union
   of source devices because uinput capabilities are immutable after
   creation and hotplugged keyboards may carry codes the startup set
   lacked. All non-k1/k2 events are mirrored through verbatim.

4. **Radio-button state machine** (`process_event`) — the core logic. Each
   input device tracks its own `k1`/`k2`/`act` (which virtual key is
   currently "on") state independently. The transition is computed as
   `state = k1 + k2 + event.value` after updating k1/k2, giving four cases
   (`S_NONE`, `S_RELEASE`, `S_SINGLE`, `S_PRESS`) that decide whether to
   emit a virtual key-down, a release, or an release-then-press pair (the
   "reverting toggle" that recreates a keypress when the active key is
   released while the other is still held). The `S_RELEASE` case is
   mode-dependent: `SOCD_TOGGLE` reverts no matter which of the two held
   keys was released (latching), while `SOCD_SNAPPY` ("last input wins")
   only reverts when the *active* key is released — releasing the
   already-suppressed key emits nothing. Autorepeat (`value == 2`) is
   ignored to avoid corrupting state. `SOCD_OFF` bypasses the state
   machine before any of this: k1/k2 are remapped one-to-one to v1/v2
   (autorepeat included), and `k1`/`k2` are still tracked so
   `release_stuck` works. `socd_apply` and `process_event` return nothing:
   their old return value named the voice to click and had no other reader.

5. **Analog input** (`analog_*`, `hid_map_*`) — optional path used only by
   `socd: analog`. Analog keyboards report per-key travel on a
   vendor-defined HID interface (usage page `0xFF53` v2 / `0xFF54` v1); the
   kernel makes no evdev node for a vendor usage page, so it exists purely
   as `/dev/hidrawN` — a pollable fd that joins the same epoll set, and one
   `EVIOCGRAB` has no bearing on. No vendor SDK and no kernel driver is
   involved. `analog_open` finds the node by matching the Wooting VID in
   sysfs `uevent` then scanning `report_descriptor` for the vendor usage
   page. `analog_decode` unpacks 16 slots per report. **A released key
   vanishes from the report rather than reporting a final zero**, so rest is
   inferred from absence — `analog_drain` rebuilds the full depth of k1/k2
   each frame rather than tracking deltas. `analog_key_feed` is the per-key
   front-end, and it runs one of two rules depending on the `deep` latch.
   TAPPING uses `actuation_mm` plus a software rapid trigger anchored on
   `rt_extreme` (`press_mm`/`release_mm`). RIDING uses nothing but the
   backplate: a key is down while it is against the floor and up as soon as
   it leaves, no rapid trigger at all, since riding bottoms out every stroke
   by definition and every note in a burst should land on the same physical
   line. `analog_deep_update` owns the latch (arm: both keys on the
   backplate in one report; disarm: BOTH off it — one finger lifting clear
   is a stroke, not an exit), and `analog_deep_set` reconciles across the
   mode change, release-only in both directions, parking both front ends on
   the way out.
   k1/k2 -> HID usage
   mapping is derived from the keyboard's own keymap via `EVIOCGKEYCODE_V2`
   (hid-input stores the HID usage as the scancode), not a hardcoded table.

6. **Event loop** (`run_loop`/`drain_device`) — a single-threaded
   `epoll`-based loop multiplexes all grabbed devices plus an inotify fd
   watching the input dir (and its `by-id`/`by-path` subdirs) with
   `IN_CREATE | IN_ATTRIB | IN_MOVED_TO`; `IN_ATTRIB` matters because a
   hotplugged node is typically root-only until udev applies the
   input-group permissions, so the first open gets `EACCES` and the chmod
   retriggers the reconcile pass. Devices are drained with
   `libevdev_next_event` (handling `LIBEVDEV_READ_STATUS_SYNC`
   dropped-event resync) until `EAGAIN`. A device is dropped from the poll
   set on error/HUP without killing the daemon — releasing its active
   virtual key first if one was held (`release_stuck`) — and the loop
   keeps running with zero devices, waiting for hotplug (it only aborts at
   startup if nothing opened AND inotify is unavailable).

`main()` wires these together: parse args -> load config -> attempt
`SCHED_FIFO` realtime priority (best-effort, warns and falls back on
failure) -> build the virtual device (safe
before any grab: discovery skips it via `is_evtrill_output`) -> open the
analog interface if `socd: analog` (before the first reconcile, so grabbed
keyboards already know their k1/k2 are analog-driven; a failure here just
warns and falls back to `toggle`) -> install
SIGINT/SIGTERM handlers -> run the event loop (which opens/grabs devices
via the initial reconcile pass and handles hotplug thereafter) -> tear
down in reverse order.

## Key invariants to preserve when editing the state machine

- `socd_apply` is the single SOCD core, shared by both input paths: the
  evdev path (`process_event`) feeds it real key events, the analog path
  (via `analog_socd_edge`) feeds it edges synthesized from travel depth.
  Adding a behavior to one path and not the other is almost always a bug.
  It takes the mode as an argument rather than reading `cfg->socd`, which is
  what lets the analog path run a shallow overlap as a plain remap.
- The `deep` latch is the ONLY thing that selects between the two modes,
  and both halves of its rule are pure depth tests on the CURRENT sample:
  it ARMS when BOTH keys are at `travel_mm - bottom_out_mm`, and DISARMS
  when BOTH are off it. No per-key commitment latch, no counter, no
  comparison of one key's depth against the other's.
- The failure being fixed is ALT-TAPPING OVERLAP, not a resting finger.
  At speed, alternating taps overlap in the middle of travel; a plain
  toggle reads that as a rock and answers the outgoing key's release by
  re-pressing its virtual key — a note nobody struck. What makes the latch
  work is that alt-tapping never puts both keys on the floor in the same
  report, because its defining shape is one finger leaving as the other
  arrives. `tools/regimetest.c` case B asserts exactly this.
- One key held deep while the other is lightly pressed or tapped is NOT a
  failure mode and never was. Two earlier branches
  (`feature/analog-input`, `feature/analog-split-rt`) were built around
  solving it — relative-depth gates, absolute-depth gates, five successive
  reformulations of "what arms the regime" — and were abandoned for it.
  Do not reintroduce a depth-comparison gate between k1 and k2.
- WHILE RIDING, THE BACKPLATE IS THE SWITCH. Rule 0 in `analog_key_feed`
  returns before any of the tapping logic: a key is down while it is
  against the floor and up as soon as it leaves, and rapid trigger does not
  run at all. That is what riding already is — you bottom out every stroke —
  so every note in a burst lands on the same hard physical line. No
  hysteresis is needed or wanted: a hard stop cannot be hovered on, the same
  property that makes the latch aimable. `actuation_mm`, `release_mm`,
  `press_mm` and `release_mm` are TAPPING-ONLY thresholds.
- ONE finger lifting clear does NOT disarm. Only both leaving the floor
  does. The earlier rule (either key back past `actuation_mm`) looked
  reasonable and was wrong: in a burst a finger routinely comes right off
  the switch while the other stays planted, which tore the latch down
  mid-gesture and sent the next note through the TAPPING profile at
  actuation. Measured on `tests/toggle_5burst_3phys_5virt.csv`: k2 sat at
  3.5000mm throughout while k1 lifted to 0.0000mm, and note 4 landed 11.9ms
  early at 0.25mm instead of at 3.44mm. Two thresholds inside one burst is
  jitter, and "how completely you lift" is an amplitude-driven rule of
  exactly the kind this design rejects everywhere else. `regimetest` cases
  F and G pin it.
- `bottom_out_mm` is load-bearing and now serves three roles by deliberate
  choice: the tapping bottom-out re-press, the latch threshold, AND the
  only threshold in force while riding. The config check refuses a zero
  value and warns above 10% of travel, where a firm tap starts to reach it
  and alt-tapping could arm the latch.
- `analog.latch_debounce_ms` WRAPS THE ARM TEST AND NOTHING ELSE. Both
  keys must stay on the backplate for that long, judged on report
  timestamps (`t_ns`, threaded into `analog_deep_update`), before `deep`
  arms; either key leaving the floor restarts the window. `analog_drain`
  stamps each report on arrival (`CLOCK_MONOTONIC`; hidraw carries no
  timestamp), and this is the only reader of that stamp. It filters two
  hard taps that both graze the floor in one report, not a resting finger,
  and it compares no depths across keys. The window runs out on the CLOCK,
  not on the next report: the board reports on change, so two planted,
  still fingers send nothing, and judging it per report left a still plant
  unarmed until something moved - and if that was the rock's first lift,
  the plant was cancelled and the rock never armed (#12). `run_loop` keeps a
  timerfd on `analog_deep_deadline` (`latch_timer_sync`) and arms from
  `analog_deep_tick` when it fires; `analog_report` also ticks with the
  report's own time before feeding, so a report landing after an unserviced
  deadline is judged under the toggle, and `replay` fires the tick at the
  deadline itself. While it runs the pair is still on the plain remap, so a
  long value delays the toggle by exactly that long and no more; a rock
  whose first lift comes inside the window does not arm. Nothing presses on
  arming; `analog_deep_set`'s reconciliation is release-only. Disarming is
  never debounced. The pending state is a flag (`plant_pending`), not
  `plant_ns == 0`, because `regimetest` passes `t_ns = 0` on every report.
  Default 0 is the old instantaneous test, which is what keeps every other
  case and the corpus unchanged; `regimetest` case S covers the window with
  periodic reports, case T a plant that then sends nothing, and case U the
  pre-timer rule `replay` runs old traces under (`arm_on_report`). The window
  is computed in ONE place (`analog_debounce_ns`, whole nanoseconds, rounded
  up) so the deadline and the arm test cannot disagree: a tick exactly on
  the deadline must arm, which `replay` depends on.
- The config check still requires `actuation_mm` to sit above the
  backplate, though no longer because actuation disarms the latch: an
  actuation line at or past the floor means a key could reach the backplate
  without ever having actuated, so the plain remap would never press it.
- The latch is resolved once per sample in `analog_deep_update`, called from
  `analog_drain` after both keys are fed and BEFORE that sample's edges are
  applied. Both directions need that ordering. Disarming first is the one
  that bites: with both keys leaving the floor in the SAME report, routing
  the edges first gives `S_RELEASE` then `S_NONE` — `v2↑ v1↓ v1↑`, a
  phantom press on the way out — while disarming first reconciles to `v2↑`
  and lets both release edges land as no-change writes.
- RECONCILIATION IS RELEASE-ONLY IN BOTH DIRECTIONS, and that is what the
  disarm rule buys. ARMING goes from OFF's two keys down to the toggle's
  one, so planting is silent. DISARMING goes from the toggle's one to none:
  under "both off the floor" there is by definition nobody still riding, so
  there is no survivor to strand and nothing to press. The older rule could
  fire with the other finger planted deep, so it HAD to press to avoid
  stranding that hold — at the cost of a second note for one lift. Nothing
  here presses.
- DISARMING MUST PARK BOTH FRONT ENDS: `live = 0` with `rt_extreme` left at
  the current depth. A key can be well down at disarm (it need only have
  left the backplate) and the tapping profile is about to judge it against
  `actuation_mm`. Clearing `live` alone lets rule 3 read that depth as a
  FRESH actuation and press a finger on its way up — a phantom note landing
  exactly where a slider ends. Leaving `live` set is no better: its eventual
  release emits an up with no matching down. The parked state is rule 2's
  "engaged but not live", which emits nothing while rising and re-presses
  only on a real stroke. `regimetest` case H3 is the slider pattern.
- `act` needs NO lazy binding, and that is a consequence of the backplate
  rule rather than a separate fix. Arming leaves the picture in the
  toggle's canonical form — exactly one virtual down, `act` naming it
  truthfully — and every later edge is a plain backplate crossing, so
  `up(act)` is always a real release and `down(!act)` always a real press,
  whichever finger moves. An earlier design released one virtual at arming
  and then rebound `act` to the first key to DIP; when that was the other
  key, `act` named a key whose virtual was already up and the handover
  wrote up(already-up) + down(already-down), which the kernel dropped. That
  cost one note per rock. Do not reintroduce `act_bound`, and do not add a
  cycle to repair a no-op that can no longer happen.
- `last_press` decides which virtual SURVIVES arming, and it must stay the
  most recently pressed key. The second key presses at `actuation_mm` and
  the latch arms when it reaches the backplate, so only that travel
  separates its key-down from the arming release — 19ms and 24ms in the
  recorded traces. Keeping the first-planted key instead emits a key-up
  19ms after the key-down a note just landed on, short enough for a game to
  miss the press. The older virtual has been down an order of magnitude
  longer and costs nothing to release. It no longer affects whether a beat
  lands, but it decides which virtual key leads a burst, so it must
  stay deterministic. `regimetest` case L pins it in both directions.
- The `S_RELEASE` transition at the first crossing needs no forcing. Both
  keys are still tracked as held (`SOCD_OFF` maintains `socd.k1`/`k2` too),
  so `state == k1 + k2 + value == 1` computes it in the existing machine.
  The disarm release falls out the same way.
- Arming must never be evaluated at a press edge, however tempting. The
  edge fires at `actuation_mm`, before the key has travelled, so any test of
  the incoming key's depth there hinges on where presses happen to land. The
  per-sample check exists precisely so the second key can reach the
  backplate on its own schedule.
- Gating the *emission* of a press is not a substitute for gating the
  mode: two presses that both reach `socd_apply` still trigger the toggle,
  which is what turns an incidental overlap into an extra keypress.
- Latch state keys off the sample's depths and `live`, NOT off
  `socd.k1`/`socd.k2`. Those track edges already applied and lag the
  front-end by a sample.
- In analog mode the digital k1/k2 of the analog keyboard MUST be dropped
  (`in->analog`, set by matching vendor/product in `input_try_open`), or
  every press is emitted twice — once from evdev and once from analog. Only
  for ANALOG-MODE pairs: a digital-mode pair on the same board has nothing
  else driving it (`process_event` checks the pair's mode).
- `analog_key_feed` must keep `live` in sync with the edges it actually
  emits; `socd_apply` assumes strict press/release alternation per key.
- `release_mm` is DERIVED, never configured: `analog_config_resolve` sets
  it to `actuation_mm * ANALOG_RELEASE_RATIO`. A fixed offset (Keychron's
  `actn_pt - 3`) cannot work here because actuation ranges from 0.1mm to
  most of the travel, and a subtraction suiting one end goes negative at
  the other. A warning fires if the derived value lands under
  `ANALOG_RELEASE_FLOOR`, where the firmware stops reporting at all.
- `analog_key_feed` decides TAPPING edges on `actuation_mm`/`release_mm`
  and the rapid trigger ALONE, and RIDING edges on the backplate alone. It
  does not inspect the other key in either mode; the only cross-key
  decision in the analog path lives in `analog_socd_edge`. An earlier
  revision gated presses on the other key's depth to filter a resting
  finger's dip. That was solving a non-problem, and it filtered on depth,
  where a deliberate rock and an incidental overlap are identical. Do not
  bring it back.
- KNOWN RESIDUAL, accepted: armed with one finger resting on a bottomed
  key, taps on the other that never reach the floor register nothing. It
  requires having planted both first, it clears the moment the resting
  finger leaves the floor, and during a real burst every stroke bottoms out
  by definition. Untested in play as of this writing.

## Key invariants to preserve when editing `process_event`

- `k1`/`k2`/`act` state lives in a `socd_state_t` per PAIR per input
  source (`input_dev_t.socd[]`, and `analog_pair_t.socd`), not global —
  multiple physical keyboards are tracked independently even though they
  share one virtual output device. `pair_of_key` routes a code to its pair.
- `MSC_SCAN` aside, a key in no pair is passed through untouched; a key in a
  pair is consumed and emits only that pair's v1/v2.
- Every emitted `EV_KEY` write must be followed by an `EV_SYN`/`SYN_REPORT`
  before the next state-changing write, otherwise userspace sees coalesced
  events.
- `MSC_SCAN` events are dropped rather than mirrored; all other non-k1/k2
  events pass through untouched.
- The daemon must never grab an evtrill output device
  (`is_evtrill_output`, keyed on uinput uniq `"evtrill"` and the
  configured device name) — every path that opens an input device has to
  keep going through this guard, in explicit mode too, or emitted events
  feed straight back in as input (infinite feedback loop).

## Pairs

- A PAIR IS THE OLD DAEMON, WHOLE. `keypair_t` is `{k1, k2, socd, hid_k1,
  hid_k2}` (config: `keys`, `mode`, `hid`); everything downstream of `pair_of_key` (digital) or the
  per-pair depth rebuild in `analog_drain` (analog) is the single-pair code
  run on that pair's state. `socd_apply` takes the pair rather than the
  config, and nothing in the core, the front-end or the latch knows how many
  pairs exist. If a change wants one pair to look at another, that is a
  smell: pairs are independent by definition.
- A ONE-PAIR CONFIG MUST BE EVENT-FOR-EVENT IDENTICAL to the pre-pairs
  build; that is what keeps the corpus and every recorded trace meaningful.
  The spelling changed (one pair is a one-entry `pairs:`) but the machine
  did not. `replay`'s as-played path never reads a config (`config_init`
  is a one-pair Z/X default for the tools), and with `-c` a one-entry
  `pairs:` copy of the old config replays identically to
  `experiment/no-audio`: 264/264 over the analysis traces and the corpus,
  under the default, `-a deadline`, `-a report` and `-c` — with the old
  build's F13/F14 labels normalised to Z/X, since keys now emit as
  themselves.
- `analog_dev_t` is the BOARD (fd, format, ids) plus `analog_pair_t pair[]`,
  one per analog-mode pair; `analog_pair_t` holds everything the latch
  functions touch, plus `idx` into `cfg->pair[]`. A memset pair is pair 0,
  which is what the tools' `fresh()` pattern relies on. Each report is read
  and decoded once and handed to every pair in index order.
- ONE TIMERFD SERVES EVERY PAIR: `analog_dev_deadline` is the earliest
  pending deadline, `analog_dev_tick` ticks them all, and a pair whose window
  has not run out ignores the tick exactly as one pair ignores an early one.
- The analog THRESHOLDS are shared — they describe the board — and the HID
  overrides are per pair.
- `pairs_check` refuses a key in two pairs (cross-talk: two machines on one
  key, each releasing what the other holds), both keys of a pair the same,
  and one `hid` override in two places.
  A collision between an override and a keymap-derived usage only exists
  once the board is open, so `analog_dev_open` checks every resolved usage
  pairwise as well.
- The reconnect retry (`analog_reconcile`) opens the board with `quiet` set,
  since it runs on every rescan of the input dir and a board that is still
  unplugged is not news; startup opens it loud.
- KNOWN, by construction rather than by pairs: on an analog pair with no
  debounce, both keys landing on the floor in ONE report arm the latch
  before either press edge is routed, so the reconciliation presses one key
  and the other gets a ZERO-LENGTH press (down and up in one report). Two
  notes, never three, but a game may miss the short one. That is a taiko big
  note; `latch_debounce_ms` > 0 keeps it on the plain remap. `regimetest`
  case W pins both.
- Coverage: `regimetest` W (two analog pairs through `analog_drain`, the
  shared timer, the big note), X (two digital pairs through `process_event`,
  the mixed board, `release_stuck`), Y (the config parser: the closed schema,
duplicates, and every cross-pair refusal, each asserted on the message it
logs so a case cannot pass by failing for some other reason).

## What the audio excision took with it

The daemon used to play a hitsound on every virtual key-down: one PipeWire
stream, a 32-voice mixer, an SPSC trigger ring, libsndfile decoding and
libsamplerate conforming. All of it is gone, and so is everything that
existed only to feed it. None of the following is an accident, and none of
it should be "restored" without restoring audio itself:

- NO VOICE RETURN VALUES. `socd_apply`, `process_event` and
  `analog_socd_edge` are `void`. They used to return `VOICE_V1`/`VOICE_V2`
  so the caller could trigger a sample.
- NO CLICK GATE IN `analog_socd_edge`. It computed the virtual picture
  before and after each edge to suppress a click whose press the kernel
  would drop as a no-change write. With nothing to click, the function is
  the mode selection, `last_press`, and one `socd_apply` call.
- NO `ANALOG_CLICK` HOOK, and `regimetest` has no `expect_clicks`. Every
  click assertion sat directly after an `expect_log` on the same report, and
  a press surviving the harness's no-change filter was equivalent to a click
  by construction, so the emit log already pins all of it.
- EVDEV IS BACK ON ITS DEFAULT CLOCK. `input_try_open` no longer calls
  `libevdev_set_clock_id(CLOCK_MONOTONIC)`: that existed so a trigger could
  be placed at the frame its key event fell on. Nothing reads
  `input_event.time` now. Analog reports are still stamped on arrival,
  because the latch debounce reads that.
- NO `mlockall`. It was there to keep the RT audio callback from page
  faulting. `SCHED_FIFO` stays; it serves the input path.
- NO EVENTFD IN THE EPOLL SET, and no `EP_AUDIO` tag. The loop multiplexes
  input devices, the analog hidraw node, inotify, and the latch-debounce
  timerfd (`EP_LATCH`) — nothing else. The timerfd is not an audio leftover:
  it is what arms a debounced latch on the clock.
- NO `<stdatomic.h>`, AND NO SECOND THREAD. The trigger ring was the only
  state shared across threads; the daemon is single-threaded again, full
  stop.
- `<time.h>` AND (in `tools/replay.c`) `<math.h>` ARE INCLUDED EXPLICITLY.
  Both had been arriving through the PipeWire headers.
- `tools/mixtest.c`, `click.wav`, the `DEF_WAV` definition, the `audio`
  config section and its parser, and the unit's `Wants=`/`After=` on
  `pipewire.service` are all removed.
