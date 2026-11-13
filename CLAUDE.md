# CLAUDE.md

evtrill is a single-file C11 daemon (`evtrill.c`) for rhythm games: it grabs
keyboards at the evdev level, runs each configured **pair** of keys through
its own state machine, and re-emits everything through one uinput virtual
keyboard. A pair's keys emit under their own codes; the machine decides only
when they go down and up.

**`GLOSSARY.md` is the project's vocabulary.** Use its terms in code,
comments, commit messages and docs, and treat its _Avoid_ lists as binding:
a note is what the virtual keyboard emits, a stroke is what a finger does,
the latch is armed or independent.

## Build and test

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
./build/machinetest                                   # must print "all assertions passed"
for t in tests/*.csv; do ./build/replay -q -c tests/corpus.yaml "$t"; done
```

A change to the state machine, the analog front-end or the latch is done when
`machinetest` passes AND every corpus trace reproduces the counts in its
filename (below). The build has no warnings at `-Wall -Wextra`; keep it that
way.

The tools build as part of `all` on purpose. `replay` and `machinetest`
`#include "evtrill.c"` and stub only `libevdev_uinput_write_event`, so they run
the daemon's own code. Keep it that way: logic the tools need belongs in
`evtrill.c`, called by both (`analog_report` is the per-report path both
tools and `analog_drain` share).

- **`machinetest`** — assertions, cases A–V. Each case prints its letter and
  what it pins; read the case before changing the behaviour it names. It logs
  what the virtual keyboard receives, by key name (`Z- X+`), after the kernel's
  no-change filter. Case M drives `analog_drain` from encoded HID reports over
  a pipe, the only coverage of the read/decode path; case V is the config
  parser, asserting each refusal on the message it logs.
- **`replay TRACE.csv`** — runs a `-T` trace through the analog front-end and
  reports latch arms, notes, stroke depths and a `backplate_mm` sweep. `-e
  FILE` writes the session as `us,event,value` rows: every key transition the
  virtual keyboard received, plus `armed,1`/`armed,0` rows for the latch.
  `-a deadline|report` picks the arm rule (see Traces).
- **The corpus**, `tests/*.csv`, named `<gesture>_<name>_<N>strokes_<M>notes`:
  `replay` must print M notes. `alttap_*` must arm the latch zero times;
  `rock_*` arm it and emit more notes than strokes. They are valid only under
  `tests/corpus.yaml`'s thresholds, and they are the regression gate on
  everything analog.

A running `evtrill.service` grabs any keyboard carrying all of its pairs'
keys, including a synthetic uinput test device. If an end-to-end test against
`-i DIR` sees nothing come out, check `systemctl status evtrill` first, or use
keys no live pair uses.

## Running

evtrill is a system service: `evtrill.service` runs as a `DynamicUser` in the
`input` group, with `SCHED_FIFO` set by systemd and device access limited to
`char-input` (read), `char-hidraw` (read) and `/dev/uinput`. `70-evtrill.rules`
opens `/dev/uinput` to `input` and grants `input` read access to Wooting
hidraw nodes as an ACL entry. Those nodes carry uaccess ACLs from other
packages, so grant access with `setfacl` there: a `MODE`/`GROUP` assignment
becomes the ACL mask and takes the desktop user's write access away.

Without `-c` the config is `/etc/evtrill/config.yaml` if it exists, else the
packaged default; CMake bakes both paths in (`SYS_CONFIG`, `DEF_CONFIG`). A
config that exists but fails to load is an error, never a fall back to the
default. For development, run `./build/evtrill -c config.yaml` as root or as a
user in `input`. `-A` (live depth monitor) and `-T` (trace) grab nothing and
create no virtual keyboard. `-i DIR` replaces `/dev/input` as the directory
scanned and watched, for testing against symlinks to synthetic devices.

## Config

`config.yaml` is the user-facing reference. The schema is **closed**:
`map_check` runs on every mapping, and an unknown key, a repeated key, or a
bad value is a load error that names it. There are no synonyms or legacy
spellings. Adding a field means adding it to that mapping's allow-list and
parsing it. A field the analog front-end reads also goes in the trace header
(below).

Root keys: `devices`, `mode`, `pairs` (required, 1–8 of `{keys: [a, b],
mode?, hid?: [a, b]}`), `analog`, `uinput`. `pairs_check` refuses a key in two
pairs, a pair whose keys are the same, and one `hid` override in two pairs;
`analog_dev_open` repeats the usage check once keymap-derived usages exist.
`analog_config_check` refuses thresholds that combine into a broken machine:
`backplate_mm` must be positive and below travel, and `actuation_mm` must sit
above the backplate so every key that reaches the backplate actuated first.

## Traces

`-T` writes one pair's depths (`cfg_trace_pair`: the first analog pair, else
pair 0) as `us,k1_mm,k2_mm`, one row per report, under a `#` header. Old
traces are kept and replayed for years, and an outside project
(osu-replay-analysis) parses the header and the `-e` stream, so treat both as
formats:

- `t0_monotonic_ns`/`t0_realtime_ns` anchor the trace; `us` counts from the
  monotonic one. Times within a trace are measured from its first report.
- `# analog k=v ...` records every threshold the front-end reads, named by
  config path. With a complete set and no `-c`, `replay` replays the session
  as played, from `config_init` plus those values; `-c` overrides them and
  notes each difference. A new front-end field goes in both `analog_trace` and
  `rec_keys` (`tools/replay.c`), after `REC_REQUIRED`, so older traces that lack
  it still count as complete and replay it at its default. A renamed field
  keeps its old name as the `rec_keys` alias (`backplate_mm` reads
  `rapid_trigger.bottom_out_mm`).
- `# latch_arm=deadline` records the arm rule. A trace without it replays
  under the rule that daemon ran: a debounced plant arms on the next report,
  not on the clock (`arm_on_report`, set only by `replay`). If the rule changes
  again, the header gets a new value.
- Outside readers take the first eight header lines; new header lines go
  after `# latch_arm`.

## Architecture

`evtrill.c` is in sections under `/* --- */` banners:

1. **Config** — libyaml's document API into `oid_config_t`. Keys are
   `KEY_*` names (`libevdev_event_code_from_name`) or numbers.
2. **Input devices** — `reconcile_devices` is the one (re)open path, run at
   startup and on every inotify event. Auto-discovery grabs `event*` nodes
   passing `auto_grab_ok` (every pair key, no `EV_REL`/`EV_ABS`, not virtual).
   A device with a key down at open (`any_key_down`, e.g. the Enter that
   launched the daemon) is opened ungrabbed and grabbed once all keys are up,
   so its release reaches whoever saw its press. Devices are deduped by
   `st_rdev`.
3. **Virtual keyboard** — `build_virtual` creates one uinput keyboard with
   every `KEY_*` code, fixed at creation (uinput capabilities are immutable,
   and a hotplugged keyboard may carry any key). Its uniq is `"evtrill"`.
4. **State machine** — `machine_apply`, on a `machine_t` (`k1`, `k2`, `act`,
   `mode`) per pair per input source. The transition is `k1 + k2 + value`
   after updating the held key: `S_NONE`, `S_RELEASE`, `S_SINGLE`, `S_PRESS`.
   On `S_RELEASE` trill falls back on either release and last only on the
   active key's. `MODE_INDEPENDENT` mirrors each key one-to-one and still
   tracks `k1`/`k2`.
5. **Analog** — the Wooting's vendor HID interface (usage page `0xFF53` v2,
   `0xFF54` v1) exists only as `/dev/hidrawN`, read directly and polled in the
   same epoll set. `analog_open` finds it by vendor id and report descriptor.
   A released key vanishes from the report, so `analog_drain` rebuilds every
   pair key's depth from each report. HID usages come from the board's own
   keymap via `EVIOCGKEYCODE_V2`. `analog_dev_t` is the board plus one
   `analog_pair_t` per analog pair, which holds everything the latch touches.
6. **Event loop** — one epoll loop over grabbed devices, the analog board,
   inotify on the input dir (`IN_ATTRIB` included: a hotplugged node is
   root-only until udev's permissions land) and the latch timerfd
   (`EP_LATCH`). `SYN_DROPPED` resyncs through libevdev. A device that errors
   is dropped after `release_stuck` releases what it held; the loop runs on
   with zero devices. Losing the analog board drops to the digital path, and
   `analog_reconcile` reopens it (quietly) on each rescan.

## Invariants

### Both paths, one machine

- `machine_apply` is the only state machine. The evdev path (`process_event`)
  and the analog path (`analog_machine_edge`) both feed it edges, and it takes
  the mode as an argument, which is how an analog pair runs independent while
  its latch is disarmed. A behaviour belongs in it, or in both paths.
- Each pair is self-contained: `machine_apply`, the analog front-end and the
  latch see one pair and never how many exist. One pair's keys never affect
  another pair.
- A one-pair config replays the corpus and every recorded trace exactly as the
  single-pair daemon did. That is what keeps the traces meaningful.
- Every `EV_KEY` write is followed by a `SYN_REPORT` before the next
  state-changing write, or userspace sees coalesced events.
- Keys in no pair pass through untouched; `MSC_SCAN` is dropped. A pair key is
  consumed and emitted only by its machine.
- Every device open goes through `is_evtrill_output` (uniq `"evtrill"` or the
  configured name), in explicit mode too, so evtrill never grabs its own
  virtual keyboard.
- On the analog board, the digital events of an analog pair's keys are
  dropped (`in->analog` and the pair's mode), so each stroke is emitted once.
  A digital pair on the same board is still driven from evdev.
- Autorepeat (`value == 2`) is forwarded while independent and dropped by
  trill and last.

### The latch

The failure evtrill exists to prevent is the phantom note from **overlap**:
at speed, alt-tapping strokes overlap mid-travel, and a trill answers the
outgoing key's release with a fallback no finger struck. Alt-tapping never
plants (its shape is one finger leaving as the other arrives), so a plant is
the discriminator. Case B is the headline assertion.

- The latch ARMS when both keys are on the backplate (`depth >= travel_mm -
  backplate_mm`) and DISARMS when both have left it. Both are pure depth tests
  on the current report. It is the only cross-key test in the analog path:
  each key's edges come from its own depth alone, and evtrill never compares
  one key's depth with the other's. A key held down while the other strokes
  is ordinary play.
- One finger lifting clear is a stroke, so only both leaving disarms. A
  partial lift mid-burst keeps the pair armed, and every note of the burst
  lands at the backplate (cases F and G).
- `analog_latch_update` resolves the latch once per report, after both keys
  are fed and BEFORE the report's edges are routed. With both keys leaving in
  one report, disarming first turns the edges into plain releases; routing
  first would emit a fallback on the way out.
- Arming is judged per report, never at a press edge: the second key presses
  at actuation, before it has travelled to the backplate.
- **Independent** keys go down at `actuation_mm` and run rapid trigger
  (`press_mm`/`release_mm` reversal distances anchored on `rt_extreme`;
  reaching the backplate always presses, a full release always releases).
  **Armed** keys use the backplate alone: down while on it, up when they
  leave. Rule 0 of `analog_key_feed` returns before the independent rules.
  A hard stop needs no hysteresis.
- `backplate_mm` is the most important number: it is the independent
  bottom-out press, the arm line, and the only threshold while armed.
- `release_mm` (the derived release depth) is `actuation_mm *
  ANALOG_RELEASE_RATIO`, never configured, so it scales with actuation
  anywhere in the travel. It warns below `ANALOG_RELEASE_FLOOR`, where the
  firmware stops reporting.
- **Reconciliation is release-only both ways** (`analog_latch_set`). Arming
  goes from independent's two keys down to the trill's one; disarming happens
  with both keys off the backplate, so nothing is held. Neither direction
  emits a note.
- **The newest stroke survives arming.** `last_press` (a key index, recorded
  on press edges) picks which key stays down. The second key's press landed
  only the travel time (~20ms) before arming; releasing it that soon could
  cost the note.
- **Arming leaves `act` true**: exactly one key down, named by `act`, so every
  later backplate crossing is a real release or a real press.
- **Disarming parks both front-ends**: `live = 0`, `rt_extreme` at the
  current depth. A key can still be well down when it leaves the backplate;
  parked, it emits nothing on the way up and presses again only on a real
  stroke (case J, a slider).
- `analog_key_feed` keeps `live` in step with the edges it emits:
  `machine_apply` relies on strict press/release alternation per key. Latch
  decisions read the report's depths and `live`, which are current, not the
  machine's `k1`/`k2`, which lag by a report.
- Known residual: while armed with one finger resting on the backplate,
  strokes on the other key that stop short of it register nothing. It clears
  when the resting finger lifts.

### Latch debounce

- `latch_debounce_ms` wraps the arm test and nothing else: a plant must last
  that long before the latch arms, and either key leaving restarts it. It
  exists for the 2B, two keys struck together that both reach the backplate
  for an instant; a rock plants and stays. Disarming is never debounced.
  While it runs the pair stays independent.
- The window ends on the clock. The board reports only on change, so a still
  plant sends nothing; `run_loop` keeps a timerfd on `analog_dev_deadline`
  (`latch_timer_sync`) and arms from the tick. `analog_report` also ticks at
  the report's own time before feeding, so a late report is judged armed.
- `analog_debounce_ns` computes the window in one place, rounded up, so the
  deadline and the arm test agree: a tick exactly on the deadline arms.
- Pending is a flag (`plant_pending`), because the tools pass `t_ns = 0`.
- One timerfd serves every pair: `analog_dev_deadline` is the earliest,
  `analog_dev_tick` ticks all, and a pair ignores an early tick.
- With no debounce, both keys reaching the backplate in one report arm the
  latch before either press is routed, so one key gets a zero-length press.
  Still two notes, and a non-zero debounce avoids it (case T).
