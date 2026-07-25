# LightWitch rework plan

Derived from the LightWitch system concept v1.0 and the resolved decisions in
[lightwitch-decisions.md](lightwitch-decisions.md) (D1–D7).

## Vocabulary

One word per thing, used identically in schema keys, code identifiers, docs,
and UI strings — there is no separate user-facing vocabulary and no
translation layer, so a term that reads well in the editor must also be the
term in the code.

| Term | Meaning |
|---|---|
| `graph` | One named, activatable program: nodes plus their links. Stored as one JSON file. |
| `node` | One building block of logic — a typed unit with ports. Never "Stein"/"stone"/"block". |
| `port` | A connection point on a node. Input ports sit on the node's left edge, output ports on its right; flow runs left to right. Say "input port"/"output port" when the direction matters. Never "pin" — that means a GPIO pin everywhere else in this project. |
| `link` | Two ports joined. Arises from adjacency (neighboring columns, row distance ≤ 1), not from a drawn line — so never "wire", "edge", or "connection" (the latter means a network connection everywhere else). |
| `adapter` | A link between two different signal types, carrying a conversion rule. Which conversions exist is the adapter matrix. |
| `signal type` | What flows through a port: event, switch, value, color. Shape is the primary code, not colour. |
| `chip` | The editable default value shown on a free input port. |
| `arc` | The small curve in the gutter drawn for a link whose two ports sit one row apart — the only line segment in the system. |
| `rail` | A named portal that links distant nodes without adjacency. |
| `dock` (verb) | To join two ports by placing their nodes next to each other. |
| `stage node`, `role`, `group` | As used elsewhere in this repo. |

Scope guardrails (from D3/D7): no native/desktop tooling, no measurement
suite, no mesh protocol rework. Every milestone ships a usable feature on the
existing ESP + browser ecosystem and leaves the fleet operational.

---

## Core design 1 · Command arbitration (last command wins) — from D1

### Today

State lives per **group** (`GroupConfig.light`, a `LightConfig` with a `seq`
counter), synced over mesh; every light renders its group's state. The only
per-light deviation is the local-only brightness override. There is **no way
to command a single light across devices** — this is new work.

### Target model

Three command sources can claim a light, arbitrated by recency:

1. **Group commands** (existing path, unchanged): web UI, MQTT, buttons,
   mesh sync → `GroupConfig.light`, `seq` bumped.
2. **Direct light commands** (new): a full `LightConfig` aimed at one light on
   one device, from any origin (another device's button, web UI, graph node,
   MQTT later).
3. **Graph stage claim** (later milestone): a stage node driving a light's
   renderer continuously.

Arbitration rule: **the newest command owns the light.**

- A direct command creates a per-light override: the light stores the override
  `LightConfig` plus the group's `seq` at capture time (`baseSeq`).
- While overridden, group re-broadcasts with `seq == baseSeq` (periodic
  self-heal advertisements) do **not** displace the override.
- Any group update with `seq > baseSeq` — i.e. someone actually changed the
  group — drops the override; the light rejoins the group. Classic behavior
  preserved: set a group scene and everyone follows.
- A newer direct command simply replaces the previous override.
- A stage claim participates identically (claiming = a command; a newer group
  or direct command takes the light back and deactivates the claim).

A direct command always carries a **full `LightConfig` snapshot** — the
sender builds it (target group state is mesh-synced, so any sender can copy
it and change fields) and the receiver replaces, never merges. Relative
effects ("20 % brighter") are explicitly *not* overrides — they are overlays
(below), because only the target device knows its own effective value.

### Brightness layering

Brightness passes through fixed layers, applied in order:

1. **Group state** — what plays (scene/pattern incl. brightness).
2. **Standing adjustment** — the existing `brightnessOverrideEnabled`/
   `brightnessOverride` on `LightHardwareConfig`: applies while the light
   follows its group, and applies again whenever it returns to the group.
   Untouched by this rework.
3. **Override / stage claim** — absolute: while active it replaces layers
   1–2 entirely.
4. **Overlay** — relative modulation on whatever layers 1–3 produce.
5. **Hardware clamp** (new in M1): `brightnessLimit` (hard cap, peaks are
   clipped) and `brightnessScale` (proportional damping) on
   `LightHardwareConfig`, applied at the LED driver boundary to *everything*
   — overrides and overlays included. This is where "this LED must never
   exceed X" and "this LED generally runs dimmer" live; they are hardware
   properties, not commands.

### Temporary commands

A direct command may carry an optional `durationMs`. When it expires, the
override is dropped and the light reverts to whatever it would otherwise show
(normally its group state) — no restore command needed, and a crash/reboot on
the sender can't leave a light stuck. Example: "blink for 2 s, then back to
the group scene." Expiry runs through the same arbitration as everything else;
a newer command arriving mid-duration replaces the temporary one immediately.

### Overlay layer (planned; implemented with M4)

Overrides *replace* a light's state. Some effects instead need to sit **on
top of** whatever is currently rendering: dim one light in a group, tint it,
or flicker it while its scene keeps running underneath — e.g. a "broken bulb"
magic effect: the light keeps its group scene but flickers.

Design (documented now, built in M4 when the render path is touched anyway):

- Per light, one **overlay slot** applied as a post-processing step after the
  pattern renders and before pixels reach the `LedDriver`. The base state —
  group scene, override, or stage claim — keeps running untouched underneath.
- Overlay types v1: `gain` (brightness factor — < 1 dims, > 1 brightens,
  so "+20 % blink" is gain 1.2 + flicker; result clamped by the hardware
  layer), `tint` (color blend with strength), `flicker` (procedural
  modulation with intensity/rate).
- Overlays carry the same optional `durationMs` and follow the same rules as
  overrides: newest overlay wins the slot, an explicit clear removes it, and
  it is independent of the base-state arbitration — changing the group scene
  does *not* clear an overlay, since the two answer different questions
  ("what plays" vs. "what disturbs it").
- Transport: `LightOverlayMsg` (additive MsgType, same `targetMac` +
  `lightIndex` addressing as `LightOverrideMsg`); later also drivable from a
  graph node for scripted magic effects.

### What it takes

- `LightOverrideMsg` (new MsgType): `targetMac`, `lightIndex`, full
  `LightConfig` snapshot, optional `durationMs`, `clear` flag — addressing
  mirrors `SetGroupMsg`. Additive, no existing message changes. Expiry and
  clear are handled receiver-side; the override is volatile across reboot;
  with multiple senders, receive order decides (last wins).
- Override slot + arbitration in the light state path
  (`applyAndPropagateLightConfig` / `PatternRunner` wiring in `main.cpp`).
- Hardware clamp: `brightnessLimit` + `brightnessScale` on
  `LightHardwareConfig`, applied at the LED driver boundary (see brightness
  layering above), editable under Settings → Lights.
- `ActionExecutor`: new **set-light action** targeting `targetMac` +
  `lightIndex` (the config UI shows device names from the peer registry and
  stores the MAC, so renames never break bindings). Deliberately minimal
  parameters: color, brightness, pattern (static or strobe for blinking),
  optional `durationMs` — the sender fills the rest of the snapshot from the
  target's synced group state. Full-snapshot construction stays a capability
  of the message (and of graph nodes from M2 on), not of the button UI.
  Targeting "the farthest device" needs proximity data and arrives with the
  proximity milestone (M7) as a graph capability, not a button feature.
- Web UI: per-light indicator "following group / overridden" plus a
  clear-override control on the owning device's dashboard. MQTT exposure of
  per-light overrides is out of scope for M1.

## Core design 2 · Graph engine placement — from D2/D3

New module `src/graph/`, self-contained, callback-wired from `main.cpp` like
every other module. Efficiency rules from the concept are kept (int32 values,
fixed-size event queue, payloads of fixed size, graph compiled to flat arrays
on load, no heap allocation in the hot path, ~30 Hz value tick). The
measurement suite (concept §12) is **not** built; a debug REST endpoint
(`GET /api/graphs/<name>/state`: node outputs, queue depth, last events) plus
the existing web log sink cover development needs on real hardware.

Automations (`AutomationManager`, `AutomationBinding`, its UI section) are
frozen as of now: bug fixes only, no new trigger types, no new features.

## Core design 3 · Roles & participation policy — from D4

- Each hardware entry (`LightHardwareConfig`, `ButtonHardwareConfig`,
  `SoundHardwareConfig`) gets an optional `role` string (e.g. `stage:ring`,
  `button:main`) and an `offered` flag. A graph declares `requires: [roles]`;
  it can only activate on a device where every required role exists **and** is
  offered. Withholding a role is the per-interaction opt-out (candle never
  joins a buzzer game but keeps repeating mesh traffic — mesh participation is
  a layer below and untouched).
- Device-level graph acceptance mode in `DeviceConfig`:
  - `auto` (default): matching graphs from mesh/group are adopted and
    activated — easiest for testing.
  - `ask`: graphs are received and stored but stay inactive until the user
    enables them in the web UI.
  - `off`: graphs from mesh/group are ignored entirely (locally created
    graphs still work).
- Every graph additionally has its own per-device enable switch (concept:
  `active` flag).

## Graph schema v1 (summary)

```json
{
  "v": 1,
  "name": "buzzergame",
  "active": true,
  "requires": ["button:main", "stage:main"],
  "nodes": [
    {"id": 1, "type": "button", "role": "button:main", "col": 1, "row": 1, "cfg": {}}
  ],
  "links": [[1, "pressed", 2, "start"]],
  "notes": [{"col": 1, "row": 5, "text": "registration"}]
}
```

English keys throughout (D5). `col`/`row` are editor-only metadata. Files live
in `/graphs/*.json` on LittleFS, managed by a `GraphManager` following the
`SceneManager` pattern. Versioning/migration follows the repo's existing
schema-migration convention.

---

## Milestones

Each milestone ends demonstrable and leaves `main` shippable.

### M1 · Last command wins (no graph involvement yet)

Per-light override + arbitration + `LightOverrideMsg` (incl. `durationMs`
expiry and `clear`) + hardware brightness clamp + set-light button action +
UI indicator, as specified above. The existing standing brightness
adjustment stays untouched; the overlay layer is designed here but not built
yet. **Demo:** press a button on device A, one light on device B leaves its
group scene and turns green — or blinks for 2 s and returns on its own;
changing the group scene takes it back; the hardware clamp caps everything.

*Touches:* `config`, `actions`, `mesh` (additive msg), `web`, `main.cpp`.

### M2 · Graph engine core + automation parity

`src/graph/`: schema v1 load/validate/compile, event queue + timer scheduler +
value tick, node registry with the parity node set — `button`, `timer`,
`mesh-receive` (GenericEvent by type, payload out), `compare`/`range`,
`constant`, `gate`, `wait`, `action` (sink into `ActionExecutor`),
`mesh-send` (GenericEvent out, built-in throttle: on-change + min interval),
`log`. REST: `GET/PUT/DELETE /api/graphs/<name>`, activate toggle, state
debug endpoint. **Parity gate:** every possible `AutomationBinding` is
expressible; a converter endpoint generates the equivalent graph from an
existing binding so migration is verifiable rule by rule.
**Demo:** an existing automation re-created as a graph behaves identically.

*Touches:* new `src/graph/`, `web` (API only), `main.cpp`.

### M3 · Graph GUI v1

Graphs card in the web UI: list with per-graph active toggle, acceptance-mode
setting, JSON text editor with validation feedback, "convert binding to
graph" button using the M2 converter, live log filter per graph. No visual
editor yet (that is M8; a mock-only shell prototype is pulled forward — see
there) — but the GUI must make migrated automations recognizable and fixable
(D2 requirement). Automations UI gets a "frozen — superseded by graphs"
notice.

*Touches:* `data/index.html`, `server/` mock + Playwright flows.

### M4 · Value layer + stage node

Value-layer nodes (`lfo` via LUT, `math`, `hold`, `hsv-mix`, adapters/type
adapters on links per concept §5.3) and the **stage node**: binds to a light
role, claims the light through the M1 arbitration, drives its `PatternRunner`
with a channel-driven pattern (intensity, stimulus, movement, limit, color —
scenes read the channels they know). Existing patterns stay available as
procedural scenes. This milestone also builds the **overlay layer** from core
design 1 (post-render `gain`/`tint`/`flicker` slot with `durationMs`,
`LightOverlayMsg`, overlay graph node), since it touches the same render
path. **Demo:** breathing lamp whose speed follows an LFO; group/user
commands still take the light back at any time — and the "broken bulb":
one light in a group flickers while its scene keeps running underneath.

*Touches:* `graph`, `patterns` (one new channel-driven pattern + overlay
post-processing in `PatternRunner`), `mesh` (additive msg), `config`.

### M5 · Roles & participation policy

Role + `offered` fields on hardware entries, `requires` matching, acceptance
modes `auto`/`ask`/`off`, per-graph enable, web UI for all of it (device
settings + graphs card). **Demo:** the same graph file activates on a matching
device and is correctly refused/parked on a non-matching or opted-out one.

*Touches:* `config`, `graph`, `web`.

### M6 · Graph distribution over mesh

Masterless sync copied from the scene-sync trio (manifest/request/chunk, new
additive MsgTypes), gated by the M5 policy on the receiving side. **Demo:**
save a graph on one device; every consenting, matching device adopts it.

*Touches:* `mesh` (additive), new `GraphSyncManager` (mirrors
`SceneSyncManager`), `web`.

### M7 · Proximity & event semantics

RSSI from `PeerRegistry` into the engine: `proximity` source node and proximity
attached to received events (stimulus = sent strength × proximity). Enables
"torch lights candle" and proximity-based targeting ("farthest device") for
M1-style commands. **Demo:** swinging a sender device toward a receiver
ignites its fire scene.

*Touches:* `mesh` (callback plumbing), `graph`.

### M8 · Dock editor (visual)

The concept's dock UI (columns, docking, adapters, chain view) on top of the
by-then-proven schema. Explicitly last: the JSON editor (M3) carries all
functionality until here, and dock details (elbow tolerance, zoom) are decided
after first editor tests.

**Pulled forward — editor shell, mock-only (#464):** the shell — canvas with
the column/dock layout, placing/moving/connecting building blocks, save/load
of schema-v1 documents (`col`/`row` as editor metadata, `notes`) — is built
early on its own page/bundle against the mock server's in-memory
`/api/graphs`, with a fixed minimal palette from the M2 parity node set and
deliberately thin node-config panels. Purpose: play through flows in the
browser and derive follow-up issues before the engine exists — the "first
editor tests" this milestone's open dock details wait on. Engine wiring,
authoritative validation, the full palette, and the remaining dock feature
set stay here in M8; nothing runs a saved graph until M2.

Two decisions settled while building it:

- **Fan-out runs through rows.** An output port may feed several neighbors,
  but only as many as it has instance rows — the same `＋` row mechanism that
  lets an input collect several sources. The number of receivers is therefore
  visible in the node's height, which keeps "position is the program" intact.
  An unbounded output with no rows was the alternative and was rejected for
  hiding that count.
- **The adapter matrix is provisional.** The four conversions currently
  offered (value→switch, switch→value, value→color, event→switch) are a
  working set for the prototype, not the authoritative table — that one lives
  in system concept §5.3, which is not in this repo yet and replaces these
  when it lands.

*Touches:* `data/index.html` (likely split into a second page/bundle —
decide when sizing it).

---

## Process compliance

Every milestone is executed with the repo's own workflow and tooling
([CONTRIBUTING.md](../CONTRIBUTING.md), `.claude/skills/`):

- **Work packages are GitHub issues.** Each milestone is split into
  reviewable issues (e.g. M1 → override arbitration, `LightOverrideMsg`,
  button action target, UI indicator). Flow per issue: `/issue <id>` →
  implement → `/validate-ui` (when `data/` or `server/` changed) →
  `/feature-checklist` → `/open-pr` (exactly one label; milestone features
  are `enhancement`, docs are `skip-changelog`). Scoped commit messages,
  English everywhere.
- **Mock-server parity is part of every feature.** Per `/feature-checklist`:
  new/changed endpoints in `src/web/WebServer.h` get matching routes in
  `server/index.js`; new config/group/peer fields go into `MOCK_CONFIG` /
  `MOCK_SELF` / `MOCK_PEERS`; new WebSocket event types get a mock emitter;
  mock data must exercise the feature. This is how the "develop in the final
  ecosystem" resolution (D3) works in practice — the graph API and GUI are
  fully exercisable against the mock server in a browser, no hardware needed.
- **Config changes migrate.** Any new persisted field bumps
  `CONFIG_SCHEMA_VERSION` and adds a sequential step to `migrateDoc()`
  (`src/config/Config.cpp`) — never the reset-to-defaults fallback. Applies
  to M1 (button action target), M5 (roles, `offered`, acceptance mode).
- **New mesh messages update the inventory.** Each additive MsgType
  (`LightOverride`, `LightOverlay`, graph sync trio) is added to the message
  table in [mesh-compatibility.md](mesh-compatibility.md) with its framing
  rule, following that doc's future rule: new MsgType, never a mutated
  existing layout. The plan is additive-only, so it stays inside the
  same-firmware policy.
- **Formatting/linting:** `clang-format` on changed `src/` files only (CI
  checks changed files); `npm run lint` / `npm test` for `server/` and the
  inline UI script.
- **Hardware testing via PR builds.** CI publishes every PR's `esp32dev`/
  `esp32s3` firmware as a `pr-<N>` prerelease installable OTA from the device
  web UI — milestone demos (M1's cross-device override, M7's proximity) are
  verified on real devices from the PR before merge. `esp32c3` is excluded
  from PR builds; C3-specific behavior is verified locally via
  `pio run -e esp32c3` targets when relevant.
- **UI review via PR previews.** Every PR gets an Azure-hosted mock-UI
  preview environment (URL posted on the PR) — GUI milestones (M3, M5, M8)
  are clickable for reviewers without any local setup.
- **`/grill-me`** is used to stress-test each milestone's design before
  implementation starts, beginning with M1.

## Explicitly deferred (unchanged from decision sheet)

Native test env & measurement suite (D3) · LED direction map + painted sphere
scenes & paint editor · sound beyond existing playback (tone generator comes
with a later stage-audio milestone) · microphone/IMU hardware · ms-level time
sync · sequence numbers · web write protection (N7 — must land before M6
ships to real fleets; small, scheduled alongside M6) · OTA over mesh
(dropped).
