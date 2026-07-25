# Development

For building from source and updating devices over WiFi with PlatformIO installed.

---

## Initial installation (USB)

Do this once per device, with a USB cable connected.

**1. Flash firmware**
```bash
pio run -e esp32c3 -t upload    # For ESP32-C3 devices (e.g., lightwitch1)
pio run -e esp32dev -t upload   # For ESP32-WROOM-32 devices
pio run -e esp32s3 -t upload    # For ESP32-S3 devices
```

**2. Flash the web UI filesystem**
```bash
pio run -e esp32c3 -t uploadfs    # For ESP32-C3 devices (e.g., lightwitch1)
pio run -e esp32dev -t uploadfs   # For ESP32-WROOM-32 devices
pio run -e esp32s3 -t uploadfs    # For ESP32-S3 devices
```

Both steps are required on a fresh device. Use the environment matching your device type. After this, all further updates can be done over WiFi.

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

Replace `lightwitch1` with the target device name configured in the web UI.

**Firmware only:**
```bash
pio run -e lightwitch1_ota -t upload
```

**Filesystem only:**
```bash
pio run -e lightwitch1_ota -t uploadfs
```

**Firmware + filesystem:**
```bash
pio run -e lightwitch1_ota -t upload && pio run -e lightwitch1_ota -t uploadfs
```

> Settings (`config.json`) are never affected by firmware updates. Filesystem updates preserve settings automatically via NVS backup.

---

## Installing PR firmware builds (experimental)

CI publishes every open PR's `esp32dev` and `esp32s3` builds as a GitHub prerelease (tag `pr-<number>`), so a device can install and track it directly over the air — no PlatformIO needed. `esp32c3` is not built for PRs and stays unsupported.

1. In the web UI, **Settings → Updates**: set `Repository` and a GitHub PAT, enable **"Allow installing PR builds"**, then click **Save** — none of these need a reboot to take effect. See [README: Firmware updates from GitHub releases](../README.md#firmware-updates-from-github-releases-device-web-ui) for the required token permissions.
2. In **Firmware update**, click **"Load open PRs"**, pick the PR from the dropdown, then **"Install selected build"**.
3. The device flashes firmware + filesystem, preserves scenes, and reboots. It now tracks that PR — the normal "Check"/"Install update" flow follows new pushes to it instead of official releases.
4. To go back to official releases, click **"Back to stable"** next to the tracking badge — it installs the latest release directly, without loading the PR list.

---

## Local web UI development

Run the web interface locally without hardware:

```bash
npm install
npm run dev
```

Open **http://localhost:8080** in a browser. The mock server (`server/index.js`) handles all REST endpoints and WebSocket, with scenes stored in memory for the duration of the process.

`npm run dev` expects the auth environment variables (`GITHUB_CLIENT_ID`, `SESSION_SECRET`, `TOKEN_SECRET`, `ALLOWED_GITHUB_USERS`); without them every request redirects to the login. For local work without that setup use:

```bash
npm run dev:no-auth
```

The graph editor (mock-only, see the [rework plan](lightwitch-rework-plan.md)'s M8 section) is served from the same process at **http://localhost:8080/graphs.html**.

> Requires Node.js.

---

## Linting & formatting

**JavaScript** (`server/`, the inline script in `data/index.html`):
```bash
npm install
npm run lint
```
Config: `eslint.config.js`. Checked in CI on every PR that touches JS.

**C++ firmware** (`src/`): formatted per `.clang-format` (based on the Google style), checked as a whole file in CI:
```bash
clang-format -i path/to/file.cpp
```
Note that the existing codebase isn't reformatted to this style yet, so CI on `src/**` changes may fail on pre-existing code until it is.
