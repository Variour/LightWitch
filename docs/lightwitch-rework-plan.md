# LightWitch rework plan

Derived from the LightWitch system concept v1.0 and the resolved decisions in
[lightwitch-decisions.md](lightwitch-decisions.md) (D1–D7). Terminology follows
the repo: **node** (not "Stein"), **graph**, **stage node**, **role**,
**group**. All identifiers, schema keys, docs, and UI strings are English;
German appears only as optional editor labels later.

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

This generalizes the existing brightness-override pattern; the old
brightness override is absorbed into it (a brightness-only direct command)
rather than maintained in parallel.

### What it takes

- `LightOverrideMsg` (new MsgType): `targetMac`, `lightIndex`, `LightConfig` —
  addressing mirrors `SetGroupMsg`. Additive, no existing message changes.
- Override slot + arbitration in the light state path
  (`applyAndPropagateLightConfig` / `PatternRunner` wiring in `main.cpp`).
- `ActionExecutor`: new action target kind "light on device" (by device name +
  light index) so a button on ESP 1 can command a light on ESP 5. Targeting
  "the farthest device" needs proximity data and arrives with the proximity
  milestone (M7) as a graph capability, not a button feature.
- Web UI: per-light indicator "following group / overridden (by X)" plus a
  clear-override control.

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
  "edges": [[1, "pressed", 2, "start"]],
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

Per-light override + arbitration + `LightOverrideMsg` + "light on device"
button action + UI indicator, as specified above. Absorbs the brightness
override. **Demo:** press a button on device A, one light on device B leaves
its group scene and turns green; changing the group scene takes it back.

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
editor yet (that is M8) — but the GUI must make migrated automations
recognizable and fixable (D2 requirement). Automations UI gets a "frozen —
superseded by graphs" notice.

*Touches:* `data/index.html`, `server/` mock + Playwright flows.

### M4 · Value layer + stage node

Value-layer nodes (`lfo` via LUT, `math`, `hold`, `hsv-mix`, joints/type
adapters on edges per concept §5.3) and the **stage node**: binds to a light
role, claims the light through the M1 arbitration, drives its `PatternRunner`
with a channel-driven pattern (intensity, stimulus, movement, limit, color —
scenes read the channels they know). Existing patterns stay available as
procedural scenes. **Demo:** breathing lamp whose speed follows an LFO;
group/user commands still take the light back at any time.

*Touches:* `graph`, `patterns` (one new channel-driven pattern), `config`.

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

The concept's dock UI (columns, docking, joints, chain view) on top of the
by-then-proven schema. Explicitly last: the JSON editor (M3) carries all
functionality until here, and dock details (elbow tolerance, zoom) are decided
after first editor tests.

*Touches:* `data/index.html` (likely split into a second page/bundle —
decide when sizing it).

---

## Explicitly deferred (unchanged from decision sheet)

Native test env & measurement suite (D3) · LED direction map + painted sphere
scenes & paint editor · sound beyond existing playback (tone generator comes
with a later stage-audio milestone) · microphone/IMU hardware · ms-level time
sync · sequence numbers · web write protection (N7 — must land before M6
ships to real fleets; small, scheduled alongside M6) · OTA over mesh
(dropped).
