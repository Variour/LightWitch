# Waveshare ESP32-S3-AUDIO-Board — Sound configuration

Per-board field values for **Settings → Sound → Add sound**, for this board's
onboard ES8311 mono codec + speaker. Build/flash with the `esp32s3`
PlatformIO environment.

The board's speaker-amp enable line (PA_EN) isn't wired to a native GPIO —
it sits on **EXIO8** of the onboard TCA9555 I2C GPIO expander (the same
expander also drives the LCD reset/touch-reset/interrupt, TF-card detect,
and camera power/reset lines — EXIO8 is dedicated to PA_EN and doesn't
conflict with those). See `src/io/Tca9555Expander.h` / `IoExpanderChip` in
`src/config/Config.h` for how that's represented.

## Field values

| Field | Value |
|---|---|
| Chip | ES8311 |
| I2C SDA pin | GPIO11 |
| I2C SCL pin | GPIO10 |
| I2C address | 0x18 *(ES8311 default)* |
| Board wires a separate MCLK pin | checked |
| I2S MCLK pin | GPIO12 |
| I2S BCLK pin | GPIO13 *(labelled `I2S_SCLK` on the board's silkscreen/pin map — same signal as BCLK)* |
| I2S WS/LRCK pin | GPIO14 |
| I2S DOUT pin | GPIO16 |
| Separate speaker amp enable pin | checked |
| Enable pin source | TCA9555 I2C expander |
| Enable pin | 8 *(EXIO8)* |
| TCA9555 I2C address | 0x20 *(TCA9555 default when A0–A2 are strapped low)* |
| Active high | checked |

GPIO15 (`I2S_DSIN`) is the codec's microphone input — not used, this firmware
doesn't support mic input yet.
