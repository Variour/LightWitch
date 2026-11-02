---
name: buzzer-rounds
description: Turns a presenter's plain-language description of buzzer rounds into round data for the LightWitch quiz buzzers (buzzer-round/0, buzzer-session/0) and hands it to the game console on the laptop. Use whenever someone describes a round for the buzzers ("Runde 4: ...", Quizfrage, Abstimmung, Skala, Umfrage, wer zuerst drückt, Pause), wants a talk or workshop prepared with buzzer rounds from an outline, wants to change a round, or asks what a console rejection means - even without saying JSON, skill or console.
---

# Buzzer rounds

A presenter describes in plain German what the audience should do with their buzzers. This skill turns that into round data in the round format v0 and delivers it to the game console. The console runs the session, talks to the buzzers, shows the screen and checks every file: it rejects anything it does not understand. So the skill's job is to translate faithfully, fit the texts to the screen and report back in words the presenter understands.

The skill holds no live state. The presenter announces, opens, closes and reveals rounds with the clicker or the arrow keys on the console.

## Workflow

1. Work out what the presenter wants: one new round, a change to a round already sent, or a whole session from an outline.
2. Translate into JSON (Translating, Round format).
3. Fit the screen texts (Fitting the screen).
4. Deliver to the console (Delivery).
5. Read the console's answer and report back in German (Reporting back).

For a single round, deliver right away once nothing had to be guessed: during a talk, speed matters more than a confirmation step. Ask first when a guess would change what the audience sees or what counts as right - which answer is correct, which of five answers to drop, a scale whose good end is the low end.

For a session, show the overview table first and deliver after the presenter agrees. A session replaces the whole plan in the console, so the presenter should see it before it lands.

## Translating

Pick the type from what the audience does:

- Set a value with UP and DOWN - "Skala", "wie sehr", "wie viele Punkte", "Zustimmung von 0 bis 9": `scale`
- First to press the big buzzer wins - "wer zuerst drückt", "Schnellrunde", "Buzzer-Runde": `race`
- Pick one of 2-4 options - "Quizfrage", "A/B/C/D", "Umfrage", "Ja oder Nein": `choice`
- Nothing to press - "Pause", "Buzzer aus": `idle`

Some things presenters say need no field, because every round already does them: "erst einmal alle gesperrt" (each round starts announced and locked), "offen bis zur nächsten Frage" (the presenter closes it), "Hoch grün, Runter rot" (fixed for scale), "der Screen zeigt die Stände" (fixed per type).

Rules that keep the data faithful:

- **One round per question on the screen.** The screen shows one fixed question per round. When the presenter lists several questions for "Runde 5", make one round each, titled "Runde 5 (1/3)", "Runde 5 (2/3)" and so on, delivered in order.
- **Title and id.** The title is the presenter's name for the round ("Runde 4", "Quizfrage 3"); without one, use a short name for the kind of round ("Umfrage", "Schnellrunde"). The id is the title in lowercase with ä→ae, ö→oe, ü→ue, ß→ss and every other run of characters turned into one `-`, trimmed, at most 32 characters: "Runde 5 (1/3)" becomes `runde-5-1-3`.
- **Same id only for a change.** A known id replaces that round in the console, a new id queues a new round. Keep track of the ids sent in this conversation. When a new round would get an id already used, add `-2`, `-3`. When the presenter changes a round ("ändere Runde 4", "bei der Quizfrage doch C"), send the full definition again under the same id.
- **Question text from the presenter's words.** Use the exact question when the presenter gives one. When they only describe it ("ich stelle eine Frage, wie viel Zustimmung es gibt"), write a short neutral question from their words, e.g. "Wie viel Zustimmung?", and say in the report that you phrased it. Never invent content: quiz answers, the right answer, or which options exist come from the presenter.
- **Scale range.** "10-stufig" or "zehn Stufen" is 0-9. "1 bis 10" is 1-10. Colours the presenter names go into `redMax` and `greenMin`; without named colours take the zone table below. When a change moves the range, recompute the zones the presenter did not name.
- **Choice.** Keep the presenter's order: it decides the key (1 triangle, 2 circle, 3 cross, 4 square). "Richtig ist B" is `"correct": 2`. A poll without a right answer is `"correct": null`.
- **Nothing beyond the format.** The console rejects unknown fields, and a silently dropped wish surprises the presenter in front of an audience. When they ask for something v0 cannot do, deliver what it can do and say in one line what is missing (table "Wishes the format cannot fulfil").

## Round format v0

The contract with the console. One JSON object per file, UTF-8, integers only, no other fields than these. Keys are English, screen texts German. Write every field of the type, also when it holds the default: the presenter then sees the whole round in the file.

Session:

| Field | Required | Rule |
|---|---|---|
| `format` | yes | exactly `"buzzer-session/0"` |
| `title` | no | 0-60 characters, on the start and login screen |
| `rounds` | no | 0-100 rounds, order = running order, ids unique |

Round, all types:

| Field | Required | Rule |
|---|---|---|
| `format` | yes | exactly `"buzzer-round/0"` |
| `id` | yes | 1-32 characters from `a-z`, `0-9`, `-` |
| `title` | no | 0-40 characters, small line above the question |
| `question` | no | 0-200 characters, screen only, never sent to the buzzers |
| `type` | yes | `"scale"`, `"race"`, `"choice"` or `"idle"` |

`scale`: value set with UP (+1, green) and DOWN (-1, red). Each buzzer starts empty, the first tap sets the middle.

| Field | Default | Rule |
|---|---|---|
| `min` | 0 | 0-98 |
| `max` | 9 | greater than `min`, at most 99, at most 24 steps (`max - min + 1`) |
| `redMax` | 3 | values up to here red; `min <= redMax < greenMin` |
| `greenMin` | 7 | values from here green; `greenMin <= max`; yellow in between |

Zones when the presenter gives a range but no colours (about 40 % red at the bottom, 30 % green at the top):

| Range | `redMax` | `greenMin` |
|---|---|---|
| 0-9 | 3 | 7 |
| 1-10 | 4 | 8 |
| 0-10 | 3 | 8 |
| 1-7 | 3 | 6 |
| 1-6 | 2 | 5 |
| 1-5 | 2 | 4 |
| 1-4 | 2 | 4 |
| 1-3 | 1 | 3 |
| 0-1 | 0 | 1 |

Other ranges: `steps = max - min + 1`, `redMax = min + round(0.4 × steps) - 1`, `greenMin = max - round(0.3 × steps) + 1`.

`race`: who presses the buzzer first. No further fields. The answer is spoken; the round only finds who was first.

`choice`: pick one of 2-4 answers. People may change their pick until the round closes; the screen shows the distribution only after closing.

| Field | Default | Rule |
|---|---|---|
| `answers` | required | 2-4 texts, 1-80 characters each; position 1-4 = triangle yellow, circle blue, cross magenta, square cyan |
| `correct` | `null` | number of the right answer, or `null` for a poll |

`idle`: nothing to press. No further fields, no `question`. The screen shows the title or "Pause".

What the console does with a delivery:

- A session replaces the title and the whole plan. A round that is running keeps running.
- A round with a new id is queued as the next round. Several new rounds in a row keep their order.
- A round with a known id replaces that definition. If it is running, it keeps running; the change applies the next time it is announced.

## Fitting the screen

The question and the answers are read from the back of the room. Limits measured on the console screen at 1920×1080 with German text:

| Text | Aim for | Upper limit | Above the limit |
|---|---|---|---|
| `question` | 80 characters (two large lines) | 110 (three lines) | smaller font, from about 170 four lines |
| each answer | 35 characters (one line) | 60 (two lines) | cut off when there are four answers |
| round `title` | a few words | 40 | rejected |
| session `title` | 40 | 60 | rejected |

Count characters exactly with code when code execution is available (`[...text].length` in JavaScript, `len(text)` in Python); estimates by eye are often off by 10-20 %.

When a text is over the upper limit, shorten it yourself: drop filler ("Was glaubt ihr eigentlich, ..."), keep the key terms, numbers and the question mark, keep the presenter's wording. The presenter can say the long version out loud; the screen carries the short one. Never shorten answers in a way that changes which one is right or makes two look alike. Report every shortened text as "lang → kurz" so the presenter can veto it.

## Delivery

Write the JSON with two-space indentation.

**Inbox folder (default).** The console watches a folder given with `--inbox` (its own default: `buzzer-inbox` in the home folder). Use the folder the presenter or the project notes name. Otherwise look for a folder called `inbox` or `buzzer-inbox` that contains `done` and `failed` (the console creates both at start); ask when there is none.

- File name: the round id plus the current time, e.g. `runde-4-143205.json`; for a session `session-143205.json`. A fresh name per delivery makes the result unambiguous.
- Write the file in one go, or write `name.json.tmp` and rename it to `name.json`.
- Then look for the result. The console takes a file within 1-2 seconds, so when it has not moved yet, look again a moment later:
  - `done/name.json`: accepted.
  - `failed/name.error.json`: rejected, its `error` field holds the German reason.
  - Still in the inbox after about 5 seconds: the console is not running or watches another folder. Say so. The file stays and is taken as soon as the console starts with this folder.

**CLI.** When your shell runs on the laptop that runs the console (Claude Code), deliver with `npx --no-install game-console send file.json` from the folder `tools/game-console` of the LightWitch repo. `--no-install` keeps npx from fetching a package of that name from the internet. Exit code 0 is accepted (the console's message on stdout, e.g. the queue position), 1 rejected (German reason on stderr), 2 the console is not running.

## Reporting back

Answer in German, short enough to read during a talk:

```
Runde 4 ist in der Konsole.
- Skala 0-9, rot bis 3, grün ab 7
- Screen-Text "Wie viel Zustimmung?" habe ich formuliert
```

Add a line for each shortened text, each guess and each wish the format could not fulfil. Show the JSON when the presenter asks for it or when preparing a session.

**On rejection** put the console's reason into the presenter's words:

| In the message | Say |
|---|---|
| `title` | Rundentitel |
| `question` | Screen-Text |
| `answers`, `answers[1]` | Antworten, Antwort 2 |
| `correct` | richtige Antwort |
| `min`, `max` | Skala von, Skala bis |
| `redMax`, `greenMin` | rot bis, grün ab |
| `id` | interne Kennung |
| `rounds[2]` | Runde 3 im Ablauf (counting starts at 0) |
| `Unbekanntes Feld` | etwas, das die Runde nicht kennt |

When the fix is clear (a colour zone outside the range, a text one character too long), fix it, deliver again and say what changed. When the fix changes the content (five answers, the right answer missing), ask first. When the console is not reachable, say so plainly and keep the file ready.

## Session from a talk outline

1. Find the places in the outline that ask for audience interaction ("Abstimmung", "Quiz", "Frage ans Publikum", "Buzzer", "Pause") and make one round per place, in outline order. Joining the buzzers is the login at session start and needs no round. When the outline marks no places, propose some and ask before building.
2. Session title: the talk title, at most 60 characters.
3. Show this overview in German before delivering, with shortened texts and guesses marked:

   | Nr | Titel | Art | Screen-Text | Antworten oder Skala |
   |---|---|---|---|---|

4. After the presenter agrees, deliver the whole session as one `buzzer-session/0` file. Rounds sent one by one earlier in the conversation are replaced by it; mention that.

## Wishes the format cannot fulfil

| Wish | What to do |
|---|---|
| Time limit ("30 Sekunden Zeit") | No timer in v0. Deliver without; the presenter closes with the clicker. |
| More than 4 answers | Ask which to drop, or split into two rounds. |
| Free text, numbers, several answers at once | Not possible. Offer choice or scale. |
| Points over several rounds, teams, overall winner | Not possible, each round has its own result. |
| More than 24 scale steps, "0 bis 100 %" | At most 24 steps. Offer 0-10 with "in 10-%-Schritten" in the question. |
| Low end is good (school grades 1-6) | Colours are always red low, green high. Ask whether to turn the scale around ("1 = schlecht, 6 = sehr gut"). |
| Other key colours, LED effects, sounds | Fixed per type. |
| Answer distribution live during the round | Hidden until the round closes, so nobody follows the majority. |
| Pictures on the screen | Not possible, the screen shows text. |

## Examples

### The reference round

> Runde 4: Alle Buzzer sind erst einmal deaktiviert. Ich stelle eine Frage, dann wird bei allen Buzzern die Hoch- und Runter-Taste freigeschaltet und grün/rot markiert. Man kann hoch und runter tippen und damit Werte auf einer 10-stufigen Skala einstellen, die durch die LEDs im Gerät visualisiert werden. Das Voting (wie viele Punkte Zustimmung gibt es?) ist offen, bis ich die nächste Frage stelle. Der Screen zeigt dauerhaft die Stände der Geräte, die bereits irgendeinen Wert angenommen haben. Wert 0-3 rot, ab 7 grün, dazwischen gelb.

```json
{
  "format": "buzzer-round/0",
  "id": "runde-4",
  "title": "Runde 4",
  "question": "Wie viel Zustimmung?",
  "type": "scale",
  "min": 0,
  "max": 9,
  "redMax": 3,
  "greenMin": 7
}
```

"10-stufig" gives 0-9, the colours give 3 and 7, everything else is how every scale round behaves. The question is only spoken, so the screen text comes from "(wie viele Punkte Zustimmung gibt es?)" and the report says so.

### Quiz, poll, race, scale range, pause

> Quizfrage 3: Wie funken die Buzzer untereinander? A ESP-NOW, B Bluetooth, C WLAN mit Router, D Zigbee. Richtig ist A.

```json
{"format": "buzzer-round/0", "id": "quizfrage-3", "title": "Quizfrage 3", "question": "Wie funken die Buzzer untereinander?", "type": "choice", "answers": ["ESP-NOW", "Bluetooth", "WLAN mit Router", "Zigbee"], "correct": 1}
```

> Kurze Umfrage: Kaffee oder Tee?

```json
{"format": "buzzer-round/0", "id": "umfrage", "title": "Umfrage", "question": "Kaffee oder Tee?", "type": "choice", "answers": ["Kaffee", "Tee"], "correct": null}
```

> Schnellrunde: Wer zuerst drückt, darf antworten. Wie heißt die Hauptstadt von Australien?

```json
{"format": "buzzer-round/0", "id": "schnellrunde", "title": "Schnellrunde", "question": "Wie heißt die Hauptstadt von Australien?", "type": "race"}
```

> Wie sicher fühlt ihr euch mit dem Thema? Von 1 bis 5.

```json
{"format": "buzzer-round/0", "id": "selbsteinschaetzung", "title": "Selbsteinschätzung", "question": "Wie sicher fühlt ihr euch mit dem Thema?", "type": "scale", "min": 1, "max": 5, "redMax": 2, "greenMin": 4}
```

No colours named, so the zones come from the table; the report says "1-2 rot, 3 gelb, 4-5 grün".

> Jetzt Pause, die Buzzer sollen nichts tun.

```json
{"format": "buzzer-round/0", "id": "pause", "title": "Pause", "type": "idle"}
```

### A change

> Ändere Runde 4: nur von 1 bis 5.

Same id, full definition, question kept, zones recomputed because the presenter named none:

```json
{"format": "buzzer-round/0", "id": "runde-4", "title": "Runde 4", "question": "Wie viel Zustimmung?", "type": "scale", "min": 1, "max": 5, "redMax": 2, "greenMin": 4}
```

### Several questions in one round

> Runde 5 sind drei Ja/Nein-Fragen: Habt ihr schon mal gelötet? Habt ihr einen 3D-Drucker? Habt ihr schon mal einen ESP32 programmiert?

Three files, delivered in this order:

```json
{"format": "buzzer-round/0", "id": "runde-5-1-3", "title": "Runde 5 (1/3)", "question": "Habt ihr schon mal gelötet?", "type": "choice", "answers": ["Ja", "Nein"], "correct": null}
```

```json
{"format": "buzzer-round/0", "id": "runde-5-2-3", "title": "Runde 5 (2/3)", "question": "Habt ihr einen 3D-Drucker?", "type": "choice", "answers": ["Ja", "Nein"], "correct": null}
```

```json
{"format": "buzzer-round/0", "id": "runde-5-3-3", "title": "Runde 5 (3/3)", "question": "Habt ihr schon mal einen ESP32 programmiert?", "type": "choice", "answers": ["Ja", "Nein"], "correct": null}
```

### Shortening

> Quizfrage 4: Was glaubt ihr eigentlich, wie viele Buzzer ein einzelnes Gateway in der ersten Version gleichzeitig im Raum verwalten können soll? 10, 50, 500 oder unbegrenzt? Richtig sind 50.

The question has 131 characters, over the limit of 110. Shortened to 50:

```json
{"format": "buzzer-round/0", "id": "quizfrage-4", "title": "Quizfrage 4", "question": "Wie viele Buzzer schafft ein Gateway in Version 1?", "type": "choice", "answers": ["10", "50", "500", "Unbegrenzt"], "correct": 2}
```

Report line: `Gekürzt: "Was glaubt ihr eigentlich, wie viele Buzzer ..." → "Wie viele Buzzer schafft ein Gateway in Version 1?"`

### A wish the format cannot fulfil

> Quizfrage 5 mit 20 Sekunden Zeit: Welches Protokoll nutzen die Buzzer? ESP-NOW, BLE, Zigbee, Thread, LoRa. Richtig ESP-NOW.

Ask before delivering: "Fünf Antworten gehen nicht, höchstens vier. Welche soll raus? Ein Zeitlimit gibt es noch nicht, du schließt die Runde per Klick."

### Session from an outline

> Bereite mir den Ablauf für meinen Vortrag vor:
> Funk ohne Router - Workshop
> 1. Begrüßung, Buzzer verbinden
> 2. Einstieg: Abfrage, wer schon mal etwas mit ESP32 gebaut hat (ja/nein)
> 3. Grundlagen ESP-NOW
> 4. Quiz: Auf welchem Frequenzband funken die Buzzer? 2,4 GHz, 5 GHz, 868 MHz. Richtig 2,4 GHz.
> 5. Mesh und Reichweite
> 6. Schnellrunde: Wie viele Kanäle hat das 2,4-GHz-Band in Europa? (mündlich)
> 7. Pause
> 8. Abstimmung: Wie verständlich war der Teil bis hier, 0 bis 9
> 9. Fragen und Abschluss

Overview shown before delivering:

| Nr | Titel | Art | Screen-Text | Antworten oder Skala |
|---|---|---|---|---|
| 1 | Einstieg | Auswahl | Habt ihr schon mal etwas mit einem ESP32 gebaut? (formuliert) | Ja, Nein (Umfrage) |
| 2 | Quiz | Auswahl | Auf welchem Frequenzband funken die Buzzer? | 2,4 GHz (richtig), 5 GHz, 868 MHz |
| 3 | Schnellrunde | Rennen | Wie viele Kanäle hat das 2,4-GHz-Band in Europa? | - |
| 4 | Pause | Ruhe | - | - |
| 5 | Abstimmung | Skala | Wie verständlich war es bis hier? (formuliert) | 0-9, rot bis 3, grün ab 7 |

Outline points 1, 3, 5 and 9 need no round. After the presenter agrees:

```json
{
  "format": "buzzer-session/0",
  "title": "Funk ohne Router - Workshop",
  "rounds": [
    {"format": "buzzer-round/0", "id": "einstieg", "title": "Einstieg", "question": "Habt ihr schon mal etwas mit einem ESP32 gebaut?", "type": "choice", "answers": ["Ja", "Nein"], "correct": null},
    {"format": "buzzer-round/0", "id": "quiz", "title": "Quiz", "question": "Auf welchem Frequenzband funken die Buzzer?", "type": "choice", "answers": ["2,4 GHz", "5 GHz", "868 MHz"], "correct": 1},
    {"format": "buzzer-round/0", "id": "schnellrunde", "title": "Schnellrunde", "question": "Wie viele Kanäle hat das 2,4-GHz-Band in Europa?", "type": "race"},
    {"format": "buzzer-round/0", "id": "pause", "title": "Pause", "type": "idle"},
    {"format": "buzzer-round/0", "id": "abstimmung", "title": "Abstimmung", "question": "Wie verständlich war es bis hier?", "type": "scale", "min": 0, "max": 9, "redMax": 3, "greenMin": 7}
  ]
}
```
