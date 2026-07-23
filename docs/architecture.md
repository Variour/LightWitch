# Architecture

An overview of how `src/` is organized, for anyone approaching the firmware
codebase for the first time. For build/flash/update instructions see
[development.md](development.md); for the mesh protocol see
[mesh-compatibility.md](mesh-compatibility.md).

## Module overview

| Module | Responsibility |
|---|---|
| `actions` | `ActionExecutor` turns a button press (`ButtonAction`) into a config change — brightness/color/pattern/scene — via injected callbacks, without depending on any other module directly. |
| `automations` | `AutomationManager` matches an inbound mesh `GenericEvent` against the configured `AutomationBinding` table and fires the first matching rule's actions into `ActionExecutor` — the decentralized automation engine's seed (see `mesh`'s `GenericEventMsg`). |
| `battery` | `BatteryMonitor` samples the battery ADC pin and derives a charge percentage plus on-battery/charging state. |
| `buttons` | `ButtonManager` debounces GPIO/TCA9555-expander buttons (short/long press, double-click) and fires the matching action into `ActionExecutor`. |
| `config` | Shared config types (`Color`, `LightConfig`, `GroupConfig`, `DeviceConfig`, ...) and the `Config` singleton that persists them to LittleFS/NVS. |
| `io` | Generic hardware building blocks not specific to one feature, currently the `Tca9555Expander` I2C GPIO expander wrapper. See `src/io/README.md`. |
| `led` | `LedDriver` interface plus `Ws2801Driver`/`Ws2812bDriver` implementations that actually push pixel data to the strip/matrix. |
| `logging` | `Logger`, a static leveled logger fanning out to pluggable sinks (serial, web UI). |
| `mesh` | ESP-NOW peer mesh (no router required): message types, peer discovery, channel coordination, WiFi-connection election, and encrypted config push. See [mesh-compatibility.md](mesh-compatibility.md). |
| `mqtt` | `MqttManager` publishes Home-Assistant-discoverable MQTT entities per group and applies incoming MQTT commands. |
| `patterns` | `Pattern` interface (one animation per light) and `PatternRunner`, which owns and ticks the active pattern for a light. Individual files (`Breathing.h`, `Candle.h`, ...) are concrete patterns. |
| `scenes` | `SceneManager` reads/writes scene JSON files; `SceneSyncManager` syncs them between mesh peers. |
| `sound` | `SoundDriver` interface for the onboard audio codec (`Es8311Driver`). See `src/sound/README.md`. |
| `storage` | `SdCardManager` wraps the onboard microSD reader for uploaded sound files. See `src/storage/README.md`. |
| `timesync` | `TimeSync` gets wall-clock time from NTP or a mesh peer, for patterns like `TimeMatrix`. |
| `update` | `Updater`, GitHub-releases-based OTA firmware/filesystem updater, including PR-build tracking. |
| `web` | `BatteryWebServer` serves the dashboard and REST/WebSocket API, driving the rest of the system through injected callbacks. |

## Control flow

`main.cpp` wires all modules together through `std::function` callbacks — no
module holds a direct pointer to another except where explicitly passed
(e.g. `PeerRegistry*` into `WebServer::begin`).

Every input source — `ButtonManager`/`AutomationManager` (via `ActionExecutor`),
`MqttManager`, `WebServer`, and inbound `MeshManager` config messages — funnels through a
shared `applyAndPropagateLightConfig()` helper: it updates `Config`, pushes
the new config into the affected lights' `PatternRunner`s, then
re-broadcasts over `MeshManager` and republishes to `MqttManager`.

Each loop iteration, every light's `PatternRunner` ticks its active
`Pattern`, which renders pixels through that light's `LedDriver`. `scenes`
supplies the frame data patterns like `SceneMatrix`/`SceneString` render.

`mesh` keeps all peers on the same radio channel and elects one peer to
hold the WiFi connection; `battery` and `logging` are cross-cutting,
feeding status into mesh presence beacons/MQTT telemetry and log sinks
respectively.
