# USB serial device config (#355)

Lets a device be fully configured — network, hardware, groups, scenes,
sound storage, everything the web UI exposes — over a USB cable, without
the device being connected to WiFi or the user being connected to the
device's own access point. Useful whenever the AP is unreachable, disabled,
or simply inconvenient (e.g. a device buried behind other hardware where
only the USB port is reachable).

## Why this shape

`WebServer.h`'s REST API already models every setting as JSON; the missing
piece was a way to reach it without a network. Reusing the JSON model and
avoiding a second, hand-maintained copy of every endpoint's logic drove two
decisions:

- **Every endpoint is registered once**, as a transport-agnostic
  `(ApiRequest, ApiResponse)` handler in a shared table (`ApiTypes.h`,
  `BatteryWebServer::_get/_post/_postStream`). The existing AsyncWebServer
  HTTP routes and the new `SerialConfigServer` both call into this same
  table — a new endpoint only needs to be written once to work over both
  transports.
- **The frontend (`data/index.html`) is unmodified in its endpoint calls.**
  Serial mode replaces `window.fetch` with an implementation that speaks
  the wire protocol below; every existing `fetch('/api/...')` call site
  keeps working without changes. The one exception, the XHR-based `.wav`
  upload, branches explicitly on serial mode (XHR has no serial
  equivalent to shim).

## Transport

The device's own `Serial` connection (the same one used for boot-time log
output) doubles as the config channel — no second UART, no new hardware.
On ESP32-C3/S3 boards (built with `ARDUINO_USB_CDC_ON_BOOT=1`) this is
native USB-CDC; on plain ESP32 it's the usual UART0-via-USB-bridge. Either
way, from a browser's perspective it's just a serial port reachable through
the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
(Chrome, Edge, Firefox 151+ — no Safari).

Since the device can't serve its own config page without a network
connection, the page is instead published to GitHub Pages (see
`.github/workflows/pages-deploy.yml`) — the exact same `data/index.html`
that ships inside the firmware, detecting at runtime which context it's in
(see the startup probe near the bottom of that file's script).

## Wire protocol

Newline-delimited frames, each prefixed with `@BLCFG ` so they can never be
mistaken for a plain-text `Logger` line sharing the same wire. While a
config session is active, the device suspends its serial log sink (see
`Logger`/`SerialConfigServer::sessionActive()`) so the two can't interleave
mid-frame; an idle timeout (30s) resumes logging automatically if the
browser disappears (tab closed, cable unplugged) without a clean `BYE`.

```
Browser -> device:
  @BLCFG HELLO
  @BLCFG REQ {"id":1,"method":"GET","path":"/api/config"}
  @BLCFG REQ {"id":2,"method":"POST","path":"/api/wifi/add","body":{"ssid":"...","password":"..."}}
  @BLCFG REQ {"id":3,"method":"POST","path":"/api/storage/upload",
              "query":"song.wav","index":0,"total":123,"chunk":"<base64>"}
  @BLCFG BYE

Device -> browser:
  @BLCFG READY {"proto":1,"device":"<name>","version":"<fw>"}
  @BLCFG RES {"id":1,"status":200,"body":{...}}
  @BLCFG RES {"id":3,"ack":true,"index":0}                                    (mid-upload)
  @BLCFG RES {"id":4,"status":200,"index":0,"total":9000,"chunk":"<base64>","final":false}
  @BLCFG BYE
```

- `id` is chosen by the browser and echoed back so a response can be
  matched to its request.
- `path`/`method` map directly onto the same paths the REST API uses
  (`/api/config`, `/api/wifi/add`, ...).
- `query` carries the one query-string parameter a couple of endpoints take
  outside the JSON body (`id` for `GET /api/scenes/get`, `name` for
  `POST /api/storage/upload`).
- Streaming endpoints (`POST /api/scenes/save`, `POST /api/storage/upload`)
  split their body into ≤2048-byte chunks (`SerialConfigServer::CHUNK_BYTES`
  / `SERIAL_CHUNK_BYTES` in `data/index.html`), base64-encoded, one chunk
  per `REQ` frame. The device acks each non-final chunk (`{"ack":true}`) so
  the browser paces the next one — there's no window/pipelining, only one
  chunk in flight at a time. The response is only sent after the final
  chunk (`index + len(chunk) >= total`).
- A large **response** body (`GET /api/scenes/get`'s raw scene file, or any
  oversized JSON body) is sent back the same chunked way — `chunk`/`index`/
  `total`/`final` — instead of one inline `body` frame. Everything else
  (the overwhelming majority of endpoints) fits in a single frame.

## Constraints

- **One request in flight at a time.** The browser must wait for a response
  (or upload ack) before sending the next request or chunk — this matches
  one browser tab talking to one device over one serial port; it is not a
  pipelined/concurrent protocol.
- **Out of scope today:** live push (peers/groups/log) has no serial
  equivalent — `data/index.html` falls back to polling `/api/config` and
  `/api/peers` every 5s while connected this way instead of the WebSocket
  push HTTP mode gets. Live device log streaming is likewise unavailable
  in serial mode (the log sink is suspended for the whole session).
- Baud rate is the existing `115200` used for logging — fine for config
  JSON and reasonably sized scenes, but slow for multi-megabyte `.wav`
  uploads. Not changed here; revisit if real-world upload times turn out
  to matter.
