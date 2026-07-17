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
| I2C address | 0x18 *(ES8311 default — leave as-is unless audio stays silent, see below)* |
| Board wires a separate MCLK pin | checked |
| I2S MCLK pin | GPIO12 |
| I2S BCLK pin | GPIO13 *(labelled `I2S_SCLK` on the board's silkscreen/pin map — same signal as BCLK)* |
| I2S WS/LRCK pin | GPIO14 |
| I2S DOUT pin | GPIO15 |
| Separate speaker amp enable pin | checked |
| Enable pin source | TCA9555 I2C expander |
| Enable pin | 8 *(EXIO8)* |
| TCA9555 I2C address | 0x20 *(TCA9555 default when A0–A2 are strapped low — not independently confirmed against this board's schematic, see below)* |
| Active high | checked *(assumed — not confirmed against this board's schematic, see below)* |

GPIO16 (`I2S_DIN`) is the codec's microphone input — not used, this firmware
doesn't support mic input yet.

## Unconfirmed details / first bring-up checklist

The table above is derived from the board's published pin map, but two
details weren't verifiable without the board's schematic or physical
hardware in hand — check these if **Test speaker** stays silent:

1. **TCA9555 I2C address (0x20 assumed).** If wrong, the PA never turns on.
   Confirm with an I2C bus scan, or try the other common strapping (0x20–0x27
   depending on the A0–A2 pins).
2. **PA_EN polarity ("Active high" assumed checked).** If the amp is
   permanently on/off regardless of state, or only works with the checkbox
   the *other* way, flip it.

The ES8311 register bring-up sequence itself also carries a general accuracy
caveat — see `src/sound/README.md`.
