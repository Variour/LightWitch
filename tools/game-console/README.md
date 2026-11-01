# Game console

The laptop side of the LightWitch quiz buzzers. It talks to the gateway buzzer over USB, keeps the session, the participants and every value, serves a live screen for the projector, and takes rounds from the Claude skill. With `--simulate 50` it runs the whole system against 50 simulated buzzers, so sessions can be rehearsed and tested without hardware.

Contract: gateway protocol v0 (`gateway-protokoll-v0.md`) and round format v0 (`runden-format-v0.md`). Where the firmware on the `claude/game-session` branch (`src/game/Link.h`, `Bridge.h`) differs from the docs, the console follows the firmware.

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
| `A` | `admit all` | Admit every waiting buzzer (manual admission). |
| `S` | | Show or hide the presenter strip. |
| `T` | | Light or dark theme (also `?theme=light` in the URL). |
| | `session-start` | Start a session (login open). |
| | `announce [id]` | Announce the next planned round, or the round with this id. |
| | `open`, `close`, `reveal` | Phase of the running round; `reveal` shows the right answer of a choice round. |
| | `winner <n>` | Set the race winner by hand (0 clears it). |
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

## Input for the Claude skill

The skill delivers round and session definitions as JSON in the round format v0. Two ways reach the running console; both apply the same rules and answer with the same result.

### Format

One JSON object per file or request, UTF-8:

- `{"format": "buzzer-round/0", ...}`: one round. Fields `id`, `title`, `question`, `type` and per type `min`, `max`, `redMax`, `greenMin` (scale) or `answers`, `correct` (choice), with the ranges and defaults of the round format.
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
| `POST /api/action` | `{"action": "next"}`, with arguments as in the CLI: `{"action": "announce", "id": "quiz-3"}`, `{"action": "winner", "n": 17}`, `{"action": "admit", "mac": "all"}`, `{"action": "renumber", "n": 3, "to": 30}`, `{"action": "release", "n": 4}`, `{"action": "admit-mode", "mode": "manual"}`, `{"action": "usb-number", "n": 17}` | `200` with `{ok: true, message}`, `409` with `{ok: false, error}` |
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

The presenter strip at the bottom shows the USB link, online buzzers, the gateway's `dropped` and `rxOverflow` counters, channel and login, the next planned round and the latest warning. `S` hides it; a red dot in the corner then marks a lost link.

## Serial link

- The console opens the port with the serialport defaults and never sets DTR or RTS afterwards. POSIX asserts both lines together on open, Windows opens with DTR on and RTS off; an ESP32 behind USB-Serial-JTAG resets only while RTS is on and DTR is off. Check on the first board that opening the port leaves it running.
- It says `link hello` on every open, repeats it every 2 s until the gateway answers, pings after 2 s without other lines and says hello again after 6 s without any line from the gateway. When the port disappears it reopens it as soon as it is back.
- After every hello the gateway replays its state. The console compares it with its own state and sends what differs: session and round again after a gateway restart, the running round when the gateway shows another one. A console started during a running session takes the session, its participants and the running round's values over from the replay.
- Value lines are not filtered by `vseq`; a buzzer restarts its `vseq` after a rejoin and the gateway already passes only newer values.
- A `device` line with a MAC and number counts as admission, in case the `admitted` line was dropped from the gateway's event queue.

## Simulator

`--simulate N` replaces the serial port with an in-process gateway that speaks the same NDJSON and follows the firmware's bridge and participant logic: channel hopping invites, admit and release repeats, echo, resend with back-off, status every 15 s, rejoin after a restart. Its people join a few seconds after the invite, tap towards a value on scale rounds, press after a human reaction time in races (with a few near ties), pick answers with a popular favourite in choice rounds, and sometimes change their mind. Most buzzers carry the stored numbers 1 to N, a few none, and one shares its number with another.

## Tests

```sh
npm test                                  # all tests, including the screen check
SCREENSHOT_DIR=/some/dir npm test         # also save a 1920x1080 screenshot per view
```

The screen check runs Chromium through Playwright and skips when no browser is installed (`npx playwright install chromium`).

## Für Vortragende (Deutsch)

1. Gateway-Buzzer per USB-C an den Laptop, dann im Ordner `tools/game-console` einmal `npm install` und `npx game-console` starten. Zum Üben ohne Hardware: `npx game-console --simulate 50`.
2. Im Browser <http://127.0.0.1:4711/> öffnen, auf den Beamer ziehen, F11 für Vollbild.
3. Runden kommen vom Skill: als Datei in den Ordner `buzzer-inbox` in deinem Benutzerordner oder per `npx game-console send runde.json`. Ist etwas falsch, steht der Grund in der Leiste unten und in `buzzer-inbox/failed/<name>.error.json`.
4. Mit Pfeil rechts oder dem Presenter weiterschalten: Sitzung starten, Anmeldung (Buzzer drücken), dann je Runde ankündigen, öffnen, schließen, bei Quizfragen auflösen.
5. `L` öffnet oder schließt die Anmeldung, `S` blendet die Leiste aus, `T` wechselt hell und dunkel.
6. Zum Schluss `npx game-console end`: alle Buzzer werden freigegeben und starten neu. Den Gateway-Buzzer danach einmal aus- und einschalten, dann ist er wieder ein normales LightWitch-Gerät.
