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

Run the web interface locally without hardware using the Cloudflare Pages dev server:

```bash
npm install
npm run dev
```

Open **http://localhost:8788** in a browser. The mock API (under `functions/`) handles all REST endpoints and persists scenes to a local KV store (`.wrangler/state/`). WebSocket is skipped on localhost by the UI.

> Requires Node.js. Wrangler is installed automatically via `npm install`.

---

## Hosted web UI (Cloudflare Pages)

The web UI is hosted on Cloudflare Pages with per-PR preview deployments. Access is restricted via Cloudflare Access — no credentials are stored in the app.

### One-time setup

**1. Create a KV namespace**

In the [Cloudflare dashboard](https://dash.cloudflare.com/) → Workers & Pages → KV → Create namespace. Name it anything (e.g. `batterylight-scenes`).

**2. Create the Pages project**

Workers & Pages → Create → Pages → Upload assets. Upload any placeholder file — the real deployments come from GitHub Actions. Then go to **Settings → Functions → KV namespace bindings** and add:

| Variable name | KV namespace |
|---|---|
| `SCENES` | *(the namespace created in step 1)* |

**3. Add GitHub Actions secrets**

In the GitHub repo → Settings → Secrets → Actions, add:

| Secret | Where to get it |
|---|---|
| `CLOUDFLARE_API_TOKEN` | Cloudflare dashboard → My Profile → API Tokens → Create Token (use the "Edit Cloudflare Workers" template) |
| `CLOUDFLARE_ACCOUNT_ID` | Cloudflare dashboard → right sidebar on any Workers & Pages page |

**4. Enable Cloudflare Access**

Zero Trust → Access → Applications → Add an application → Select "Cloudflare Pages" and pick the Pages project.

Add a policy that allows the relevant GitHub users/organisation. Cloudflare handles all authentication — the app has no auth code.

### Per-PR previews

Every pull request is automatically deployed by the `preview.yml` GitHub Actions workflow. The preview URL is posted as a comment on the PR and updated on each push. The URL is protected by the same Cloudflare Access policy.

---

## Hardware

See [WIRING.md](WIRING.md) for wiring diagrams for both supported LED types.

| LED type | ESP32-WROOM-32 | ESP32-C3 |
|----------|----------------|----------|
| WS2812B (default) | Data: GPIO 25 | Data: GPIO 20 |
| WS2801 | Data: GPIO 25, Clock: GPIO 26 | Data: GPIO 20, Clock: GPIO 21 |

The LED type is configured per device in the web UI under **Network → LED Type**.
