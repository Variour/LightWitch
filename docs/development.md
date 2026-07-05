# Development

For updating devices over WiFi and running the web UI locally with PlatformIO / Node.js installed.

> For the initial USB flash of a fresh device, see [Initial installation (USB, from source)](../README.md#initial-installation-usb-from-source) in the README.

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

Open **http://localhost:8080** in a browser. The mock server (`server/index.js`) handles all REST endpoints and WebSocket, with scenes stored in memory for the duration of the process. Auth is skipped entirely when no environment variables are configured.

> Requires Node.js.
