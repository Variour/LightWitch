## SD card: core `SD_MMC`, board-fixed pins

`SdCardManager` wraps `SD_MMC`, the microSD driver bundled with `arduino-esp32`
(same tier as `LittleFS`/`WiFi` — no `platformio.ini` `lib_deps` entry needed,
unlike `src/io/Tca9555Expander` or `src/sound/Es8311Driver`).

The Waveshare ESP32-S3-AUDIO-Board's TF card slot is wired in SDMMC 1-bit
mode to GPIO40 (CLK), GPIO42 (CMD), GPIO41 (DATA/D0) — confirmed against the
board's own schematic (`ESP32-S3-AUDIO-Board_1.1.pdf`, TF Card block), which
also matches the pin numbers a third-party pin-mapping project
([jensenbox/waveshare-esp32-s3-audio](https://github.com/jensenbox/waveshare-esp32-s3-audio))
had already published — cross-checked before the schematic was in hand
because that project's other facts (TCA9555 @0x20, ES8311 @0x18, PA-enable on
EXIO8, LED on GPIO38) all matched what `docs/boards/waveshare-esp32-s3-audio.md`
already documented from real hardware.

These pins live in `platformio.ini`'s `env:esp32s3` `build_flags`
(`SD_CARD_CLK_PIN`/`SD_CARD_CMD_PIN`/`SD_CARD_D0_PIN`), the same convention as
`BATTERY_ADC_PIN` — `SdCardManager::kHwSupported` is `true` only when they're
defined. Unlike lights/sound/buttons, there's no per-device Settings entry
for this: the reader is a fixed onboard peripheral, not something wired up
differently per user, so `main.cpp` just probes it unconditionally at boot
(`SD_MMC.begin()` failing fast and harmlessly on boards without a card, or
without this peripheral at all, is enough of a "not present" signal).

The schematic also shows a card-detect line, `SD_CD_PIN`, on TCA9555 EXIO3.
`SdCardManager` doesn't use it — the schematic doesn't state its resting
polarity, and getting that wrong would be worse than not using it at all
(`SD_MMC.begin()`'s own success/failure is already a reliable, zero-guesswork
presence signal). Revisit if hot-swap detection is ever needed.

Upstream `<SD_MMC.h>` itself is guarded by `SOC_SDMMC_HOST_SUPPORTED` and
compiles to nothing on chips without an SDMMC host peripheral (ESP32-C3,
which this project also targets, is one of them) — so `SdCardManager.h` only
`#include`s it, and only calls into the `SD_MMC` global, inside the same
`SD_CARD_CLK_PIN`-guarded branches as `kHwSupported`. Every method still has
to exist unconditionally (`WebServer.h` calls them on all targets), so the
unsupported branch is a plain no-op/empty return rather than the method being
missing entirely.

## Scope: storage only, WAV only, flat layout

This is storage bring-up only, deliberately split from playback the same way
`src/sound/Es8311Driver` split codec bring-up from an actual playback
pipeline (see `src/sound/SoundDriver.h`) — decoding a file and streaming it
to the speaker is a separate, later step.

Uploads are restricted to `.wav` (validated in `WebServer.h` and mirrored in
`server/index.js`) since that's the only format a future playback pipeline is
currently expected to support without pulling in a decoder library — see
`src/sound/README.md` for why this project is careful about that kind of
dependency. Files sit flat in the card's root directory; no subfolder
convention has been introduced since nothing yet needs one.
