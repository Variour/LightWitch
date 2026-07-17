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
(0x0B, 0x0C, 0x10, 0x11, 0x1B, 0x1C) entirely.

Two more real bugs were found and fixed on first real hardware bring-up
(silent/noisy output on two separate Waveshare ESP32-S3-AUDIO boards),
cross-checked against Espressif's own `es8311_coeff_div[]` table
(`espressif/esp-bsp`'s `components/es8311/es8311.c`) for this driver's fixed
16 kHz/16-bit target with the ESP32 legacy I2S driver's default
`use_apll=false` 256x MCLK multiple (MCLK = 4.096 MHz for 16 kHz):
- `REG_RESET`'s final value was `0xC0`, putting the *codec* into I2S master
  mode (bit6) while the ESP32 I2S peripheral is also configured as master —
  two masters driving BCLK/LRCK — and re-asserting the reset bit (bit7) that
  nothing afterwards cleared. Fixed to `0x00` (slave mode, reset released).
- `REG_LRCK_DIV_LO` derived `lrck_l` from "BCLK cycles per 16-bit stereo
  frame" (32) instead of DIG_MCLK/LRCK (4096000/16000 = 256, i.e.
  `lrck_h=0x00, lrck_l=0xff` after the table's `-1` register encoding).
  Fixed to `0xff`.
- `REG_BCLK_DIV` copied the coefficient-table's `bclk_div` value (4, reg
  0x03) as-is, but that table assumes a 32-bit-per-channel I2S slot
  (physical BCLK = LRCK*64) — this driver's `i2s_config_t` uses
  `I2S_BITS_PER_SAMPLE_16BIT`/stereo, i.e. BCLK = LRCK*32 = 512kHz, so the
  real divisor is DIG_MCLK/BCLK = 4096000/512000 = 8 (reg 0x07), not the
  table's assumed 4 (reg 0x03). Copying `es8311_coeff_div[]` register values
  directly is only valid if the I2S slot width matches what generated that
  table row — it doesn't here. Fixed to `0x07`.

A fourth bug, cross-checked against the Linux kernel `es8311` ALSA driver
(`sound/soc/codecs/es8311.h`'s `ES8311_CLKMGR1_MCLK_SEL` define — the
clearest source found for this specific bit's polarity): `REG_CLK_MANAGER1`
bit7 is 1 = external MCLK pin, 0 = derive MCLK from BCLK/SCLK. This driver's
`mclkFromSclk ? 0xBF : 0x3F` had that backwards — a board with a wired MCLK
pin (bit7 should be 1) got `0x3F` (bit7=0, BCLK-derived), so the codec
clocked itself from the wrong source entirely regardless of how correct the
divider registers above were. Fixed to `mclkFromSclk ? 0x3F : 0xBF`.

If a different sample rate/bit depth is ever added, re-derive
REG_BCLK_DIV/REG_LRCK_DIV_HI/REG_LRCK_DIV_LO from `es8311_coeff_div[]` for
the new MCLK/rate pair rather than reusing these fixed values.
