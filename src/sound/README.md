## Why no external ES8311 library

The codec bring-up in `Es8311Driver.cpp` is an original implementation written
against the publicly documented ES8311 register map (Everest Semiconductor
datasheet — register addresses and their purpose are factual/functional
information, not copyrightable expression), not a vendored copy of any
existing driver (e.g. Espressif's `es8311` component, `esp-adf`, or a
community Arduino library). I2C access uses the Arduino `Wire` library
(already a transitive dependency of this project's Arduino core) and I2S
output uses `driver/i2s.h`, which ships with `arduino-esp32` — no new entry
in `platformio.ini`'s `lib_deps` was needed, so there's no third-party
license to track for this feature.

If a second sound chip is added later and an existing library turns out to be
the better choice for it, check and document that library's license the same
way `Adafruit_WS2801`/`Adafruit_NeoPixel` are tracked for the LED drivers.

## Accuracy note

Some exact register byte values in the bring-up sequence (clock dividers,
power-up ordering) are a best-effort synthesis of the public register map and
should be re-checked against the datasheet / an oscilloscope on first real
hardware bring-up — the register addresses and overall sequence structure are
solid, but this hasn't been validated against physical ES8311 silicon yet.
