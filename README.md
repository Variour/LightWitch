# Battery Light

ESP32-based addressable RGB light with web interface, OTA updates, and mesh sync.

---

## Initial installation (USB)

Do this once per device, with a USB cable connected.

**1. Flash firmware**
```bash
pio run -e esp32c3 -t upload    # For ESP32-C3 devices (e.g., batterylight1)
pio run -e esp32dev -t upload   # For ESP32-WROOM-32 devices
```

**2. Flash the web UI filesystem**
```bash
pio run -e esp32c3 -t uploadfs    # For ESP32-C3 devices (e.g., batterylight1)
pio run -e esp32dev -t uploadfs   # For ESP32-WROOM-32 devices
```

Both steps are required on a fresh device. Use the environment matching your device type. After this, all further updates can be done over WiFi.

---

## First boot

Each device starts as a WiFi access point named after its MAC address (e.g. `light-a1b2c3`).

| Setting | Value |
|---------|-------|
| Network | `light-a1b2c3` (unique per device) |
| Password | `bl-9f4a2c81` |
| Web interface | http://192.168.4.1 |

1. Connect to the device's WiFi network.
2. Open **http://192.168.4.1** in a browser.
3. Under **Network**, set a memorable device name, enter your WiFi credentials, and click **Save & Reboot**.

After reboot the device joins your WiFi and is reachable at **http://\<devicename\>.local** (e.g. http://batterylight1.local). The AP turns off automatically once connected.

> If WiFi is unreachable the device falls back to AP mode so you can always reach it at http://192.168.4.1 to fix credentials.

---

## Updating all devices (OTA)

Use this for routine updates once all devices are on WiFi.

**Firmware + filesystem (most common):**
```bash
pio run -t upload_all
```
Uploads firmware to all devices in parallel, waits 15 s for reboots, then uploads the filesystem in parallel. Settings are preserved automatically.

**Filesystem only** (when only `data/` files changed):
```bash
pio run -t upload_all_fs
```

---

## Updating a specific device (OTA)

Replace `batterylight1` with the target device name configured in the web UI.

**Firmware only:**
```bash
pio run -e batterylight1_ota -t upload
```

**Filesystem only:**
```bash
pio run -e batterylight1_ota -t uploadfs
```

**Firmware + filesystem:**
```bash
pio run -e batterylight1_ota -t upload && pio run -e batterylight1_ota -t uploadfs
```

> Settings (`config.json`) are never affected by firmware updates. Filesystem updates preserve settings automatically via NVS backup.

---

## Local web UI development

Run the web interface locally without hardware:

```bash
npm install
npm run dev
```

Open **http://localhost:8080** in a browser. The mock server (`server/index.js`) handles all REST endpoints and WebSocket, with scenes stored in memory for the duration of the process.

> Requires Node.js.

---

## Web UI container

Every pull request builds a Docker image and pushes it to the GitHub Container Registry. A comment is posted on the PR with the exact command to run it:

```sh
docker run --rm -p 8080:8080 ghcr.io/<owner>/batterylight:pr-<N>
```

The `latest` tag tracks the `main` branch. No setup or secrets are required beyond the default `GITHUB_TOKEN`.

---

## Hardware

See [WIRING.md](WIRING.md) for wiring diagrams for both supported LED types.

| LED type | ESP32-WROOM-32 | ESP32-C3 |
|----------|----------------|----------|
| WS2812B (default) | Data: GPIO 25 | Data: GPIO 20 |
| WS2801 | Data: GPIO 25, Clock: GPIO 26 | Data: GPIO 20, Clock: GPIO 21 |

The LED type is configured per device in the web UI under **Network → LED Type**.
