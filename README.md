# Battery Light

ESP32-based addressable RGB light with web interface, OTA updates, and mesh sync.

---

## First install from a GitHub release (no tools required)

Use this to flash a fresh device using pre-built firmware from the GitHub releases page. All you need is a Chrome or Edge browser with WebSerial support.

**1. Download the firmware**

Go to the [latest release](https://github.com/Variour/batteryLight/releases/latest) and download the files for your board type:

| File | What it is |
|------|-----------|
| `bootloader-esp32c3.bin` | Bootloader for ESP32-C3 devices |
| `bootloader-esp32dev.bin` | Bootloader for ESP32-WROOM-32 devices |
| `partitions.bin` | Partition table (same for both board types) |
| `firmware-esp32c3.bin` | Firmware for ESP32-C3 devices |
| `firmware-esp32dev.bin` | Firmware for ESP32-WROOM-32 devices |
| `littlefs.bin` | Web UI filesystem (same for both board types) |

**2. Open the web flasher**

Go to **https://esp.huhn.me** in Chrome or Edge.

**3. Connect your device**

Plug in the ESP32 via USB, then click **Connect** and select the device's serial port.

**4. Add the files**

Add entries for your board type with these addresses:

**ESP32-C3:**

| Address | File |
|---------|------|
| `0x0` | `bootloader-esp32c3.bin` |
| `0x8000` | `partitions.bin` |
| `0x10000` | `firmware-esp32c3.bin` |
| `0x3C0000` | `littlefs.bin` |

**ESP32-WROOM-32:**

| Address | File |
|---------|------|
| `0x1000` | `bootloader-esp32dev.bin` |
| `0x8000` | `partitions.bin` |
| `0x10000` | `firmware-esp32dev.bin` |
| `0x3C0000` | `littlefs.bin` |

**5. Flash**

Click **Program** and wait for it to finish. The device will reboot automatically.

Continue with [First boot](#first-boot) below.

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
| Password | `batterylight` |
| Web interface | http://192.168.4.1 |

1. Connect to the device's WiFi network.
2. Open **http://192.168.4.1** in a browser.
3. Under **Network**, set a memorable device name, enter your WiFi credentials, and click **Save & Reboot**.

After reboot the device joins your WiFi and is reachable at **http://\<devicename\>.local** (e.g. http://batterylight1.local). The AP turns off automatically once connected.

> If WiFi is unreachable the device falls back to AP mode so you can always reach it at http://192.168.4.1.

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

---

## Online hosting (Azure Container Apps)

The web UI runs as a Docker container on Azure Container Apps, with a permanent `latest` deployment and per-PR preview environments.

### Authentication

Access is restricted to specific GitHub accounts via OAuth. The production container handles the OAuth flow; PR preview containers delegate to it, so only one OAuth App registration is needed regardless of how many PR environments exist.

**One-time setup:**

1. **Register a GitHub OAuth App** at github.com/settings/developers
   - Authorization callback URL: `https://<prod-fqdn>/auth/callback`

2. **Add GitHub Actions secrets** (repo → Settings → Secrets → Actions):

   | Secret | Where used | Value |
   |---|---|---|
   | `GITHUB_OAUTH_CLIENT_ID` | Prod container | Client ID from the OAuth App |
   | `GITHUB_OAUTH_CLIENT_SECRET` | Prod container | Client Secret from the OAuth App |
   | `ALLOWED_GITHUB_USERS` | All containers | Comma-separated GitHub usernames, e.g. `alice,bob,charlie` |
   | `AUTH_TOKEN_SECRET` | All containers | Random secret shared across all containers — `openssl rand -base64 32` |
   | `AUTH_SESSION_SECRET` | All containers | Random secret for session cookies — `openssl rand -base64 32` |

   Existing secrets (`AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`, `AZURE_RESOURCE_GROUP`, `GHCR_PASSWORD_B64`) are unchanged.

3. **Deploy infrastructure** by running the *Deploy infrastructure* workflow manually (or pushing a change to `infra/`).

### Deployments

| Event | Result |
|---|---|
| Push to `main` | Updates the permanent `batterylight-latest` container app |
| Open / push to a PR | Creates or updates a `batterylight-pr-<N>` container app; URL posted as a PR comment |
| PR closed / merged | PR container app and its registry image are cleaned up on next push to `main` |

---

## Firmware updates from GitHub releases (device web UI)

Devices can check for and install new firmware directly from GitHub releases without a computer attached.

### Setup

1. Go to **Settings → Firmware updates** in the device web UI.
2. Set **Repository** to `variour/batterylight` (the default).
3. Create a GitHub **fine-grained personal access token** with the following permissions:
   - **Repository access:** this repository only (`variour/batterylight`)
   - **Repository permissions:**
     - `Contents` — **Read-only** (required to download release assets)
     - `Metadata` — **Read-only** (automatically included, required to access repository metadata)

   No other permissions are needed. Classic tokens work too with the `repo` scope, but a fine-grained token with read-only access is safer.

4. Paste the token into **Personal access token** and click **Save & Reboot**.

### Usage

- Click **Check** to query the latest GitHub release and compare it to the device's current firmware version.
- If a newer version is available, an **Install update** button appears. Click it to flash both firmware and filesystem over WiFi. The device reboots automatically when done.
- Settings (WiFi, groups, etc.) are preserved — the device restores them from NVS after the filesystem is reflashed.

> **Note:** The token is stored in NVS on the device. It is write-only from the web UI — it is never returned by the API. Use a token with minimal permissions scoped to this repository only.

---



Every push to `main` and every pull request builds a Docker image pushed to the GitHub Container Registry. Run any image locally:

```sh
docker run --rm -p 8080:8080 ghcr.io/<owner>/batterylight:latest      # main branch
docker run --rm -p 8080:8080 ghcr.io/<owner>/batterylight:pr-<N>      # specific PR
```

Auth is disabled when running locally (no environment variables set).

---

## Hardware

See [WIRING.md](WIRING.md) for wiring diagrams for both supported LED types.

| LED type | ESP32-WROOM-32 | ESP32-C3 |
|----------|----------------|----------|
| WS2812B (default) | Data: GPIO 25 | Data: GPIO 20 |
| WS2801 | Data: GPIO 25, Clock: GPIO 26 | Data: GPIO 20, Clock: GPIO 21 |

The LED type is configured per device in the web UI under **Network → LED Type**.
