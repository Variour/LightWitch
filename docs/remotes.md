# Remote control research (#308)

Goal: a device detached from the light itself that can trigger the same
actions available to buttons today (brightness, scenes, patterns, etc.).
This is research only — no implementation yet, pending specific
requirements (battery life, form factor, cost target).

## Key finding

`GroupConfig` (including its `LightConfig`) is already synced mesh-wide over
ESP-NOW (`GroupSyncMsg`/`LightConfigMsg` in `src/mesh/MeshTypes.h`), and
buttons already dispatch through `ActionExecutor::execute()`
(`src/actions/ActionExecutor.h:27`) against a `groupId`, not a specific
device. `ActionExecutor.h:10-14` documents itself as reusable "from
MQTT/API/mesh trigger sources later."

Consequence: a mesh device with buttons but no LED string attached would
already act as a remote for any group on the mesh, with no new mesh
message type required — the action/mesh layers were built to support this.

## Options considered

**A. Same firmware, no LEDs.** A device variant that's the same firmware
family, joins the mesh normally, configured through the existing web UI,
just has no LED string attached. Reuses buttons, `ActionExecutor`, mesh
sync, and config UI unchanged. Lowest cost; consistent with the
same-firmware-only mesh compatibility policy (`docs/mesh-compatibility.md`).
Caveat: current `ButtonManager` is loop-polled with an always-on ESP-NOW
radio, by deliberate no-interrupts house style
(`src/buttons/ButtonManager.h:8`) — fine for mains/rechargeable power, bad
for coin-cell battery life.

**B. Trimmed low-power build.** Same as A, separate PlatformIO env (e.g.
ESP32-C3), with real deep-sleep + wake-on-GPIO and duty-cycled ESP-NOW for
coin-cell battery life. More engineering; a deliberate, scoped exception to
the no-interrupts house style.

**C. Off-the-shelf BLE remote.** Add a BLE host role to the firmware to
support consumer BLE HID remotes (camera shutter clickers etc.). No BLE
code exists in the project today — this is a new subsystem, plus a
keycode → `ButtonAction` mapping. Only worth it if the goal is reusing
hardware people already own rather than a companion device.

**D. RF / IR / Zigbee — ruled out for now.**
- Zigbee needs an 802.15.4 radio, not present on any current board
  (esp32dev / esp32-c3 / esp32-s3).
- IR is line-of-sight only, a poor fit for a device meant to work from
  anywhere in a room.
- RF (433/868 MHz) works and gives good battery life, but adds a receiver
  module to the BOM of every light and a code-learning/pairing UX for
  arbitrary consumer remotes.

## Decision

Directions A and B are the ones worth pursuing. Implementation is
postponed until the specific requirements for a remote (power model, form
factor, cost) are settled — see open questions below.

## Open questions before implementation

- Power model: mains/rechargeable (A is sufficient) vs. coin-cell/long
  battery life (needs B's deep-sleep rework).
- Form factor / button count for a first remote.
- Whether a "no LEDs" device needs any UI/config changes (e.g. hiding
  light-specific settings) or works as-is with `lightCount = 0`.
