# evtrill

An SOCD cleaner for rhythm games on Linux. evtrill turns two fingers rocking
on two keys into fast, clean alternation on any keyboard, and it also does
last-input-wins SOCD cleaning, the behaviour sold as Razer Snap Tap and
Wooting Snappy Tappy.

![demo](docs/demo.gif)

evtrill grabs your keyboard at the evdev level, before Xorg or Wayland see
anything, runs each configured **pair** of keys through its own state machine,
and re-emits everything through one virtual keyboard. A pair's keys keep their
own codes; evtrill only decides when they go down and up. Every other key
passes through untouched.

## Modes

Each pair runs one of three modes:

- **`trill`** (default). With both keys held, every stroke or release of
  either key is the next note, alternating. Rock two fingers between the keys
  and every lift and landing is a note.
- **`last`**. The newest stroke wins: it suppresses the older key, and only
  releasing the active key falls back to the one still held. This is Snap Tap
  and Snappy Tappy.
- **`analog`**. For analog keyboards. The keys are independent while you
  alt-tap and trill while you rock, switched by how far they are pressed. See
  [Analog mode](#analog-mode).

Up to 8 pairs run at once, each with its own mode. In osu!taiko, for example,
one pair for dons and one for kats: a don and a kat together are two notes,
never a trill.

## Install

### Arch Linux

An AUR package is coming.

### From source

Needs libevdev, libyaml, CMake 3.13 or newer, a C11 compiler and `pkg-config`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
sudo udevadm control --reload && sudo udevadm trigger
```

This installs the `evtrill` binary, a default config in `/usr/share/evtrill/`,
the `evtrill.service` system unit, and a udev rule.

## Setup

1. Copy the default config to `/etc/evtrill/` and edit it. It is commented
   throughout; [`config.yaml`](config.yaml) is the reference.

   ```sh
   sudo install -Dm644 /usr/share/evtrill/config.yaml /etc/evtrill/config.yaml
   ```

2. Start the service:

   ```sh
   sudo systemctl enable --now evtrill
   ```

After editing the config, `sudo systemctl restart evtrill`. If the daemon
refuses the config, `journalctl -u evtrill` says why: the schema is closed, so
a typo is an error rather than a silently ignored line.

### What the service can touch

evtrill runs as a throwaway system user created at start, whose only privilege
is the `input` group. That is what lets it read keyboards; the udev rule adds
`/dev/uinput`, to create the virtual keyboard, and read-only access to Wooting
boards for analog mode. Systemd sets its realtime priority, and the unit
denies it the network, capabilities, and the rest of the filesystem.

You do not need to be in the `input` group yourself.

## Configuration

```yaml
mode: trill                       # the default for every pair

pairs:
  - { keys: [KEY_Z, KEY_X] }      # osu!standard
  - { keys: [KEY_D, KEY_F], mode: last }
```

Keys are `KEY_*` names or numeric codes. No key may be in two pairs. By
default evtrill grabs every keyboard that has every pair's keys, and picks up
keyboards as they are plugged in; `devices:` pins specific ones instead.

## Analog mode

An analog keyboard reports how far each key is pressed, not just whether it is
down. That lets one pair serve both ways of playing it:

| gesture | what it is | the pair should be |
| --- | --- | --- |
| **alt-tapping** | alternating strokes, one finger each | independent |
| **rocking** | both fingers planted, rocking between the keys | trilling |

Each is wrong for the other. At speed, alt-tapping strokes overlap in the
middle of travel, and a trill answers the overlap with a **phantom note**: a
note no finger struck. Independent keys, meanwhile, give rocking nothing to
alternate.

### The latch

> **Planting both keys on the backplate is the rock, and nothing else is.**

Alt-tapping never puts both keys on the backplate at once: its shape is one
finger leaving as the other arrives, however fast you play. Rocking starts by
planting both. So each analog pair has a **latch**: it **arms** when both keys
reach the backplate together, and the pair trills; it **disarms** when both
have left it, and the keys are independent again. One finger lifting clear is
a stroke, not an exit.

The backplate is a hard stop, so this is a trigger you can aim: "plant both
keys to start rocking" is something you can actually do. Where it starts is
`analog.backplate_mm`, the most important number in the analog block.

Arming and disarming emit no notes of their own.

### Independent and armed

| latch | a key goes down | a key comes up |
| --- | --- | --- |
| independent | at `actuation_mm`, then on rapid trigger | on rapid trigger |
| armed | on reaching the backplate | on leaving the backplate |

Rapid trigger works like Wootility's: `press_mm` and `release_mm` are reversal
distances, a full release always releases, and reaching the backplate always
presses. While armed it does not run at all: every note of a burst lands at
the backplate.

### Picking thresholds

`evtrill -A` prints each key's live depth and the peak of each stroke. It
grabs nothing, so it runs alongside the service. It reads the board's hidraw
node as you: Wootility's udev rules give you that access, or run it with
`sudo`. Set `backplate_mm` so the
backplate starts just above where your fingers rest when rocking.

Then check it against your own alt-tapping: record a stream with
`evtrill -T > alttap.csv` (play, then Ctrl-C) and replay it with the `replay`
tool from the build directory:

```sh
./build/replay alttap.csv
```

It should report the latch armed **0 times**. If it arms, `backplate_mm` is
too wide for how hard you bottom out; the sweep at the end of the report shows
how the count moves with it. If 2Bs (both keys struck together on purpose)
arm it, raise `latch_debounce_ms`.

`-A` also shows each key's HID usage id. evtrill reads these from the
keyboard's own keymap, but a pair's `hid:` pins them if a key comes out wrong.

### Requirements and caveats

- **A Wooting keyboard.** evtrill reads its analog interface straight from
  `/dev/hidraw*`, with no vendor SDK or driver. Other analog boards are not
  supported yet.
- **Turn off the keyboard's own rapid trigger and SOCD settings** (Snappy
  Tappy, Rappy Snappy) for keys in analog pairs. evtrill does both, and the
  on-board versions fight it.
- Without an analog board, analog pairs fall back to `trill`, with a warning.
- While armed, with one finger resting on the backplate, strokes on the other
  key that stop short of the backplate register nothing. It clears as soon as
  the resting finger lifts, and in a real burst every stroke reaches the
  backplate.

## Running by hand

```
usage: evtrill [-h] [-A|-T] [-c CONFIG] [-i DIR]

    -h          show this help and exit
    -A          analog monitor: print live key travel depth and exit
    -T          trace one pair's travel as CSV on stdout until Ctrl-C
    -c CONFIG   path to YAML config
    -i DIR      directory to scan/watch for event devices (default /dev/input)
```

`-A` and `-T` grab nothing. To try a config before installing it, stop the
service and run `sudo evtrill -c myconfig.yaml`.

## Fair play

evtrill rewrites your input before the game sees it: what the game receives is
not literally what your fingers did. Some games, anti-cheat systems and
tournament rulesets prohibit SOCD cleaning; Valve banned the equivalent
keyboard features (Razer Snap Tap, Wooting SOCD) from CS2 in 2024, and rhythm
game communities have their own rules on input assistance. Check the rules of
whatever you play. How you use it is up to you.

## License

[MIT](LICENSE.md)
