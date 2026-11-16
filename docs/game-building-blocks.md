# Game building blocks

Goal (owner, 2026-11-16): the device firmware holds only the building blocks
every game mechanic needs. Game and round logic comes from the console or the
hub as data. New games need a console change, not a firmware release.

Status: proposal. Builds on the game session of `src/game/` (round format v0,
gateway protocol v0).

## Fixed cases in the firmware today

| Where | Fixed case |
| --- | --- |
| `GameTypes.h` `RoundType` | Race, Scale, Choice, Move as closed list |
| `Participant::_onMemberPress` | one input rule per round type: race takes Buzz once, choice maps Up/Right/Down/Left/Enter to 1-5 (first or last counts), scale counts Up/Down with "first tap sets the middle" and optional Enter confirm, move logs direction presses |
| `Participant` | answering device of a race, countdown only for choice and plain scale, naming phase with Up/Down |
| `GameRender.h` `renderParticipant` | one LED picture per round type and phase: free keys white, chosen pulse, confirmed ring colour, scale zones red/yellow/green, rainbow scale, race winner celebration, answer timer, wrong answer orange red |
| `GameFrames.h` header flags | `firstOnly`, `winnerWrong`, `rainbow` carry game meaning instead of display or input parameters |
| `GameTypes.h` | 9 colour palette, 24 rainbow steps, wrong colour, direction colours |
| `GameConfig.h` | Buzzerboard rev A0 LED indices (column, ring, dot, keys) inside the game module |

Session handling (invite, join, admit, release, channel change, status, echo,
reliable reports, time base, orientation) is already generic and stays.

## Building blocks

### 1. Inputs

A round names its inputs and one rule per input group.

- **Sources:** keys and triggers from the board profile (Buzz, D-pad, Enter,
  shoulder keys, Hall triggers), orientation (Lage), later IMU gestures.
  Every input carries its edge time from the interrupt.
- **Rules:**
  - `once`: the first press sets a value (race).
  - `pick`: each key of the group maps to a value; `first` or `last` counts
    (choice, vote, yes/no).
  - `counter`: two keys step a value between min and max, start value, hold
    repeat, optional confirm key (scale, points).
  - `log`: every press goes into the key log (move, sequence games).
- **Send policy:** at once, after a quiet time, after a commit time, on the
  confirm key.

### 2. Values and states

- One value per device and round, plus the key log. Reports, echo and
  retries stay as they are.
- Phases announced, open, closed; open and close times; the device with
  the turn and its time; a result value per round (`correct`, `winner`).
- Device states the round can refer to: `free`, `chosen`, `confirmed`
  (logged in), `checking` (logged in, resolution pending), `locked`, `turn`,
  `right`, `wrong`, `none` (no rating: nothing in or late), `winner`.
- At the resolution the zone "right key" gets its own rule.

### 3. LED output

- **Zones** from the board profile: `column`, `ring`, `dot`, `keys` (per
  logical key, follows orientation). A lamp has one zone `all`.
- **Effects** per zone: `off`, `fill`, `bar` (n of m steps, reading
  direction), `pulse`, `blink` (max 3 Hz), `run` (head up the column),
  `timer` (runs down over the turn time, colour fades).
- **Colours:** palette index, direction colour, RGB, a colour table by value
  (zones, rainbow) or "colour of the value".
- **Display rules:** a short list `state -> zone, effect, colour`. The
  device draws the first rule that matches. The console sends them with the
  round, the firmware brings presets for the v0 round types.

### 4. Sound

Same rules as LEDs: `state -> sound, volume`. Sounds by number from the
device's sound set (`src/sound/`).

### 5. Display (hub, `FEATURE_DISPLAY`)

Text and number fields the console fills: round title, counter, list of
numbers. No game logic on the hub screen.

### 6. Radio

- Additive message type `QuizRules` (new id) with the input rules, display
  and sound rules of a round, sent with `QuizState` and repeated like it.
  Devices without it run the v0 presets from `roundType`.
- The v0 frames stay unchanged (D7). `roundType` stays as preset choice.

## Proposal: `QuizRules` frame and link field `input`

Open for review (owner and Variour) before any code.

**Link:** `game round` takes an optional object `input`. Without it the
device runs the preset of `type`.

```json
{"cmd": "game round", "round": 7, "type": "choice", "phase": "open",
 "keys": {"up": "white", "down": "white"},
 "input": {"mode": "pick", "values": {"up": 1, "down": 2}, "firstOnly": true}}
```

| Field | Modes | Meaning |
| --- | --- | --- |
| `mode` | all | `once`, `pick`, `counter`, `log` |
| `values` | once, pick, log | value per key, 1-254; the keys must be in `keys` |
| `up`, `down`, `confirm` | counter | key names; `confirm` optional |
| `min`, `max`, `firstUp`, `firstDown` | counter | 0-254, `min <= firstDown <= firstUp <= max` |
| `repeat` | counter | holding up / down steps on |
| `sendDelayMs` | all | 0-5000 |

The rule takes `firstOnly` from the round. `log` goes with type `move` and
only with it. With `input`, `keys` needs no type-specific key set.

**Radio:** message type 47 `QuizRules`, bridge to devices, sent right after
each `QuizState` of a round that has `input`.

```
QuizHeader h            12
uint16 session           2
uint16 roundId           2
uint8  mode              1
uint8  flags             1   bit 0 firstOnly, bit 1 repeat
uint8  keyValue[10]     10   per Key slot, 0 = no value
uint8  upKey, downKey, confirmKey   3   Key, 0xFF = none
uint8  min, max, firstUp, firstDown 4
uint16 sendDelayMs       2
                        37 bytes
```

A device applies the rule for `roundId` once it has the round's `QuizState`;
until then, and on devices without the frame, the preset of `roundType`
applies. Display and sound rules follow as their own frame once the shared
answer layout is settled.

## Mapping of the v0 round types

| v0 | Input | Display rules (short) |
| --- | --- | --- |
| race | `once` Buzz | free: ring white; chosen: ring white; confirmed: ring blue; turn: timer; winner: run, then green |
| choice | `pick` Up/Right/Down/Left/Enter = 1-5 | free: keys white; chosen: key pulse; confirmed: ring in key colour; right: run; wrong: column orange red |
| scale | `counter` Up/Down, optional Enter | free: Up/Down white; value: bar in zone colour |
| scale rainbow | `counter` 1-24 | value: bar in rainbow colour |
| move | `log` directions | per press: key flash |

## Order of the pull requests

1. This document.
2. Board zones: LED indices of the Buzzerboard into the board profile, the
   game module asks for zones. No behaviour change.
3. Input rules: `InputRule` in `src/game/`, `Participant` runs the v0 round
   types through it. No protocol change.
4. LED effects and display rules: `GameRender` draws from rules, v0 types as
   presets. No protocol change.
5. `QuizRules` frame and link command `game rules`: the console sends rules.
6. Sound rules, display fields on the hub.

Each step keeps the v0 behaviour on the devices and the console.
