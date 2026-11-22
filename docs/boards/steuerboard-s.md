# Steuerboard S

Round 1S control board for small lamps (Teelicht, Öllampe) and the glow
balls (Wurfball, Magische Kugel): ESP32-C3-WROOM-02, TP4056 charger, DW01A
cell protection, SY7088 5 V boost, two switched LED outputs, optional
LIS2DH12 position sensor. Hardware design: the `smart-laternenfackel`
project. The firmware support lives in `src/board/` and in the
`BOARD_STEUERBOARD_S` branch of `src/config/BoardProfile.cpp`; it is part of
the `esp32c3` image.

## Pin map (provisional)

The schematic is not drawn yet. Change these together with it, all of them
sit in `src/board/SteuerboardS.h`.

| GPIO | Signal | Notes |
|---|---|---|
| 0 | LIS2DH12 INT1 | active low, latched; wakes the C3 from sleep |
| 1 | VBAT / 2 | 1M / 1M divider with 100 nF, ADC1 |
| 2 | I2C SDA | strap pin, held high by the 4.7k pull-up |
| 3 | TP4056 CHRG | open drain with pull-up, low while charging; deep-sleep wake. The Qi board's BQ51050B CHG (open drain) joins this net |
| 4 | TP4056 STDBY | open drain with pull-up, low when full |
| 5 | LED output A 5 V switch | high = on, gate pulldown |
| 6 | LED output B 5 V switch | high = on, gate pulldown |
| 7 | I2C SCL | |
| 9 | SW1 (BOOT) | active low |
| 10 | LED data A | into the SN74LV1T125 |
| 20 | LED data B | into the SN74LV1T125 |

## First boot

On a device without a saved config, the `esp32c3` image checks for an S
(I2C pull-ups on GPIO2/GPIO7 and a cell on the VBAT divider) and seeds:

- `board` = 1 (Steuerboard S), kept in the config from then on,
- I2C bus GPIO2/GPIO7, battery reporting on,
- light A: WS2812B, colour order RGB (a lamp file carries the wire order),
  data GPIO10, 9 pixels (Teelicht: 3 Kerze triangles),
- SW1 on GPIO9 with role `power`,
- group 0: pattern Candle at brightness 100, which is 12.7 lm/dm² on the lamp
  scale, about 1.7 lm on the Teelicht.

A factory reset seeds again.

## Behaviour

**LED outputs.** Each output's 5 V switch is on only while its frame lights
an LED. After switching on, the ICs get 10 ms before the first frame; after
2 s dark the data line stops and the 5 V switch opens.

**Battery and charging.** The battery reading comes from the VBAT divider,
"charging" means a charger feeds the board: CHRG low (charging, also Qi) or
STDBY low (full). Below 3.5 V on battery the light blinks dark three times,
repeated every 10 minutes. Below 3.3 V for 30 s the device goes into deep
sleep and wakes only when a charger pulls CHRG low; at boot the same check
runs before Wi-Fi.

**Off.** SW1 held for 2 s switches the device off: LEDs and radio off, light
sleep until SW1, a charger, or (with `motionOffS` set and a sensor fitted)
motion; then it boots afresh. Light sleep, because GPIO9 cannot wake the C3
from deep sleep.

**Motion (glow balls).** `motionOffS` (`/api/config`, 0 = off, up to 3600 s):
on battery, the device switches off after that many seconds without motion
and motion wakes it again. The sensor runs at 10 Hz in low-power mode with an
80 mg high-pass wake threshold.

**Position.** With a lamp file loaded, the gravity direction (low-passed,
updated when it turns by more than ~3°) becomes the lamp's "up", so Fire
burns upward however the ball lies. Sensor axes are taken as lamp axes; check
that on the first ball.

Nothing switches off while a firmware update runs.
