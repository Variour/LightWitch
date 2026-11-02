# Game console

The laptop side of the LightWitch quiz buzzers. It talks to the gateway buzzer over USB, keeps the session, the participants and every value, serves a live screen for the projector, and takes rounds from the Claude skill. With `--simulate 50` it runs the whole system against 50 simulated buzzers, so sessions can be rehearsed and tested without hardware.

Contract: gateway protocol v0 (`gateway-protokoll-v0.md`), round format v0 (`runden-format-v0.md`) and, for the charging overview and the fleet commands, the fleet protocol v0 draft (`protokoll-fleet-v0-entwurf.md`). Where the firmware on the `claude/game-session` branch (`src/game/Link.h`, `Bridge.h`) differs from the docs, the console follows the firmware.

## Run it

Node 20 or newer, macOS, Windows or Linux. No build step.

```sh
cd tools/game-console
npm install
npx game-console --simulate 50        # rehearse with 50 simulated buzzers
npx game-console                      # real gateway on USB, found automatically
npx game-console --port COM5          # or name the port
```

Then open the screen at <http://127.0.0.1:4711/> on the projector (F11 for full screen).

| Option | Meaning |
|---|---|
| `--port <path>` | Serial port of the gateway (`COM5`, `/dev/cu.usbmodem1101`, `/dev/ttyACM0`). Without it, the console takes the only port with the Espressif USB vendor id `303a` and waits until there is exactly one. |
| `--simulate <N>` | No USB: built-in gateway with N simulated buzzers. |
| `--sim-loss <percent>` | Simulator: drop each radio frame with this probability. |
| `--sim-latency <ms>` | Simulator: delay each radio frame by up to this many ms. |
| `--sim-reboots` | Simulator: every 30-60 s one buzzer restarts and rejoins. |
| `--channel <1-13>` | Radio channel of the session, default 6. |
| `--fleet <0-65535>` | Fleet of the invite, default 0 (all buzzers). |
| `--manual-admit` | Admit joining buzzers only on the presenter's command. |
| `--http-port <n>` | Port of the screen and the API, default 4711. Also used by the client commands. |
| `--inbox <dir>` | Inbox folder for the skill, default `~/buzzer-inbox`. |
| `--trace` | Print every protocol line (`>` to the gateway, `<` from it). |

`npx game-console ports` lists the serial ports and marks the Espressif ones.

## Presenter controls

Every control is an action of the running console. The same actions work from the screen's keyboard, the CLI and the HTTP API.

| Screen key | CLI | Action |
|---|---|---|
| `→`, `PageDown`, `Space`, `Enter` | `next` | The next step (see below). Presenter clickers send `PageDown`. |
| `L` | `login-open`, `login-close` | Open or close login. |
| `F` | `fleet-view [on\|off]` | Show or hide the charging overview (see below). |
| `A` | `admit all` | Admit every waiting buzzer (manual admission). |
| `S` | | Show or hide the presenter strip. |
| `T` | | Light or dark theme (also `?theme=light` in the URL). |
| | `session-start` | Start a session (login open). |
| | `announce [id]` | Announce the next planned round, or the round with this id. |
| | `open`, `close`, `reveal` | Phase of the running round; `reveal` shows the right answer of a choice round. |
| | `winner <n>` | Set the race winner by hand (0 clears it). |
| | `group <k>` | Move round with `"play": "gruppen"`: group k moves now. |
| | `idle` | Start an idle round. |
| | `admit <mac> [n]` | Admit one waiting buzzer, optionally with a number. |
| | `renumber <n> <new>` | Give a participant another number; the buzzer stores it. |
| | `release <n>` | Release one participant. |
| | `admit-mode auto\|manual` | Switch automatic admission. |
| | `usb-number [n\|none]` | Read, set or clear the stored number of the buzzer on the USB cable (firmware command `game number`). |
| | `end` | End the session; the gateway releases all buzzers for 5 s. |
| | `status` | Short state in the terminal. |

`next` steps through a session:

1. No session: start one, login open.
2. Login open: announce the next planned round (this closes login). With no round planned, close login.
3. Round announced: open it. A race opens with a 1 s countdown.
4. Round open: close it.
5. Choice round with a right answer: reveal it.
6. Otherwise: announce the next planned round, at least 1 s after the last close. With none left, switch to idle.

The race winner is set 1 s after the first press arrives and corrected if an earlier press arrives later. `winner <n>` overrides it.

Admission: while login is open, every joining buzzer is admitted with the number it has stored, or the smallest free number if it has none or the number is taken. A buzzer that rejoins after a restart is admitted again without a press, also with login closed. With `--manual-admit`, joining buzzers wait on the login screen until `admit all` or `admit <mac>`.

## Orientation and `hold`

A buzzer can be held upright (`hochkant`), across with the buzzer on the right (`quer`) or on the left (`quer-links`), or lie flat on the table (`liegend`). Provisional until the firmware reads its orientation sensor: the buzzer takes a new orientation after holding still for 2 s, remaps its buttons and reports logical inputs (`up`, `right`, `down`, `left`, `triggerLeft`, `triggerRight`, `buzzer`, `enter`) together with its orientation `lage`. Upside down or face down is no orientation; the last one stays.

| `lage` | up | right | down | left | triggerLeft / triggerRight |
|---|---|---|---|---|---|
| `hochkant` | Up | Right | Down | Left | - |
| `quer` | Left | Up | Right | Down | L2 / L1 |
| `quer-links` | Right | Down | Left | Up | R1 / R2 |
| `liegend` | Up | Right | Down | Left | - |

Every round with inputs (scale, race, choice, move) takes an optional `hold`:

- `frei` (default): the buzzer follows its orientation.
- `hochkant`, `quer`, `liegend`: the round requires this orientation; `quer-links` counts as `quer`. A buzzer in another one is locked. The screen shows "x von y bereit" (`liegend`: "x von y liegen bereit") while the round is announced and, in a race, until the release.
- `fest`: the orientation at the open stays for the whole round.

The console:

- Keeps each participant's `lage` from its `join` and `device` lines (null: unknown, as with today's firmware).
- Counts `results.ready`: the admitted participants whose `lage` meets the required one. Unknown counts as not ready. null for `frei` and `fest`.
- Ignores inputs (values, race presses, move keys) made in another orientation than the round requires. The input's own `lage` decides, else the participant's. An unknown orientation passes, so today's firmware keeps working.
- With `fest`, notes each participant's `lage` at the open and uses that one for the round.

## Move rounds

A `move` round is a small board game: PowerPoint draws the board, the console keeps the game state, and each buzzer moves its figure with its four direction keys.

```json
{"format": "buzzer-round/0", "id": "lauf-ins-ziel", "title": "Spiel", "question": "Wer ist zuerst im Ziel?", "type": "move", "hold": "quer", "goal": "right"}
```

- `hold` as above, default `frei`. `goal`: `right`, the only goal for now. `cols` 2-40 (default 22) and `rows` 1-20 (default 10): the PowerPoint bridge measures the field and sends them when loading; authors leave them out.
- Cells are 0-based (col, row), col 0 left, row 0 top. Number n starts at col = floor((n-1)/rows), row = (n-1) mod rows: with 10 rows the numbers 1-10 start in col 0, 11-20 in col 1.
- Opening puts every participant on its start cell and clears the order. A buzzer admitted later gets its start cell once that cell is free. A start cell in the goal column or beyond the board gives no figure (`col` and `row` null).
- While the round is open, each press moves the figure one cell. A key line with a `lage` carries the logical direction. A key line without one carries the physical button: with `hold` `quer` the console turns it (left is up, up is right, right is down, down is left), otherwise it takes it as printed. A press that would leave the board or land on an occupied cell leaves the figure where it is.
- A figure that reaches the last column is in the goal and gets the next place; its further presses do nothing. The first in the goal wins. Figures in the goal keep their cell, so at most `rows` buzzers get into the goal.
- Closing freezes the board. Announcing and opening the round again starts over.

`round` in `/api/state` carries `hold`, `goal`, `cols`, `rows` and `results`: `players` (every admitted participant by number: `n`, `col`, `row`, `place` from 1 or null), `order` (the numbers in the goal), `winner` (a number or null), `finished` (how many are in the goal), `participants`, `ready` and `play`.

### Play modes

PowerPoint redraws a figure in about 17 ms, so 50 moving figures are too slow. `play` picks how many move; the modes other than `alle` move about ten. A buzzer's group is its transport case: numbers 1-10 group 1, 11-20 group 2, and so on.

- `alle` (default): a figure per participant, as above.
- `team`: a figure per group with at least one admitted member. Team k starts at col 0, row min((k-1)*2, rows-1). Each press of a member moves the team by 1/members of a cell in that direction, members counted at that moment. Positions are fractions, kept within the board; teams do not block each other. A team at x = cols-1 is in the goal and stops. `results.teams` lists `team`, `members`, `x`, `y` (rounded to 3 decimals) and `place`, by team number. `order` and `winner` hold team numbers, `finished` counts teams. `players` have no cell.
- `erste`: after the open, the first `rows` participants to press get a figure, in that order, at col 0 and rows 0, 1, 2 and so on. That first press only takes the place. Everybody else watches for the rest of the round. Moving and the goal as in `alle`. `results.chosen` lists the numbers in order, `watching` counts the admitted participants without a place.
- `gruppen`: a figure per participant on its start cell, as in `alle`. Only the active group moves; the lowest group with members starts. `group <k>` (`{"action": "group", "k": 2}`) switches while the round is open; the figures keep their cells. `results.group` names the active group.

## Gateway lines for orientation and move rounds

Provisional until the firmware has them:

- `game round` carries `hold` (`hochkant`, `quer`, `liegend` or `fest`; absent means `frei`). It is fixed for a running round. The gateway passes it to the buzzers with the round state.
- `game join` and `game device` carry the buzzer's `lage`. A buzzer sends its status right when it takes a new orientation, so the gateway writes a `device` line at once, and again with every regular status.
- `game value` carries the `lage` the buzzer had when it made the value.
- `game key`, one line per press in a move round: `{"ns":"game","ev":"key","n":7,"round":23417,"key":"right","kseq":12,"lage":"quer"}`. With `lage` the key is logical, without it the physical button. `kseq` is the buzzer's press counter (8 bit, per buzzer and round, from 1). The console applies each press once: a `kseq` that is not newer than the last applied one of that number (mod 256) is a resend. A `join` of the buzzer starts its count again.

`participants[]` in `/api/state` carries `lage`; `round` carries `hold` and `results.ready` for every round with inputs.

## Charging overview and fleet

Every buzzer outside a game session reports its state about every 30 s; the gateway passes each report on as a `fleet status` line. The console keeps the latest report per MAC. Fleet commands go to the gateway, which broadcasts them to every buzzer outside a game session; buzzers in a running session ignore them.

| Screen key (overview) | CLI | Action |
|---|---|---|
| `F`, `→`, `Esc` | `fleet-view [on\|off]` | Back to the game screen. Without an argument the CLI toggles. |
| `U` twice within 4 s | `update-all [--pr <n>]` | Update every buzzer and the gateway itself: newest release, or the build of pull request n. |
| `E` | `charge event` | Charge target Event: 100 %. |
| `L` | `charge store` | Charge target Lagern (store): about 80 %. |
| | `off-all` | Buzzers on battery go to ship mode now and leave the overview. Buzzers on USB stay on. |
| | `ap <n\|mac>` | Open the maintenance WiFi of one buzzer for 10 minutes. A number must be unique in the overview. |
| | `fw-ref <hex\|gateway>` | Firmware the overview compares with. Default: the gateway's own build from `link hello`. |
| | `wifi-set` | The device on the USB cable: ask for SSID and password in the terminal, store them on the device. |
| | `config-get` | The device on the USB cable: firmware, number, number of WiFi networks, charge target. |

The overview shows per buzzer its number, state (aktiv, am Netz, Standby), battery, charge state (lädt, voll, Akku, Ladefehler), charge target (a mark on the battery bar), firmware with "aktuell" or "veraltet", update progress, the reason of a failed update, errors, and "kein WLAN" for a buzzer that cannot take `update-all`. Colours as on the buzzer's charging display: yellow charging, green full, blue update, violet outdated, red error. At the top: how many charge, are full, run outdated firmware, update, and have an error. A buzzer silent for 90 s is dimmed and leaves the counts.

`update-all` updates the gateway too. On a new build it restarts: the overview shows "Gateway getrennt" until the link is back, then "Gateway wieder verbunden, neue Firmware ..." for a minute. The reference firmware follows the new hello.

- `wifi-set` reads two lines (SSID, password) from stdin when stdin is not a terminal. The password goes to the device only: the console does not store, show or log it, and `--trace` prints `***`. Password: 8 to 63 printable ASCII characters, or empty for an open network.
- The console sends fleet commands only when the gateway names `fleet` in the `spaces` of its hello, and config commands only with `config` there.
- `/api/state` carries `fleet`: `ref` (`fw`, `source`: `gateway` or `console`), `counts` and `devices[]` with `mac`, `n`, `state`, `bat`, `vbat`, `chg`, `target`, `usb`, `wifi`, `fw`, `fwStatus`, `upd`, `pct`, `updErr`, `updErrText`, `err`, `errors` (German names of the error bits), `rssi`, `lastSeen` (ms since 1970, when the gateway heard the buzzer), `ageS` and `stale`. `link.fwChange` (`from`, `to`, `at`) names a new gateway build for a minute after the reconnect.
- Factory test: the test script talks to the device under test on its own serial port and reads number, battery, firmware and RSSI of the buzzers from `/api/state` (`fleet.devices[]`: `n`, `bat`, `fw`, `rssi`). With two Espressif devices plugged in, start the console with `--port` for the gateway.

HTTP actions: `{"action": "update-all", "pr": 481}`, `{"action": "charge", "target": "event"}`, `{"action": "off-all"}`, `{"action": "ap", "to": 12}` (or a MAC), `{"action": "fw-ref", "fw": "1a2b3c4d"}`, `{"action": "fleet-view", "mode": "on"}`, `{"action": "config-get"}`, `{"action": "wifi-set", "ssid": "Halle 3", "pass": "..."}`.

## Input for the Claude skill

The skill delivers round and session definitions as JSON in the round format v0. Two ways reach the running console; both apply the same rules and answer with the same result.

### Format

One JSON object per file or request, UTF-8:

- `{"format": "buzzer-round/0", ...}`: one round. Fields `id`, `title`, `question`, `type`, `hold` (every type but idle, see above) and per type `min`, `max`, `redMax`, `greenMin` (scale), `answers`, `correct` (choice) or `goal`, `cols`, `rows` (move, see above), with the ranges and defaults of the round format.
- `{"format": "buzzer-session/0", "title": "...", "rounds": [...]}`: a whole plan, up to 100 rounds.

Unknown fields and fields of another round type are rejected and named. Only integers are numbers.

What applying does:

- A session replaces the title and the whole round plan. The next `next` announces its first round. A running round keeps running.
- A round with an id that is already planned replaces that definition in place. A running start of it keeps running unchanged.
- A round with a new id is queued as the next round. Several new rounds in a row keep their order.

Radio channel and fleet are console options, not part of the JSON.

### Result

Success:

```json
{"ok": true, "message": "Runde \"quiz-3\" als nächste eingereiht (Position 2 von 4)."}
```

Rejection, with German text ready for the presenter:

```json
{
  "ok": false,
  "error": "Abgelehnt: 1 Fehler.\n- Runde \"quiz-3\": \"answers\" braucht 2 bis 4 Antworten (sind 5).",
  "errors": [
    {"path": "answers", "message": "Runde \"quiz-3\": \"answers\" braucht 2 bis 4 Antworten (sind 5)."}
  ]
}
```

`error` is a summary line plus one line per problem. `errors[].path` names the field, inside a session as `rounds[2].max`.

### Inbox folder

For a skill that can only write files into a shared folder.

- Folder: `~/buzzer-inbox` (`C:\Users\<name>\buzzer-inbox` on Windows), or `--inbox <dir>`. The console creates it with `done/` and `failed/` inside and prints the path at start.
- Write each round or session as `<name>.json` into the folder. Write it in one go, or write `<name>.json.tmp` and rename it. The console takes a file once its size and time stayed the same for one poll (500 ms), oldest first. Hidden files and other extensions stay untouched.
- Applied: the file moves to `done/<name>.json`.
- Rejected: the file moves to `failed/<name>.json`, next to `failed/<name>.error.json` with the result above plus `file` and `at` (ISO time).
- A file name used again overwrites the old one in `done/` or `failed/`.
- Rejections also show in the presenter strip and the console's terminal.

### CLI

```sh
npx game-console send round.json
npx game-console round --file round.json
```

Prints the message on success (exit code 0), the German `error` text on stderr when rejected (exit code 1), and a note on stderr when no console is running (exit code 2). Pass `--http-port` when the console runs on another port.

### HTTP

The console listens on `127.0.0.1:4711` only. Requests need a `Host` of `127.0.0.1`, `localhost` or `[::1]` with the port; POST bodies need `Content-Type: application/json`.

| Request | Body | Answer |
|---|---|---|
| `POST /api/load` | a round or a session | `200` with `{ok: true, message}`, `422` rejected, `400` not JSON, `413` larger than 256 KB |
| `POST /api/action` | `{"action": "next"}`, with arguments as in the CLI (fleet actions above): `{"action": "announce", "id": "quiz-3"}`, `{"action": "winner", "n": 17}`, `{"action": "admit", "mac": "all"}`, `{"action": "renumber", "n": 3, "to": 30}`, `{"action": "release", "n": 4}`, `{"action": "admit-mode", "mode": "manual"}`, `{"action": "usb-number", "n": 17}` | `200` with `{ok: true, message}`, `409` with `{ok: false, error}` |
| `GET /api/state` | | the full state the screen gets |
| `GET /ws` | | WebSocket, the same state on every change and once per second |

```sh
curl -s -X POST http://127.0.0.1:4711/api/load -H 'Content-Type: application/json' -d @round.json
```

## The screen

One page with a view per state, German texts, sized for 1920x1080 and readable from the back of a room:

- Start: session title before the session runs and after it ended.
- Login: "Buzzer drücken zum Mitmachen", the count and the numbers of the admitted buzzers, the last pressed buzzer.
- Idle: title and all participants with online state and battery.
- Scale: per value a column of LED segments in its zone colour (red up to `redMax`, green from `greenMin`, yellow between), the count per value, the mean, how many voted, and a tile per buzzer with its value. Shown live while open and kept after closing.
- Race: countdown to the start, then the ranking by press moment with ties under 5 ms marked "Gleichstand", and the winner.
- Choice: answers with the symbol and colour of their key (triangle yellow, circle blue, cross magenta, square cyan). While open only "x von y haben geantwortet", after closing the count per answer, after the reveal the right answer and who had it.
- Move: title, question, "x von y im Ziel" and the number of the first in the goal. The board itself is in PowerPoint. `team`: "x von y Teams im Ziel" and "Team k". `erste`: "x von 10 Plätzen vergeben" until every place is taken. `gruppen`: "Gruppe k ist dran".
- A round that requires an orientation: "x von y bereit" under the question while announced and, in a race, until the release.

The presenter strip at the bottom shows the USB link, online buzzers, the gateway's `dropped` and `rxOverflow` counters, channel and login, the next planned round and the latest warning. `S` hides it; a red dot in the corner then marks a lost link.

## Serial link

- The console opens the port with the serialport defaults and never sets DTR or RTS afterwards. POSIX asserts both lines together on open, Windows opens with DTR on and RTS off; an ESP32 behind USB-Serial-JTAG resets only while RTS is on and DTR is off. Check on the first board that opening the port leaves it running.
- It says `link hello` on every open, repeats it every 2 s until the gateway answers, pings after 2 s without other lines and says hello again after 6 s without any line from the gateway. When the port disappears it reopens it as soon as it is back.
- After every hello the gateway replays its state. The console compares it with its own state and sends what differs: after a gateway restart the session and the running round in one write, so the gateway reads both in the same pass and goes from no session straight to the running round; the running round alone when the gateway shows another one. A console started during a running session takes the session, its participants and the running round's values over from the replay.
- Value lines are not filtered by `vseq`; a buzzer restarts its `vseq` after a rejoin and the gateway already passes only newer values.
- A `device` line with a MAC and number counts as admission, in case the `admitted` line was dropped from the gateway's event queue.

## Simulator

`--simulate N` replaces the serial port with an in-process gateway that speaks the same NDJSON and follows the firmware's bridge and participant logic: channel hopping invites, admit and release repeats, echo, resend with back-off, status every 15 s, rejoin after a restart. Its people join a few seconds after the invite, tap towards a value on scale rounds, press after a human reaction time in races (with a few near ties), pick answers with a popular favourite in choice rounds, and sometimes change their mind. Most buzzers carry the stored numbers 1 to N, a few none, and one shares its number with another.

The simulated buzzers already do what the firmware will do with orientation. Each starts in a random orientation, takes a new one after 2 s of holding still, and reports logical inputs with its `lage`; its person presses the physical button that points the right way as they hold it. In a round that requires an orientation, most people turn their buzzer into it after 1-6 s, about 6 % take 15-45 s and about 4 % never do, so "x von y bereit" stays below y for a while. Until then the buzzer is locked and its person waits: in a race with `liegend` they press only once their buzzer lies.

In move rounds the people see the board: each presses at a pace of their own (one press every 0.45-1 s on average), mostly towards the goal. In `erste` everybody tries to get a place right after the open, with a race's reaction time; those without one stop. In `gruppen` the people press only while their group is active. With 20 buzzers on 22 columns the first reach the goal after about 20 s, the tenth after 30-50 s. The buzzers number their presses and send the unconfirmed ones again until the gateway's echo shows them; the gateway passes each press on once as a `game key` line.

Keyboard: with `--simulate` in a terminal, the arrow keys move one simulated buzzer in the direction shown on the board, and Tab picks the next participant (in an open move round only one that can move now). The terminal names the buzzer ("Tastatur steuert Nr. 7"); its person pauses meanwhile. The buzzer sends the direction as a logical input, in the orientation the round requires (`quer` when it requires none), and is turned into that orientation at once. Ctrl+C quits.

The simulated buzzers start on USB power supplies: most charge, some are full at 80 %, one has a charger fault, one runs an old firmware, two are out on battery and one is in standby. They answer the fleet commands: an update takes 30-60 s with progress and ends with the release build (or a build per PR number), a new charge target makes full buzzers charge on, `off` switches the buzzers on battery off. The simulated gateway updates itself as well and restarts on a new build. When login opens, the people unplug every buzzer. The simulated gateway also answers `wifi-set` and `config-get`.

## Tests

```sh
npm test                                  # all tests, including the screen check
SCREENSHOT_DIR=/some/dir npm test         # also save a 1920x1080 screenshot per view, the charging overview as screen-ladeuebersicht.png
```

The screen check runs Chromium through Playwright and skips when no browser is installed (`npx playwright install chromium`).

## Für Vortragende (Deutsch)

1. Gateway-Buzzer per USB-C an den Laptop, dann im Ordner `tools/game-console` einmal `npm install` und `npx game-console` starten. Zum Üben ohne Hardware: `npx game-console --simulate 50`.
2. Im Browser <http://127.0.0.1:4711/> öffnen, auf den Beamer ziehen, F11 für Vollbild.
3. Runden kommen vom Skill: als Datei in den Ordner `buzzer-inbox` in deinem Benutzerordner oder per `npx game-console send runde.json`. Ist etwas falsch, steht der Grund in der Leiste unten und in `buzzer-inbox/failed/<name>.error.json`.
4. Mit Pfeil rechts oder dem Presenter weiterschalten: Sitzung starten, Anmeldung (Buzzer drücken), dann je Runde ankündigen, öffnen, schließen, bei Quizfragen auflösen.
5. `L` öffnet oder schließt die Anmeldung, `S` blendet die Leiste aus, `T` wechselt hell und dunkel.
6. Zum Schluss `npx game-console end`: alle Buzzer werden freigegeben und starten neu. Den Gateway-Buzzer danach einmal aus- und einschalten, dann ist er wieder ein normales LightWitch-Gerät.

## Für die Crew: Laden und Lagern (Deutsch)

1. Buzzer laden an jedem USB-Netzteil. Gateway-Buzzer per USB-C an den Laptop, Konsole starten, Bildschirm öffnen, `F` drücken: die Ladeübersicht. Oben steht, wie viele laden, voll sind, veraltete Firmware haben oder einen Fehler melden. Rot umrandet: Fehler, violett: Firmware veraltet, blau: Update läuft. "am Netz" heißt: der Buzzer hängt am Netzteil.
2. Vor dem Event `E`: alle Buzzer laden auf 100 %. Zum Lagern danach `L`: alle laden nur bis etwa 80 %.
3. Firmware veraltet: zweimal `U` (oder `npx game-console update-all`). Die Buzzer holen das Update übers WLAN, das Gateway auch. Solange die Säule am Buzzer blau läuft, nicht abziehen. Das Gateway startet danach neu, die Übersicht zeigt das und verbindet sich von selbst wieder.
4. WLAN für Updates: den Buzzer per USB-C an den Laptop und `npx game-console wifi-set` eingeben. Name und Passwort werden im Terminal abgefragt, das Passwort bleibt unsichtbar. Einen Buzzer, der nicht das Gateway ist, danach einmal aus- und einschalten. "kein WLAN" auf einer Kachel heißt: dieser Buzzer braucht das noch.
5. Zum Einpacken `npx game-console off-all`: alle Buzzer im Akkubetrieb gehen aus. Buzzer am Netzteil bleiben an und laden weiter.
6. Ein einzelner Buzzer braucht Wartung: `npx game-console ap 12` öffnet sein Wartungs-WLAN für 10 Minuten.
7. `F` oder Pfeil rechts führt zurück zum Spielbildschirm.
