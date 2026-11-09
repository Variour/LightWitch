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
| `battery` | `BatteryMonitor` samples the battery ADC pin (or reads VBAT from the charger, see `power`) and derives a charge percentage plus on-battery/charging state. |
| `buttons` | `ButtonManager` debounces GPIO/TCA9555-expander buttons (short/long press, double-click), fires the matching action into `ActionExecutor`, and reports raw press/release edges. Keys carry a stable role (`ButtonRoles.h`) other modules look them up by. |
| `config` | Shared config types (`Color`, `LightConfig`, `GroupConfig`, `DeviceConfig`, ...) and the `Config` singleton that persists them to LittleFS/NVS. |
| `fleet` | Fleet package of the quiz buzzer: device states (`PowerState.h`: active, charging, standby, and the deep sleep and ship mode of the transport protection in `DeepSleep.h`), the charge target, `FleetStatus`/`FleetCommand` frames (`FleetFrames.h`, `FleetRadio.h`), "update all", the maintenance AP key combo, the charging and update displays (`FleetRender.h`) and the USB namespaces `config`, `fleet` and `test` (`FleetLink.h`, `FactoryTest.h`, see [usb-link.md](usb-link.md)). `FleetManager` wires them to the charger, WiFi, mesh, keys and LEDs; every tunable is in `FleetConfig.h`. |
| `game` | Game session (quiz buzzer): `Participant` joins a session on a buzz press during an invite and runs as a quiet participant until released, `Bridge` repeats the console's session and round state over ESP-NOW, and `Link` speaks NDJSON with the presenter's laptop over USB serial (more namespaces plug in as a `Link::Extension`). `GameManager` wires them to NVS, serial, radio channel and LEDs; frames are in `GameFrames.h`, every tunable in `GameConfig.h`. |
| `io` | Generic hardware building blocks not specific to one feature: the `Tca9555Expander` I2C GPIO expander wrapper, and `SerialPort`, the firmware's serial port for log lines and the USB link (USB-Serial-JTAG on esp32c3/c5/s3, UART0 on esp32dev, through the ESP-IDF drivers). See `src/io/README.md`. |
| `led` | `LedDriver` interface plus `Ws2801Driver`/`Ws2812bDriver` implementations that actually push pixel data to the strip/matrix. |
| `logging` | `Logger`, a static leveled logger fanning out to pluggable sinks (serial, web UI). |
| `mesh` | ESP-NOW peer mesh (no router required): message types, peer discovery, channel coordination, WiFi-connection election, and encrypted config push. `SoftAp.h` owns the device's softAP: the fallback AP on most boards, a time-limited maintenance AP on the Buzzerboard (no fallback AP). See [mesh-compatibility.md](mesh-compatibility.md). |
| `mqtt` | `MqttManager` publishes Home-Assistant-discoverable MQTT entities per group and applies incoming MQTT commands. |
| `patterns` | `Pattern` interface (one animation per light) and `PatternRunner`, which owns and ticks the active pattern for a light. Individual files (`Breathing.h`, `Candle.h`, ...) are concrete patterns. |
| `power` | `PowerManager` drives the BQ25628 charger (`Bq25628`): host mode and watchdog, charge voltage (the fleet's charge target), LED rail on battery (boost OTG, off in standby), battery ADC for `BatteryMonitor`, and ship mode on a long press of the power key, a low battery or a fleet `off`. |
| `scenes` | `SceneManager` reads/writes scene JSON files; `SceneSyncManager` syncs them between mesh peers. |
| `sound` | `SoundDriver` interface for the onboard audio codec (`Es8311Driver`). See `src/sound/README.md`. |
| `storage` | `SdCardManager` wraps the onboard microSD reader for uploaded sound files. See `src/storage/README.md`. |
| `timesync` | `TimeSync` gets wall-clock time from NTP or a mesh peer, for patterns like `TimeMatrix`. |
| `update` | `Updater`, GitHub-releases-based OTA firmware/filesystem updater, including PR-build tracking. |
| `web` | `BatteryWebServer` serves the dashboard and REST/WebSocket API, driving the rest of the system through injected callbacks. |

## Build features

`FEATURE_GAME` (0 or 1, set in every env's `build_flags` in `platformio.ini`) compiles the `game` and `fleet` modules in. It is 1 for esp32dev, esp32s3 and esp32c5, and 0 for esp32c3, whose OTA slot has no room for them; there the firmware behaves as before the game module.

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
