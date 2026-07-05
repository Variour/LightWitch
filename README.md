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

## Hardware

Supported LED types: **WS2812B** (default) and **WS2801**.

LED type and data/clock GPIO pins are configured per light in the web UI.

---

## Known issues

See [docs/known-issues.md](docs/known-issues.md).

---

## Development & hosting

- [docs/development.md](docs/development.md) — building from source, OTA via PlatformIO, running the web UI locally
- [docs/hosting.md](docs/hosting.md) — deploying the web UI to Azure Container Apps
