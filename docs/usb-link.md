# USB link

A LightWitch device speaks NDJSON on its USB serial port (USB-Serial-JTAG on
the ESP32-C5/S3, the UART bridge on the ESP32). One JSON object per line, at
most 512 bytes per line in both directions. Commands carry `"ns"` and
`"cmd"`, replies and events carry `"ns"` and `"ev"`, and an optional `"id"`
(integer, or a string of 1-16 characters) is echoed in the reply.

Builds with `FEATURE_GAME=1` (esp32dev, esp32s3, esp32c5) have the link; the
esp32c3 build does not.

## Namespaces

| Namespace | Version | Who | Purpose |
|---|---|---|---|
| `link` | 0 | any device | `hello`, `ping`, `bye` |
| `game` | 0 | bridge | game session and rounds (gateway protocol v0) |
| `config` | 0 | any device | WiFi list, charge target, maintenance AP |
| `test` | 0 | any device | factory test |
| `fleet` | 0 | bridge | fleet commands to the mesh, status of every device |

- `link hello` makes the device the bridge until its next restart. Its reply
  lists every namespace: `"spaces":{"link":0,"game":0,"config":0,"fleet":0,"test":0}`.
- A `config` or `test` line opens the link without `hello`, so a device
  answers on USB without becoming the bridge (factory test, setup).
- `game` and `fleet` need a `hello` first; without it they get the error `no_hello`.
- While the link is open, log lines stay off the port. The link closes after
  10 s without a line; send `link ping` every few seconds to keep it open.
- Lines that are not JSON objects with `ns` (boot messages, log lines before
  the link opens) are not part of the protocol.

## Errors

`{"ns":"link","ev":"error","id":…,"code":"…","refNs":"…","refCmd":"…","field":"…","msg":"…"}`

Codes of the gateway protocol (`bad_json`, `bad_field`, `unknown_ns`,
`unknown_cmd`, `line_too_long`, `version`, `session_active`, `no_session`,
...) plus:

| Code | Meaning |
|---|---|
| `no_hello` | `game` or `fleet` without a `link hello` |
| `unsupported` | `config ap` on a device that runs a fallback AP (every board except the Buzzerboard) |
| `store_failed` | the WiFi list or the charge target could not be saved |
| `busy` | `fleet`: earlier commands are still being repeated; `test`: another test runs |
| `no_usb` | `test charge` without USB power and a charger |
| `session_active` | `fleet` while the bridge runs a game session; `test leds/keys/charge` on a participant in a session |

## Device states (Buzzerboard, outside a game session)

| State | When | Behaviour |
|---|---|---|
| `active` | normal operation | keys, LEDs and mesh as usual; no fallback AP on the Buzzerboard |
| `charging` | USB power and no host data link (no link traffic, the USB-Serial-JTAG sees no host) | keys do nothing, no AP, no WiFi except for an update, charging display at low brightness, radio only for fleet frames |
| `standby` | on battery, 8 h without key, session, USB or host link | LEDs and LED rail off, radio listens 500 ms of every second, fleet frames only |

- From `standby` a key, a session invite, USB power or a host link brings the device back.
- `standby` ends in ESP deep sleep once the battery is at 80 % or below
  (without a battery percentage: VBAT at or below 4.02 V). In sleep the
  device wakes on BUZZ, UP, DOWN, LEFT, ENTER or POWER, and every 60 s on a
  timer: with USB it boots (charging mode), with VBAT below 3.5 V or after 7
  days in sleep it enters ship mode (wake: POWER about 1 s, or USB).
- After a key wake from sleep the device goes to standby after 5 min instead
  of 8 h, unless a further key press, a session invite, USB power or a host
  link follows; then the 8 h apply. Going back to sleep from there keeps
  counting toward the 7 days. A device taken off USB always gets the 8 h.
- Weak USB supply: in charging mode, if the battery keeps discharging (IBAT
  below -40 mA) for 60 s while USB power is present, the device turns its
  LEDs off, lets the radio listen 500 ms of every second and sets error bit
  9, until USB is unplugged.
- Charging starts off at boot and goes on once VBAT shows a cell (above
  2.8 V, at least 15 s after charging went off). Without a cell it stays
  off and the battery reads as unknown (`bat` and `vbat` `null`). Charge
  current 200 mA.
- VBAT below 3.3 V for 30 s on USB (any state): ESP deep sleep while the
  charger charges on; the 60 s timer wake boots again from VBAT 3.5 V.
- The low-battery cut-off (below 3.3 V for 30 s on battery → ship mode)
  applies in every state.
- A device without a charger is always `active`.
- Only a detected Buzzerboard sends FleetStatus and acts on fleet commands.

### Build flags for a bring-up image

The times and levels above can be shortened with `-D` build flags (defaults
in `src/fleet/FleetConfig.h`), e.g. in `PLATFORMIO_BUILD_FLAGS` or a local
`build_flags` line, without changing `platformio.ini`:

| Flag | Default | Meaning |
|---|---|---|
| `FLEET_INACTIVE_MS` | 28800000 (8 h) | on battery without activity until standby |
| `FLEET_WAKE_INACTIVE_MS` | 300000 (5 min) | the same after a key wake from sleep |
| `FLEET_SLEEP_TO_SHIP_S` | 604800 (7 days) | time in sleep until ship mode |
| `FLEET_STORAGE_PERCENT` | 80 | battery percentage at which standby ends in sleep |
| `FLEET_STORAGE_MV` | 4020 | the same as VBAT, without a battery percentage |

## `config` (any device)

| Command | Reply |
|---|---|
| `{"ns":"config","cmd":"wifi","id":1,"networks":[{"ssid":"Office","pass":"…"}]}` | `{"ns":"config","ev":"wifi","id":1,"ok":true,"count":1}` |
| `{"ns":"config","cmd":"charge","id":2,"target":"event"}` | `{"ns":"config","ev":"charge","id":2,"ok":true,"target":"event"}` |
| `{"ns":"config","cmd":"ap","id":3,"minutes":10}` | `{"ns":"config","ev":"ap","id":3,"ok":true,"minutes":10}` |
| `{"ns":"config","cmd":"get","id":4}` | `{"ns":"config","ev":"config","id":4,"wifiCount":1,"token":true,"target":"store","number":12,"fw":"4a45fc32"}` |
| `{"ns":"config","cmd":"token","id":5,"token":"github_pat_…"}` | `{"ns":"config","ev":"token","id":5,"ok":true,"token":true}` |

- `wifi` replaces the whole list: 0-5 networks, `ssid` 1-32 bytes, `pass` `""`
  (open network) or 8-63 bytes. Passwords are never echoed or logged. Five
  long entries do not fit one 512-byte line. The list takes effect at the
  next connection (an update, a restart).
- `token`: the GitHub token the device sends with release and PR downloads
  (0-127 bytes, `""` clears it), the same setting as **Personal access
  token** in the web UI. Set it on every device before a fleet `update`:
  GitHub allows 60 anonymous API requests per hour and IP address, and a
  device makes up to 3 per update. The reply and `get` only say whether a
  token is set; it is never echoed or logged.
- `charge`: `store` (charger VREG 4.05 V, about 80 %) or `event` (4.20 V,
  100 %). Stored in NVS, default `store`, applied at once; `store_failed`
  (nothing changed) if NVS does not take it. Lowering the target while VBAT
  is above the new VREG on USB trips the charger's battery over-voltage
  protection: it stops charging until VBAT has come down. That is expected
  and not reported as an error (up to 4.25 V).
- `ap`: the maintenance AP (SSID = device name, password = the AP password
  from the device config) for `minutes` (1-60, default 10), on the current
  radio channel. Buzzerboard only; it has no fallback AP. Holding L1 + R1 for
  3 s in active state opens it for 10 minutes as well. While it is open the
  status LED pulses blue slowly.
- `get`: `number` is `null` without a device number; `fw` is the firmware
  build (first 4 bytes of the app image's SHA-256, as in `link hello`).

## `fleet` (bridge)

| Command | Mesh frame |
|---|---|
| `{"ns":"fleet","cmd":"update","id":5,"source":"release"}` | every device installs the latest release |
| `{"ns":"fleet","cmd":"update","id":5,"source":"pr","pr":481}` | every device installs the build of PR 481 |
| `{"ns":"fleet","cmd":"charge","id":6,"target":"event"}` | every device sets its charge target |
| `{"ns":"fleet","cmd":"off","id":7}` | every device on battery enters ship mode; devices on USB ignore it |
| `{"ns":"fleet","cmd":"ap","id":8,"mac":"02:00:00:00:00:11"}` | that device opens its maintenance AP for 10 minutes |

Each is answered with `{"ns":"fleet","ev":"sent","id":…}` once queued. The
bridge sends every command three times, 300-700 ms apart, with one nonce;
devices act once per nonce. Only Buzzerboards act on fleet commands, and
not while in a game session.

- `update`: a device needs a WiFi network and, on battery, VBAT of at least
  3.5 V. It connects WiFi for the update (also from charging mode), shows the
  update on its LEDs, installs and restarts. A device that runs the requested
  build already stays as it is. The fleet command does not need the device's
  "PR installs" setting. A Buzzerboard bridge updates itself too, after its
  three repeats are out; the console reconnects after the bridge's restart.
- `charge`: a Buzzerboard bridge sets its own charge target too.
- `off`, `ap`: not applied to the bridge itself (it runs on USB). A device
  in charging mode ignores `ap`.

### `status` event

One line per device that sent a FleetStatus frame, the latest value per
device (like `game value`): after every new frame, and all of them again
after `link hello`. A Buzzerboard bridge lists itself too (`rssi` 0).

`{"ns":"fleet","ev":"status","mac":"02:00:00:00:00:11","n":12,"state":"charging","bat":64,"vbat":3912,"chg":"charging","target":"store","usb":true,"wifi":true,"fw":"4a45fc32","upd":"idle","pct":0,"updErr":"","err":0,"rssi":-48,"age":2}`

| Field | Meaning |
|---|---|
| `mac` | the device |
| `n` | device number, `null` without one |
| `state` | `active`, `charging`, `standby` |
| `bat` | battery percent, `null` if unknown |
| `vbat` | battery voltage in mV, `null` if unknown |
| `chg` | `charging`, `full`, `discharging`, `fault`, or `none` (no charger) |
| `target` | `store` or `event` |
| `usb` | USB power present |
| `wifi` | at least one WiFi network configured |
| `fw` | firmware build, 8 hex digits |
| `upd` | last fleet update: `idle`, `checking`, `downloading`, `installing` (written, restarting), `failed` |
| `pct` | update progress 0-100 |
| `updErr` | why the last fleet update failed: `""`, `no_wifi`, `wifi_failed`, `low_battery`, `failed` |
| `err` | error bits, see below |
| `rssi` | of the last frame at the bridge |
| `age` | seconds since that frame |

### Error bits (`err`)

| Bit | Name | Meaning |
|---|---|---|
| 0 | charger offline | the charger is configured but does not answer |
| 1 | TS cold | battery NTC cold, charging suspended (on USB only) |
| 2 | TS hot | battery NTC hot, charging suspended (on USB only) |
| 3 | thermal shutdown | the charger IC is in thermal shutdown |
| 4 | battery fault | battery over-current or over-voltage protection |
| 5 | safety timer | the charge safety timer expired (on USB only) |
| 6 | supply fault | VBUS over-voltage or VSYS short/over-voltage |
| 7 | TS bias fault | NTC bias reference fault, e.g. no NTC (on USB only) |
| 8 | low battery | on battery below 3.5 V |
| 9 | weak supply | charging mode: USB power present but the battery discharges (see above) |

Bits 0-7 make `chg` `fault` while USB power is present. Bit 4 is left out
while VBAT is between a lowered charge voltage and 4.25 V (see `config charge`).

## `test` (any device, factory test)

| Command | Reply |
|---|---|
| `{"ns":"test","cmd":"info","id":1}` | `info`, see below |
| `{"ns":"test","cmd":"leds","id":2}` | `{"ns":"test","ev":"leds","id":2,"ok":true}` when done |
| `{"ns":"test","cmd":"keys","id":3}` | `{"ns":"test","ev":"keys","id":3,"ok":true}`, then `{"ns":"test","ev":"key","key":"l1"}` per press |
| `{"ns":"test","cmd":"keys-end","id":4}` | `{"ns":"test","ev":"keys-end","id":4,"ok":true}` |
| `{"ns":"test","cmd":"charge","id":5}` | `{"ns":"test","ev":"charge","id":5,"ok":true,"samples":30,"ibus":441,"ibat":612,"vbus":4990}` after 10 s |
| `{"ns":"test","cmd":"reboot","id":6}` | `{"ns":"test","ev":"reboot","id":6,"ok":true}`, then a restart into normal operation |

- `info`:
  `{"ns":"test","ev":"info","id":1,"mac":"…","fw":"4a45fc32","n":12,"charger":{"found":true,"rev":1,"fault":0},"vbat":3912,"vbus":5012,"ibus":420,"ibat":380,"ts":58.9,"tsC":25.1,"tdie":31.5,"chg":"charging","imu":{"id":106,"g":[0.012,-0.020,1.003]}}`.
  Voltages in mV, currents in mA (`ibat` positive while charging), `ts` in %
  of the TS bias and `tsC` in °C (USB only, else `null`), `tdie` in °C,
  `charger.fault` the charger's FAULT_Status_0 register, `imu.id` the
  WHO_AM_I value (106 = 0x6A), `imu.g` one accelerometer reading in g.
  Without a charger reading the measurements are `null`; without an IMU `imu` is `null`.
- `leds`: all 34 LEDs white, red, green, blue at brightness 20 (0.6 s each),
  then one white LED runs along the chain (60 ms per LED).
- `keys`: each key lights an LED while the test runs: BUZZ the ring, the
  D-pad keys their own LED, L1, L2, R1, R2 and POWER every other column LED
  from the USB-C end. Keys do nothing else meanwhile.
- `charge`: charges at the `event` target for 10 s with the LEDs dark and
  returns the mean IBUS, IBAT and VBUS of the charger ADC samples; `ok`
  is `false` without samples. IBUS above about 500 mA means R5 (ILIM) is wrong.
- The link closing ends a running test.
