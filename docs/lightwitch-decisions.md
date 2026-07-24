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

### D2 · Fate of the automation engine (#447/#439) ✅

The freshly built `AutomationBinding` table (trigger → rules → actions) is
functionally a degenerate graph stage. Evolving both systems in parallel
creates two sources of truth for "event X → effect Y".

**Recommendation:** freeze automations now (no new trigger types, no new
features); graphs subsume them; migrating existing bindings to graphs is a
later step. `ActionExecutor` stays — not as a user-facing concept but as an
internal sink of the engine (it cleanly encapsulates config mutation plus
propagation over mesh/MQTT).

### D3 · Hardware-free engine core + native test environment ✅

Concept §1.8 requires an engine with no hardware dependency, testable on the
desktop. The repo has **no** `[env:native]` and zero C++ unit tests; nearly
every header includes `Arduino.h`, and base types (`Color`, `LightConfig`) live
in `Config.h`.

**Recommendation:** build the engine as a standalone module with **its own
minimal types** (no includes from `src/config`); adapters translate at the
boundary. Add `[env:native]` plus unit tests with the first engine commit.
This is less an option than a discipline commitment — without it, §3 (queue,
scheduler, topological sort, joints) is not reliably developable. The one
thing to decide: does the team accept native tests as a PR gate in CI?

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

### D5 · Canonical graph schema: language & v1 scope ☑️

**Decided: English canonical keys** (`nodes`, `edges`, `requires`), matching
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

## What the rework plan needs as input

**D1–D4** must be decided — they determine the module boundaries and the file
format. **D5–D6 are already decided** (English keys; no master role, WiFi
untouched), and **D7** is additive-only and needs no debate beyond confirming
the optional sequence numbers. For **N1–N12** it is enough to adopt the
recommendation as an assumption or override individual items; none of them
changes the shape of the first milestones (§10, stages 1–4).
