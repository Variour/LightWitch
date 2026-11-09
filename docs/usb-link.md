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
- Any device can be the bridge. On a Buzzerboard, while the link is open after
  `hello`, a soft white pulse runs from the USB-C end down the column over the
  device's own colours (`config look`), or over blue without them.
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

## `game` (bridge): orientation and move rounds

Additions to the gateway protocol v0 (firmware spec "Lage v0"). Everything
else in `game` is as in the gateway protocol.

### Orientation (`lage`)

A Buzzerboard in a game session reads its orientation sensor
(LSM6DS3TR-C, 6D detection). Directions are as seen by the person holding
the device:

| `lage` | Held | up | right | down | left | trigger left / right |
|---|---|---|---|---|---|---|
| `hochkant` | upright in one hand, USB-C at the top | UP | RIGHT | DOWN | LEFT | - |
| `quer` | landscape, buzzer right, LED column at the top | LEFT | UP | RIGHT | DOWN | L2 / L1 |
| `quer-links` | landscape, buzzer left, LED column at the bottom | RIGHT | DOWN | LEFT | UP | R1 / R2 |
| `liegend` | flat on a table, front up | UP | RIGHT | DOWN | LEFT | - |

- A new orientation counts after 2 s of stable holding. Upside down
  (USB-C at the bottom) and front down are no orientation: the last one stays.
- The device shows a new orientation once: the direction colours (up light
  orange, right blue, down purple, left light blue) run once around the D-pad
  and on to their keys, then one LED runs through the column in its new
  reading direction. Then the new key mapping applies.
- Until an orientation is known the device reads its keys as in
  `hochkant`. Devices other than the Buzzerboard have no sensor and never
  report one.
- Key colours in `game round` belong to logical keys: `"keys":{"up":"green"}`
  lights the key that is up as the device is held (in `hochkant` and
  `liegend` the UP key, as before).
- Choice rounds: answers 1-4 stay up, right, down, left, and their keys
  light in the direction colours above, by logical direction. The console
  may keep sending colour names in `keys`; for choice the device ignores
  them and takes only the key names.
- The LED column fills in reading direction: from the bottom in `hochkant`
  and `liegend`, from the left in `quer` and in `quer-links` (as held).

`join`, `device` and `value` lines carry `"lage"` when the device knows its
orientation, and leave it out otherwise:

- `join`, `device`: the device's orientation. A device sends its status at
  once when it takes a new orientation, so a `device` line follows right away.
- `value`: the orientation the input was read in.

`{"ns":"game","ev":"device","n":7,"mac":"02:00:00:00:00:07","fw":"4a45fc32","proto":1,"bat":81,"chg":false,"rssi":-48,"round":23417,"seq":2,"lage":"quer"}`

### `hold` in `game round`

| `hold` | Devices |
|---|---|
| `frei` (default, may be left out) | follow their orientation |
| `hochkant`, `quer`, `liegend` | in any other orientation the device is locked: keys do nothing and nothing is reported. `quer-links` counts as `quer`. |
| `fest` | the orientation at the moment the round opens stays for the whole round |

- Not with `idle` (`bad_field`, "omit hold for idle"). Like type, keys and
  scale, `hold` is fixed for a running round (`round_fixed`).
- A locked device, while the round is announced or open: key LEDs, ring
  and column off, the status dot pulses slowly white (provisional). A closed
  round shows its result in any orientation.
- Only a known orientation that does not match locks a device. A device
  without a known orientation (no sensor, or none accepted yet) is never
  locked; its inputs carry no `lage`.

### Move rounds (`"type":"move"`)

`{"ns":"game","cmd":"round","round":23417,"type":"move","phase":"open","keys":{"up":"white","right":"white","down":"white","left":"white"},"hold":"quer"}`

- `keys`: exactly `up`, `right`, `down` and `left`. Phases as for other rounds.
- While open, each press of a direction key is one key event (no
  auto-repeat); buzz and enter do nothing. The device numbers its presses
  with `kseq`: 8 bit, per device and round, 1-255, after 255 comes 1 (0
  never occurs); a new join starts again at 1. It keeps up to 8
  unacknowledged presses and sends them again until the bridge's echo
  shows them. A `kseq` is new when it is 1-127 ahead of the last one, mod
  256.
- The bridge writes each press exactly once, in order per device:

`{"ns":"game","ev":"key","n":7,"round":23417,"key":"right","kseq":12,"lage":"quer"}`

| Field | Meaning |
|---|---|
| `n` | device number |
| `round` | the round the press belongs to |
| `key` | `up`, `right`, `down`, `left`: logical with `lage`, the physical key without it |
| `kseq` | the device's press counter |
| `lage` | the orientation the press was read in; left out when unknown |

- The bridge passes a press on once its line has room in the bridge's queue
  (64 presses); the device repeats the others. Key lines come before
  `device` and `value` lines.
- The game logic (positions, winner) stays in the console.
- Device display (provisional): the four direction keys glow dimmed in the
  round's key colour while open; a press pulses its key until acknowledged.

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
  off and the battery reads as unknown (`bat` and `vbat` `null`).
- Input current limit 2 A with the ILIM pin off, at every USB source;
  charge current 1000 mA. On a weaker source VBUS sags to 4.6 V (VINDPM)
  and the charger takes less.
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
| `{"ns":"config","cmd":"get","id":4}` | `{"ns":"config","ev":"config","id":4,"wifiCount":1,"token":true,"target":"store","look":true,"number":12,"fw":"4a45fc32"}` |
| `{"ns":"config","cmd":"token","id":5,"token":"github_pat_…"}` | `{"ns":"config","ev":"token","id":5,"ok":true,"token":true}` |
| `{"ns":"config","cmd":"look","id":6,"leds":["#ffffff","#ff0800",…,"#ff0800"]}` | `{"ns":"config","ev":"look","id":6,"ok":true,"leds":24}` |

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
- `look`: the device's own colours on the score column, 24 `"#rrggbb"`
  from LED1 to the USB-C end, or `[]` to remove them. Stored in NVS. Active
  and idle, the column breathes these colours at the display brightness
  instead of the idle pattern, in turn with the battery level.
- `get`: `look` says whether own colours are set; `number` is `null` without a device number; `fw` is the firmware
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
- `leds`: all 34 LEDs white, red, green, blue at the display brightness, at least 8 (0.6 s each),
  then one white LED runs along the chain (60 ms per LED).
- `keys`: each key lights an LED while the test runs: BUZZ the ring, the
  D-pad keys their own LED, L1, L2, R1, R2 and POWER every other column LED
  from the USB-C end. Keys do nothing else meanwhile.
- `charge`: charges at the `event` target for 10 s with the LEDs dark and
  returns the mean IBUS, IBAT and VBUS of the charger ADC samples; `ok`
  is `false` without samples.
- The link closing ends a running test.
