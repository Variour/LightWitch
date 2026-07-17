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
| `bootloader-esp32s3.bin` | Bootloader for ESP32-S3 devices |
| `partitions.bin` | Partition table (same for all board types) |
| `firmware-esp32c3.bin` | Firmware for ESP32-C3 devices |
| `firmware-esp32dev.bin` | Firmware for ESP32-WROOM-32 devices |
| `firmware-esp32s3.bin` | Firmware for ESP32-S3 devices |
| `littlefs.bin` | Web UI filesystem (same for all board types) |

**2. Open the web flasher**

Go to **https://esp.huhn.me** in Chrome or Edge.

**3. Connect your device**

Plug in the ESP32 via USB, then click **Connect** and select the device's serial port.

**4. Add the files**

Add entries for your board type with these addresses:

**ESP32-C3 / ESP32-S3:**

| Address | File |
|---------|------|
| `0x0` | `bootloader-esp32c3.bin` (C3) / `bootloader-esp32s3.bin` (S3) |
| `0x8000` | `partitions.bin` |
| `0x10000` | `firmware-esp32c3.bin` (C3) / `firmware-esp32s3.bin` (S3) |
| `0x3B0000` | `littlefs.bin` |

**ESP32-WROOM-32:**

| Address | File |
|---------|------|
| `0x1000` | `bootloader-esp32dev.bin` |
| `0x8000` | `partitions.bin` |
| `0x10000` | `firmware-esp32dev.bin` |
| `0x3B0000` | `littlefs.bin` |

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

## Device settings

After first boot, go to **Settings** to review:

| Setting | Why |
|---|---|
| **Device name** | mDNS hostname (`http://<name>.local`) and OTA target name; defaults to a MAC-based name like `light-a1b2c3`. |
| **AP password** | Defaults to `batterylight` (published in this README) — change it if the device's AP could ever be reachable by others. |
| **Timezone** | Only used by the Time-mode pattern; skip it if you don't use that mode. |
| **MQTT** | Optional home-automation integration — leave the broker host blank to disable it. |
| **Firmware updates** | Optional GitHub-releases integration — see [below](#firmware-updates-from-github-releases-device-web-ui). |

Log level, OTA port, and buttons can be left at their defaults unless you have a specific reason to change them.

---

## Adding more devices to the mesh

Devices discover each other automatically over ESP-NOW and form a mesh (scene sync, pattern sync, etc.) — this only requires sharing the same 2.4 GHz WiFi channel, not the same WiFi network or credentials. To set up additional devices without re-entering every field by hand:

1. Flash and boot the new device as described above; it appears in the first device's **Connected Lights** table once discovered.
2. Click **Push config to all** (or the per-device push button next to a specific peer) to send WiFi networks, AP password, MQTT settings, GitHub repo/token, and OTA-enabled to the target(s) in one step.
3. Any field left unchecked in the push dialog is skipped on the target. Sensitive fields (AP password, MQTT password, GitHub token) are filled in by the firmware itself and never appear in the browser.

---

## Configuring your first light

A fresh device starts with one group (**Default**) but no lights configured — add your hardware before anything lights up.

1. Go to **Settings → Lights** and click **Add light**.
2. Choose the **LED type** (WS2812B single-wire or WS2801 two-wire), the **data pin** (and **clock pin** for WS2801), and the light's **length** (LED count, or columns/rows for a matrix).
3. Click **Save & Reboot** — hardware changes require a restart to take effect.
4. Back on the main tab, use the **Group** dropdown in the Connected Lights table to assign the light to a group (new lights start in **Default**).

Repeat for each physical light attached to the device (up to 4).

### Groups

A **group** holds the pattern/scene/color state — lights don't have their own; they display whatever their assigned group is set to. Put multiple lights in the same group to keep them in sync, or in separate groups to control them independently.

**Default** cannot be deleted; deleting any other group moves its lights back to Default.

---

## Configuring sound output

Adding a sound output brings the codec up and lets you play a short built-in test melody to verify wiring.

1. Go to **Settings → Sound** and click **Add sound**.
2. Choose the **chip** (only ES8311, a mono I2S codec, is supported today), then the **I2C pins** (SDA/SCL, plus the I2C address if your board's CE pin isn't strapped to the default), and the **I2S pins** (BCLK, WS/LRCK, DOUT). Leave the MCLK pin unset if your board doesn't wire one — the ES8311 can derive its clock from BCLK internally. If your board gates a separate speaker amp, set the **PA enable pin**: either a direct GPIO, or — on boards where it sits behind a TCA9555 I2C GPIO expander shared with other peripherals instead of a native pin — select **TCA9555 expander** and its I2C address.
3. Click **Save & Reboot** — hardware changes require a restart to take effect.
4. Click **Test speaker** to play a short built-in melody and confirm the wiring works.

One sound output per device. For known board-specific pin values, see [docs/boards/](docs/boards/) (currently: [Waveshare ESP32-S3-AUDIO-Board](docs/boards/waveshare-esp32-s3-audio.md)).

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

Supported LED types:
- WS2812B
- WS2801

LED type and data/clock GPIO pins are configured per light in the web UI.

Supported sound chips:
- ES8311 (mono I2S codec), see [Configuring sound output](#configuring-sound-output)

Sound chip and I2C/I2S GPIO pins are configured in the web UI.

**Battery (optional, ESP32-C3 / ESP32-S3 only):** BAT (battery +) can be sensed through a 200 kΩ / 100 kΩ (±1 %) voltage divider, solder-bridged onto GPIO1. GPIO1 isn't ADC-capable on the classic ESP32 (esp32dev), so this is unavailable there. On the Waveshare ESP32-S3-AUDIO-Board, closing that bridge disables the board's camera header (they share the same pin) — not a concern for this firmware, which never uses the camera. Once wired, enable it under Settings → Battery; battery percentage and charging/mains status then show up in the device list, over the mesh, and via MQTT. There's no readable charge-status signal on this hardware — the charger IC's STAT pin only drives its own indicator LED, not a GPIO — so "charging" is inferred from voltage alone (see `BatteryMonitor.h`).

---

## Known issues

See [docs/known-issues.md](docs/known-issues.md).

---

## Development & hosting

- [docs/development.md](docs/development.md) — building from source, OTA via PlatformIO, running the web UI locally
- [docs/hosting.md](docs/hosting.md) — deploying the web UI to Azure Container Apps
- [docs/mesh-compatibility.md](docs/mesh-compatibility.md) — mesh wire-protocol compatibility policy and message inventory
- [docs/boards/](docs/boards/) — per-board pin values for hardware that needs more than "pick a GPIO" (e.g. sound codecs behind an I2C GPIO expander)
