# Waveshare ESP32-S3-AUDIO-Board — Sound, Button & LED configuration

Per-board field values for **Settings → Hardware → I2C bus**,
**Settings → Sound → Add sound**, **Settings → Hardware → Add button**, and
**Settings → Lights → Add light**, for this board's onboard ES8311 mono
codec + speaker, its TCA9555-backed buttons, and its onboard WS2812 RGB LED.

## I2C bus

The codec, the TCA9555 I/O expander, and the RTC all share a single I2C bus.
Configure this **before** adding the sound output or any TCA9555-backed
button below — both require it.

### Field values

| Field | Value |
|---|---|
| This board has an I2C bus | checked |
| SDA pin | GPIO11 |
| SCL pin | GPIO10 |

## Sound

The board's speaker-amp enable line (PA_EN) isn't wired to a native GPIO —
it sits on **EXIO8** of the onboard TCA9555 I2C GPIO expander.

### Field values

| Field | Value |
|---|---|
| Chip | ES8311 |
| I2C address | 0x18 *(ES8311 default)* |
| I2S BCLK pin | GPIO13 *(labelled `I2S_SCLK` on the board's silkscreen/pin map — same signal as BCLK)* |
| I2S WS/LRCK pin | GPIO14 |
| I2S DOUT pin | GPIO16 |
| Board wires a separate MCLK pin | checked |
| I2S MCLK pin | GPIO12 |
| Separate speaker amp enable pin | checked |
| Enable pin source | TCA9555 I2C expander |
| Enable pin | 8 *(EXIO8)* |
| TCA9555 I2C address | 0x20 *(TCA9555 default when A0–A2 are strapped low)* |
| Active high | checked |

GPIO15 (`I2S_DSIN`) is the codec's microphone input — not used, this firmware
doesn't support mic input yet.

## Buttons

The board has 5 physical buttons: **RESET**, **BOOT**, and **KEY1**–**KEY3**.

- **RESET** is wired to the ESP32-S3's `CHIP_PU` (EN) pin — a hardware reset
  line, not something firmware can read as a button.
- **BOOT** is wired to native **GPIO0** (also the boot-mode strapping pin).
- **KEY1**, **KEY2**, **KEY3** are wired to **EXIO9**, **EXIO10**, **EXIO11**
  on the onboard TCA9555 I2C GPIO expander (same expander/address as Sound's
  PA-enable pin above), not to native ESP32 GPIOs.

### Field values — BOOT

| Field | Value |
|---|---|
| Pin source | Direct GPIO |
| Pin | 0 *(GPIO0)* |
| Active low | checked *(button pulls to GND when pressed)* |

### Field values — KEY1 / KEY2 / KEY3

Add one button per key; only **Pin** differs between them.

| Field | Value |
|---|---|
| Pin source | TCA9555 I2C expander |
| Pin | 9 for KEY1, 10 for KEY2, 11 for KEY3 *(EXIO9/EXIO10/EXIO11)* |
| TCA9555 I2C address | 0x20 *(TCA9555 default when A0–A2 are strapped low)* |
| Active low | checked *(button pulls to GND when pressed; the board provides its own pull-up — the TCA9555 has no internal pull resistors)* |

## Lights

The board has 7 onboard WS2812 RGB LEDs on the back, chained on a single
data line.

### Field values

| Field | Value |
|---|---|
| LED type | WS2812B — single wire |
| Data pin (GPIO) | GPIO38 |
| Colour order | GRB *(WS2812B default)* |
| Length | 7 |
| Rows | 1 |

## Storage (SD card)

Unlike Sound and Lights above, there's nothing to fill in here — the onboard
TF card slot is a fixed peripheral, not something wired up differently per
device, so the firmware auto-probes it at boot (see `src/storage/`) and it
just shows up under Settings → Storage when a card is inserted.

For reference, it's wired in SDMMC 1-bit mode: CLK on GPIO40, CMD on GPIO42,
DATA/D0 on GPIO41 — confirmed from the board's own schematic
(`ESP32-S3-AUDIO-Board_1.1.pdf`, TF Card block). The schematic also shows a
card-detect line (`SD_CD_PIN`) on the TCA9555's EXIO3, which the firmware
doesn't use (see `src/storage/README.md` for why).
