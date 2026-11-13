# evtrill

evtrill takes over pairs of keys on a Linux keyboard and decides when each
key goes down and up, so that two fingers can alternate between two keys
faster and more cleanly than the keys alone allow. Built for rhythm games.

## Language

### Keys and keyboards

**Pair**:
Two keys run together by one state machine, under one mode.
_Avoid_: key pair, SOCD pair, pool, slot

**Unpaired key**:
A key that belongs to no pair; it passes through untouched.
_Avoid_: passthrough key, non-pair key, other key

**Active key**:
The one key of a held pair that is down on the virtual keyboard right now.
_Avoid_: act, current virtual, winner

**Physical keyboard**:
A real keyboard under a player's fingers. evtrill grabs the ones carrying
its pairs' keys.
_Avoid_: source keyboard, input device, grabbed device

**Virtual keyboard**:
The single keyboard evtrill creates and re-emits everything through; what
the game actually sees.
_Avoid_: output keyboard, uinput device, virtual output, virtual key

**Grab**:
Taking exclusive ownership of a physical keyboard, so nothing else sees its
raw events.

### Modes

**Mode**:
What a pair does with its two keys: trill, last or analog.
_Avoid_: socd, SOCD mode, regime

**Trill**:
The mode in which, with both keys held, every stroke or release of either
key is the next note, alternating.
_Avoid_: toggle, reverting toggle, rapid-fire, on

**Last**:
The mode in which the newest stroke wins: it suppresses the older key, and
only releasing the active key falls back to the one still held.
_Avoid_: snappy, Snappy Tappy, Snap Tap, last-input-wins

**Analog**:
The mode in which a pair's keys are independent until they are planted,
then trill until both leave the backplate.
_Avoid_: deep mode, auto, hybrid

**Fallback**:
A fresh note on the still-held key when the other key of a held pair is
released. Trill falls back on either release; last only on the active
key's.
_Avoid_: revert, reverting, handover, re-press

**SOCD cleaning**:
The general name for resolving two opposing held keys into one; last is a
form of it. Used for discoverability, never as the name of a mode.
_Avoid_: SOCD (bare), cleaner

### Playing

**Stroke**:
One physical press of a key by a finger.
_Avoid_: press (bare), tap, keystroke, input

**Note**:
One key-down on the virtual keyboard; what the game counts.
_Avoid_: press (bare), virtual press, beat, keypress, click, emit

**Phantom note**:
A note no stroke asked for; the failure evtrill exists to prevent.
_Avoid_: spurious note, extra press, ghost press

**Alt-tapping**:
Alternating discrete strokes between a pair's two keys, one finger each.
_Avoid_: tapping (bare), alternate tapping, tapping profile

**Rocking**:
Both fingers planted on a pair's keys, rocking between them so that every
lift and landing is a note.
_Avoid_: riding, wobbling, trilling (as a gesture), rolling

**Overlap**:
Both keys of a pair down at once mid-travel during alt-tapping, because one
finger lands before the other has lifted.
_Avoid_: incidental rock, collision

**2B**:
Both keys of a pair struck together on purpose, to hit two things at once.
_Avoid_: chord, big note, double, jump

### Analog

**Analog board**:
A keyboard that reports how far each key is pressed, not just whether it is
down.
_Avoid_: analog device, hidraw node, HE keyboard

**Travel**:
The full distance a key can move.
_Avoid_: depth (for the full distance)

**Depth**:
How far a key is pressed right now.
_Avoid_: travel (for the current position), position

**Actuation**:
The depth at which an independent key goes down.
_Avoid_: actuation point, press threshold

**Rapid trigger**:
Releasing or re-pressing a key on a reversal of direction by a set
distance, without it having to cross actuation again.

**Backplate**:
The band at the very bottom of travel, where a key is bottomed out.
_Avoid_: floor, bottom, bottom-out zone, deep

**Plant**:
Both keys of a pair on the backplate at the same moment.
_Avoid_: dual bottom-out, deep press

**Latch**:
An analog pair's switch between independent keys and trill. It arms on a
plant and disarms when both keys have left the backplate.
_Avoid_: deep, deep latch, regime, gate

**Armed**:
The latch's set state: the pair trills, and the backplate alone decides when
a key is down.
_Avoid_: riding, engaged, deep, on

**Independent**:
An analog pair with its latch disarmed: each key acts on its own, at
actuation with rapid trigger.
_Avoid_: off, plain remap, pass-through, untouched, tapping

**Latch debounce**:
How long a plant must last before the latch arms, so that a 2B which
reaches the backplate does not arm it.

**Report**:
One reading from the analog board of every pressed key's depth.
_Avoid_: frame, sample, packet

### Recording and testing

**Trace**:
A recording of one pair's depths, report by report, made with `-T`.
_Avoid_: session, recording, capture, log

**Corpus**:
The traces kept in the repo whose filenames state the result the state
machine must produce.
_Avoid_: test data, fixtures

**Replay**:
Running a trace through evtrill's own state machine offline.
_Avoid_: simulation, playback
