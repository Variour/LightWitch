## Why no external ES8311 library

The codec bring-up in `Es8311Driver.cpp` is an original implementation
written against the publicly documented ES8311 register map (register
addresses and their purpose are factual/functional information, not
copyrightable expression). This was a purely technical call, not a license
one — this repo is currently private, so licensing wasn't a constraint
either way (see PR #366 for the project's forthcoming `LICENSE`/
`THIRD_PARTY_LICENSES.md` once this repo is public): the most complete
public option,
[`pschatzmann/arduino-audio-driver`](https://github.com/pschatzmann/arduino-audio-driver)'s
`ES8311` class, isn't self-contained — it's built against that framework's
own `i2c_bus_handle_t`/`codec_config_t` abstractions (`Platforms/API_I2C.h`,
`DriverCommon.h`), not plain Arduino `Wire`. Using it here would mean either
implementing that I2C-bridge abstraction ourselves or pulling in a
meaningful slice of their broader audio framework, disproportionate for
bringing up one chip in DAC-only mode. Espressif's own `es8311`/`esp_codec_dev`
have the same shape of problem: they're ESP-IDF components (their own
`i2c_bus`/config-struct conventions), not a `framework = arduino` +
`lib_deps` drop-in.

I2C access uses the Arduino `Wire` library (already a transitive dependency
of this project's Arduino core) and I2S output uses `driver/i2s.h`, which
ships with `arduino-esp32` — no new entry in `platformio.ini`'s `lib_deps`
was needed for this driver.

This is a different call than `src/io/Tca9555Expander` in the sibling `io`
directory, which *does* use an external library — see `src/io/README.md`:
that library's API is a plain `Wire`-based single-purpose class with no
surrounding framework to pull in, so there was no integration friction.

## Accuracy note

The register bring-up sequence was cross-referenced against
`pschatzmann/arduino-audio-driver`'s `ES8311` class
(`src/Codecs/es8311/ES8311.h`) for the *facts* it encodes (register
addresses and byte values — not its code, which wasn't copied; see above for
why that class isn't used directly). That check found and fixed a real gap:
this driver was missing several system/analog power-up register writes
(0x0B, 0x0C, 0x10, 0x11, 0x1B, 0x1C) entirely. The clock-divider math
(REG02/REG05/REG06/REG07/REG08, which vary by sample rate and MCLK
frequency) is comparatively less certain — the reference implementation
derives these from a lookup table with bit-packed encodings this driver
didn't attempt to reverse-engineer with full confidence, so those specific
values are still a best-effort synthesis for this driver's fixed 16 kHz/
16-bit target. Re-check against the datasheet / an oscilloscope /
`pschatzmann/arduino-audio-driver`'s `es8311_coeff_div[]` table on first
real hardware bring-up if audio comes out garbled or at the wrong pitch.
