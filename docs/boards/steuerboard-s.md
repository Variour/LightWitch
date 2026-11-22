# Steuerboard S

Round 1S control board for small lamps (Teelicht, Öllampe) and the glow
balls (Wurfball, Magische Kugel): ESP32-C3-WROOM-02, TP4056 charger, DW01A
cell protection, SY7088 5 V boost, two switched LED outputs, optional
LIS2DH12 position sensor. Hardware design: the `smart-laternenfackel`
project. The firmware support lives in `src/board/` and in the
`BOARD_STEUERBOARD_S` branch of `src/config/BoardProfile.cpp`; it is part of
the `esp32c3` image.

## Pin map

From S schematic draft 1. Change these together with the schematic, all of
them sit in `src/board/SteuerboardS.h`.

| GPIO | Signal | Notes |
|---|---|---|
| 0 | LIS2DH12 INT1 | active low, latched; deep-sleep wake. Sensor at 0x18 |
| 1 | VBAT / 2 | 1M / 1M divider with 100 nF, ADC1 |
| 2 | I2C SDA | strap pin, held high by the pull-up |
| 3 | SW1 | to GND, 10k pull-up; deep-sleep wake |
| 4 | TP4056 CHRG | open drain, 100k pull-up, low while charging; deep-sleep wake. The Qi board's BQ51050B CHG (open drain) joins this net |
| 5 | TP4056 STDBY | open drain, 100k pull-up, low when full |
| 6 | LED output A 5 V switch | high = on, 100k pulldown |
| 7 | LED output B 5 V switch | high = on, 100k pulldown |
| 8 | I2C SCL | strap pin, held high by the pull-up |
| 9 | BYPASS | high = cell straight onto the LED rail, only while BOOST_EN is low; BOOT pad |
| 10 | LED data A | 10k pulldown |
| 20 | BOOST_EN | SY7088 EN, high = boost on (4.84 V), 1M pulldown |
| 21 | LED data B | 10k pulldown |

## First boot

On a device without a saved config, the `esp32c3` image checks for an S
(I2C pull-ups on GPIO2/GPIO8 and a cell on the VBAT divider) and seeds:

- `board` = 1 (Steuerboard S), kept in the config from then on,
- I2C bus GPIO2/GPIO8, battery reporting on,
- light A: WS2812B, colour order RGB (a lamp file carries the wire order),
  data GPIO10, 9 pixels (Teelicht: 3 Kerze triangles),
- SW1 on GPIO3 with role `power`,
- group 0: pattern Candle at brightness 100, which is 12.7 lm/dm² on the lamp
  scale, about 1.7 lm on the Teelicht.

A factory reset seeds again.

## Behaviour

**LED outputs.** Each output's 5 V switch is on only while its frame lights
an LED. After switching on, the ICs get 10 ms before the first frame; after
2 s dark the data line stops and the 5 V switch opens.

**LED rail source.** While an output is on, the rail runs from the boost, or
straight from the cell (bypass) above 3.75 V, back to the boost below 3.65 V.
BOOST_EN always goes low before BYPASS goes high. With every output off,
both are off.

**Battery and charging.** The battery reading comes from the VBAT divider,
"charging" means a charger feeds the board: CHRG low (charging, also Qi) or
STDBY low (full). Below 3.5 V on battery the light blinks dark three times,
repeated every 10 minutes. Below 3.3 V for 30 s the device goes into deep
sleep and wakes only when a charger pulls CHRG low; at boot the same check
runs before Wi-Fi.

**Off.** SW1 held for 2 s switches the device off: LEDs off, deep sleep
until SW1, a charger, or (with `motionOffS` set and a sensor fitted) motion;
then it boots afresh.

**Motion (glow balls).** `motionOffS` (`/api/config`, 0 = off, up to 3600 s):
on battery, the device switches off after that many seconds without motion
and motion wakes it again. The sensor runs at 10 Hz in low-power mode with an
80 mg high-pass wake threshold.

**Position.** With a lamp file loaded, the gravity direction (low-passed,
updated when it turns by more than ~3°) becomes the lamp's "up", so Fire
burns upward however the ball lies. Sensor axes are taken as lamp axes; check
that on the first ball.

Nothing switches off while a firmware update runs.
