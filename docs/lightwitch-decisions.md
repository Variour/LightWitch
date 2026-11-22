# LightWitch × batteryLight — decision sheet

Based on the LightWitch system concept v1.0 (2026-07-24) and the current state
of `main`. Part 1 lists the structural decisions: they shape the rework plan
(module boundaries, file format, mesh integration) and should be settled by the
team before planning starts. Part 2 collects the remaining open questions
(concept §11 and related) with a recommendation as a working assumption — the
plan can proceed on these assumptions, and they can be revisited later.

Recommendation legend: ✅ clear preference · ⚖️ genuine trade-off, team call
needed · ☑️ already decided.

---

## Part 1 · Structural decisions

### D1 · Who owns the LEDs — graph world vs. group world ⚖️

**Today:** a group holds `LightConfig` (pattern/scene/color), synced across the
mesh; `PatternRunner` renders it. **Concept:** graphs run locally, the stage
node is the device-wide instance of a light, and channels (intensity, stimulus,
limit, …) drive the scene.

| Option | Trade-off |
|---|---|
| **(a) Graph as a new mode** — `GroupMode::Graph`; while a light is in this mode, the stage node renders instead of the existing pattern path. Existing patterns remain callable as procedural scenes. | Incremental; the fleet stays operational throughout; group/mesh sync untouched. Cost: two rendering paths to maintain in parallel, plus transition rules on mode change. |
| (b) Big bang — graphs fully replace groups/patterns | Conceptually clean ("firmware renders only pixels"), but no working fleet for months and loss of proven features (MQTT, sync, overrides) until parity is rebuilt. |
| (c) Graph as action source only — graphs write configs via `ActionExecutor` and nothing else | Cheapest entry, but the channel-based stage (§6.3) is unreachable; the concept degenerates into a nicer automation table. |

**Recommendation: (a)**, with a declared long-term direction toward (b): new
capabilities are built in the graph world only, and the legacy path is frozen.

**Resolution (2026-07-24):** groups stay permanently — they are a smart-home
feature, not a legacy path. Devices and groups must be addressable
independently, and the arbitration rule is **last command wins**: a command
targeted at a single light overrides that light's group state; a newer group
command takes the light back. See the rework plan, "Command arbitration".

### D2 · Fate of the automation engine (#447/#439) ✅

The freshly built `AutomationBinding` table (trigger → rules → actions) is
functionally a degenerate graph stage. Evolving both systems in parallel
creates two sources of truth for "event X → effect Y".

**Recommendation:** freeze automations now (no new trigger types, no new
features); graphs subsume them; migrating existing bindings to graphs is a
later step. `ActionExecutor` stays — not as a user-facing concept but as an
internal sink of the engine (it cleanly encapsulates config mutation plus
propagation over mesh/MQTT).

**Resolution (2026-07-24):** confirmed. Automations are frozen now; the graph
engine takes over. Requirements on the takeover: it must be gapless (every
existing binding expressible as a graph before migration), and the web GUI must
represent graphs well enough that a migrated binding is directly recognizable
and fixable.

### D3 · Hardware-free engine core + native test environment ✅

Concept §1.8 requires an engine with no hardware dependency, testable on the
desktop. The repo has **no** `[env:native]` and zero C++ unit tests; nearly
every header includes `Arduino.h`, and base types (`Color`, `LightConfig`) live
in `Config.h`.

**Recommendation:** build the engine as a standalone module with **its own
minimal types** (no includes from `src/config`); adapters translate at the
boundary. Add `[env:native]` plus unit tests with the first engine commit.
This is less an option than a discipline commitment — without it, §3 (queue,
scheduler, topological sort, adapters) is not reliably developable. The one
thing to decide: does the team accept native tests as a PR gate in CI?

**Resolution (2026-07-24):** deprioritized. No native environment and no
desktop tooling for now — development happens in the final ecosystem (ESP +
browser). What is kept from the concept: the engine's efficiency rules (int32,
fixed-size queue, no heap in the hot path) and a clean module boundary so a
native test env can be added later without rework. What is cut for now: the
probe/measurement suite (concept §12), unit-test infrastructure, and anything
that bloats the firmware without shipping a feature. Development visibility
comes from the existing log sink plus a small graph-state debug endpoint.

### D4 · Roles/device profile vs. existing hardware config ✅

Concept §4.2 calls for `/profil.json` (role → pin/segment plus calibration).
The device already has a hardware config (`LightHardwareConfig`,
`ButtonHardwareConfig`, `SoundHardwareConfig` inside `DeviceConfig`,
NVS/LittleFS-persisted, web-UI-managed, mesh config push).

**Recommendation:** **no second source of truth.** Roles become names on the
existing hardware entries (e.g. `role: "stage:ring"` on a light slot), and
calibration becomes an extra block on those entries. The "profile" is then a
view over `DeviceConfig`, not a separate file. Role matching ("a graph runs on
every device whose profile fits") works against these entries.

**Resolution (2026-07-24):** confirmed, and extended into a participation
policy. Per device it must be possible to opt out of kinds of interaction
(e.g. a candle stays a candle — and a mesh repeater — but never joins a buzzer
game). Mechanism: (1) roles on hardware entries can be individually offered or
withheld, so a graph requiring a withheld role simply doesn't match; (2) a
device-level graph acceptance mode — `auto` (default: adopt every matching
graph, easiest for testing), `ask` (new graphs arrive but stay inactive until
the user enables them), `off` (ignore graphs from mesh/group entirely).

### D5 · Canonical graph schema: language & v1 scope ☑️

**Decided: English canonical keys** (`nodes`, `links`, `requires`), matching
the rest of the repo; German terms appear only as editor labels. This mirrors
the concept's own §5.5 split between display language and stored form. The v1
scope additionally includes: the edge list as sketched, the `v` field, the
role declaration, and `col`/`row` as editor-only metadata.

### D6 · Master role — resolved: no change to WiFi or election ☑️

Constraint set by the project owner: no structural changes to WiFi handling or
the existing setup unless functionally unavoidable. Reviewing the concept's
master role (§2: UI access, distribution, log collection) against the repo
shows nothing forces one in v1:

- Every device already serves the full web UI — any device is an equal UI
  entry point; no dedicated master needed for access.
- Distribution can mirror the existing **masterless** scene-sync pattern
  (manifest/request/chunk between peers) for graphs — no coordinator needed.
- A central live log is the only master-shaped feature left; it is deferred
  (each device's own log view exists today).

**Resolution:** v1 has no master role at all; concept §11.2 is closed without
touching `WifiElection` or anything in the WiFi stack.

### D7 · Mesh interface — additive only, existing behavior untouched ✅

No existing message type or behavior changes. What the graph engine's
mesh-send/mesh-receive nodes need on top is purely additive:

- **Proximity in the receive path:** RSSI per peer is already tracked in
  `PeerRegistry`; it only needs to be handed into the receive callback so
  "stimulus = sent strength × proximity" works. Data exists, one plumbing step.
- **Rate limiting / send-on-change for graph sends:** today's `GenericEvent`
  senders are one-shot (button, automation). A graph can emit a value at the
  30 Hz tick — an unthrottled node would flood the mesh, so the *new* send
  path needs a throttle. Existing senders are unaffected.
- **New message types for graph distribution**, copied from the scene-sync
  trio (manifest/request/chunk) — the same additive pattern every feature so
  far has used.
- A thin facade wraps `MeshManager` for the engine so the engine core stays
  hardware-free (ties into D3). The facade is a wrapper, not a rework.

Optional and droppable: sequence numbers on the `GenericEvent` class, only
useful for the loss-rate measurement in concept §12.4.

**Resolution (2026-07-24):** minimally invasive, confirmed. Stay on the
current mesh layer as long as possible. Only guaranteed addition: the graph
mesh-send node ships with a built-in throttle (send-on-change + minimum
interval) so a graph cannot flood the mesh. Sequence numbers are dropped.
Proximity plumbing and graph distribution arrive only with their milestones.
Should genuinely high-rate ("flooding") graph designs ever be wanted, the
question becomes where such a graph should run at all — revisit then, not now.

---

## Part 2 · Non-blocking — recommendation as working assumption

| # | Question (concept ref) | Recommendation | Rationale / note |
|---|---|---|---|
| N1 | Time sync for buzzer fairness (§11.3) | v1: "first message wins" | Existing `TimeSync` is second-granularity; ms-level mesh time is new work → v2, and only after measuring real press spacing (§12.5). |
| N2 | Runtime state across reboot (§11.4) | volatile; optional "retain" flag later | Matches the flash-wear argument (§12.4); the codebase already writes config only on explicit changes. |
| N3 | Color internals / RGBW (§11.5) | HSV inside engine + stage, convert to RGB at the `LedDriver` boundary; defer RGBW | Codebase is RGB throughout; converting at a single boundary keeps the change small. No supported RGBW hardware exists today. |
| N4 | LED map authoring (§11.6) | presets per form factor (strip/ring/matrix → generated direction vectors); camera wizard later | `MatrixLayout` already provides matrix geometry; the map **format** still belongs in schema v1 to avoid a migration. |
| N5 | Sphere paint editor (§11.7) | after milestone 5, as per concept | The existing scene editor (flat w×h frames) remains the painting tool until then. |
| N6 | Sound pipeline (§11.8) | v1 = tones + short samples on top of what exists | WAV-from-SD, playlists, and synchronized start exist (`PlayAudioMsg`); only a small tone generator is new. Defer mixing/formats. |
| N7 | Web write protection (§11.9) | PIN for writing endpoints, at latest before graph-push ships | The device API is open today; graph distribution raises the stakes considerably (arbitrary behavior becomes pushable). |
| N8 | Graph migration (§11.10) | follow the repo's existing schema-migration convention | Already documented (commit d05e7ca); don't invent a new mechanism. |
| N9 | Dock editor details: elbow tolerance, zoom (§11.11) | after first editor tests | Editor v1 is REST + a JSON text field anyway (§5.6). |
| N10 | OTA over mesh (§11.12) | **drop** | GitHub OTA + mesh nudges (`CheckUpdate`/`TriggerUpdate`) exist and work; mesh chunk transfer stays reserved for graphs/scenes. |
| N11 | Acceptance target values (§11.13) | measure first, then fix — as per concept | Build the probe infrastructure (§12.1) early; set targets after milestone 2. |
| N12 | Microphone / IMU | pick hardware by milestone 6 | No driver foundation in the repo; the only existing source is `BatteryMonitor`. Affects milestone 7, not plan start. |

---

## Status

All structural decisions **D1–D7 are resolved** (see the resolution notes
above). The rework plan is derived from them: see
[lightwitch-rework-plan.md](lightwitch-rework-plan.md). For **N1–N12** the
recommendations stand as working assumptions; N11 (measurement suite) is
additionally deferred by the D3 resolution.
