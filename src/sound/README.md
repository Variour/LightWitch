## Why no external ES8311 library

The codec bring-up in `Es8311Driver.cpp` is an original implementation written
against the publicly documented ES8311 register map (Everest Semiconductor
datasheet — register addresses and their purpose are factual/functional
information, not copyrightable expression), not a vendored copy of any
existing driver. This project has no `LICENSE` file (i.e. all rights
reserved / no explicit permission to redistribute), which rules out the
actively-maintained Arduino-ecosystem ES8311 libraries
([`pschatzmann/arduino-audio-driver`](https://github.com/pschatzmann/arduino-audio-driver)
and its predecessor `arduino-audiokit`) — both are GPL-3.0, which would
require the whole firmware binary to be distributed under GPL-3.0-compatible
terms. Espressif's own `es8311`/`esp_codec_dev` components are Apache-2.0
(no such conflict), but they're ESP-IDF components, not Arduino libraries —
using them here would mean either vendoring their source under an Apache-2.0
attribution/NOTICE, or moving this project's `framework = arduino` build to
a mixed `arduino, espidf` setup, both bigger changes than writing the
(fairly small) I2C bring-up sequence directly.

I2C access uses the Arduino `Wire` library (already a transitive dependency
of this project's Arduino core) and I2S output uses `driver/i2s.h`, which
ships with `arduino-esp32` — no new entry in `platformio.ini`'s `lib_deps`
was needed for this driver specifically.

This is a different call than `src/io/Tca9555Expander` in the sibling `io`
directory, which *does* use an external library — see `src/io/README.md` for
why: no GPL conflict there, and a well-tested, MIT-licensed, PlatformIO-
registry library already existed with no integration friction.

## Accuracy note

Some exact register byte values in the bring-up sequence (clock dividers,
power-up ordering) are a best-effort synthesis of the public register map and
should be re-checked against the datasheet / an oscilloscope on first real
hardware bring-up — the register addresses and overall sequence structure are
solid, but this hasn't been validated against physical ES8311 silicon yet.
