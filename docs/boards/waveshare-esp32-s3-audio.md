# Waveshare ESP32-S3-AUDIO-Board — Sound & LED configuration

Per-board field values for **Settings → Sound → Add sound** and
**Settings → Lights → Add light**, for this board's onboard ES8311 mono
codec + speaker and its onboard WS2812 RGB LED.

## Sound

The board's speaker-amp enable line (PA_EN) isn't wired to a native GPIO —
it sits on **EXIO8** of the onboard TCA9555 I2C GPIO expander.

### Field values

| Field | Value |
|---|---|
| Chip | ES8311 |
| I2C SDA pin | GPIO11 |
| I2C SCL pin | GPIO10 |
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
- **BOOT** is wired to native **GPIO0** (also the boot-mode strapping pin) —
  the only one of the five that can currently be configured as a button in
  this firmware.
- **KEY1**, **KEY2**, **KEY3** are wired to **EXIO9**, **EXIO10**, **EXIO11**
  on the onboard TCA9555 I2C GPIO expander, not to native ESP32 GPIOs.

### Field values (BOOT button only)

| Field | Value |
|---|---|
| GPIO pin | GPIO0 |
| Active low | checked *(button pulls to GND when pressed)* |

### KEY1–KEY3 are not usable yet

This firmware's button configuration only supports a native ESP32 GPIO pin —
unlike Sound's PA-enable pin, there's no I2C-expander source option for
buttons. Since KEY1–KEY3 sit behind the TCA9555 expander, they can't be
wired up through the current "Add button" form; expander support would need
to be added to the button handling code before these three could be used.

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
