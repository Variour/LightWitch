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

The register bring-up sequence was originally cross-referenced against
`pschatzmann/arduino-audio-driver`'s `ES8311` class
(`src/Codecs/es8311/ES8311.h`) and other secondary sources (Espressif's
`es8311_coeff_div[]` table, the Linux kernel `es8311` ALSA driver) for the
*facts* they encode — not their code, which wasn't copied; see above for why
that class isn't used directly. That process found and fixed a real gap
(several system/analog power-up register writes — 0x0B, 0x0C, 0x10, 0x11,
0x1B, 0x1C — were missing entirely), but it also produced several **wrong**
fixes: web-fetched summaries of source code proved unreliable for bit-exact
register facts mid-investigation (contradictory re-fetches of the same
file), and three separate "corrections" — `REG_RESET`'s final value,
`REG_CLK_MANAGER1`'s MCLK-source polarity, and `REG_BCLK_DIV` — turned out
to be backwards relative to what actually works on real hardware.

The sequence now in `_resetAndConfigureClocks()`/`_configureFormatAndPower()`
was instead captured directly off the I2C bus: `pschatzmann/arduino-audio-driver`
was temporarily wired up in a standalone throwaway PlatformIO project (same
board, same pins, same I2C address), confirmed to produce a clean tone, and
its actual register writes logged via its own I2C-bus debug tracing. That
capture is ground truth from working hardware, not another citation, and is
what this file's values now match — including two registers (0x16, 0x0A)
this driver hadn't written at all before, since ADC-related registers
seemed irrelevant to a DAC-only codec but the working capture set them
regardless.

If a different sample rate/bit depth is ever added, re-derive
REG_CLK_MANAGER2/REG_ADC_OSR/REG_DAC_OSR/REG_ADC_DAC_DIV/REG_BCLK_DIV/
REG_LRCK_DIV_HI/REG_LRCK_DIV_LO for the new MCLK/rate pair — ideally the same
way (capture a known-working reference's actual register writes) rather than
computing them from a coefficient table read secondhand.
