#pragma once
#include <Arduino.h>
#include <FS.h>

#include <functional>

#include "../logging/Logger.h"

#if defined(SD_CARD_CLK_PIN) && defined(SD_CARD_CMD_PIN) && defined(SD_CARD_D0_PIN)
#include <SD_MMC.h>
#define BL_SD_CARD_HW 1
#else
#define BL_SD_CARD_HW 0
#endif

// Wraps the onboard microSD reader on the Waveshare ESP32-S3-AUDIO-Board (see
// docs/boards/waveshare-esp32-s3-audio.md). Unlike lights/sound/buttons, this
// is a fixed onboard peripheral wired to fixed pins — there's nothing for a
// user to configure, so it's auto-probed at boot instead of added via
// Settings (see SD_CARD_CLK_PIN/SD_CARD_CMD_PIN/SD_CARD_D0_PIN in
// platformio.ini's env:esp32s3, and src/storage/README.md for where those
// pin numbers came from).
//
// <SD_MMC.h> itself is only meaningful on chips with an SDMMC host peripheral
// (it's guarded upstream by SOC_SDMMC_HOST_SUPPORTED and compiles to nothing
// on e.g. ESP32-C3) — every method below is guarded the same way as begin()
// so this header stays includable, and every method a harmless no-op, on
// builds that don't define the SD_CARD_*_PIN macros.
//
// The board also exposes a card-detect line (SD_CD_PIN) on TCA9555 EXIO3,
// but its polarity isn't documented and this class deliberately doesn't use
// it — SD_MMC.begin() failing is just as reliable a "no card" signal and
// doesn't risk misreading an unverified pin.
//
// Storage only for now: mount, list, upload, delete. Playing a file back
// through the speaker is a separate, later step (mirrors how Es8311Driver
// split hardware bring-up from an actual playback pipeline — see
// SoundDriver.h).
class SdCardManager {
   public:
    static constexpr bool kHwSupported = BL_SD_CARD_HW;

    // Attempts to mount the card. Safe to call unconditionally: a no-op
    // returning false when kHwSupported is false, and a quick, harmless
    // failure (not a crash/hang) when the pins are right but no card is
    // inserted.
    bool begin() {
#if BL_SD_CARD_HW
        SD_MMC.setPins(SD_CARD_CLK_PIN, SD_CARD_CMD_PIN, SD_CARD_D0_PIN);
        _mounted = SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format_if_mount_failed=*/false);
        if (_mounted) {
            Logger::i("[sd] card mounted: %llu MB total, %llu MB used",
                      (unsigned long long)(SD_MMC.totalBytes() / (1024 * 1024)),
                      (unsigned long long)(SD_MMC.usedBytes() / (1024 * 1024)));
        } else {
            Logger::i("[sd] no card detected on CLK=%d CMD=%d D0=%d", SD_CARD_CLK_PIN,
                      SD_CARD_CMD_PIN, SD_CARD_D0_PIN);
        }
        return _mounted;
#else
        return false;
#endif
    }

    bool present() const { return _mounted; }

    uint64_t totalBytes() const {
#if BL_SD_CARD_HW
        return _mounted ? SD_MMC.totalBytes() : 0;
#else
        return 0;
#endif
    }

    uint64_t usedBytes() const {
#if BL_SD_CARD_HW
        return _mounted ? SD_MMC.usedBytes() : 0;
#else
        return 0;
#endif
    }

    // Invokes fn(name, size) for every regular file in the card's root
    // directory. Flat layout only — no subdirectories are created or walked.
    void forEachFile(const std::function<void(const String&, size_t)>& fn) const {
#if BL_SD_CARD_HW
        if (!_mounted) return;
        File root = SD_MMC.open("/");
        if (!root) return;
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            if (f.isDirectory()) continue;
            String name = f.name();
            if (name.startsWith("/")) name.remove(0, 1);
            fn(name, f.size());
        }
#else
        (void)fn;
#endif
    }

    // Opens `name` (a bare filename, no path separators) at the SD root for
    // writing, creating/truncating it. Returns an invalid File on failure or
    // if no card is mounted.
    File openForWrite(const String& name) {
#if BL_SD_CARD_HW
        if (!_mounted) return File();
        return SD_MMC.open("/" + name, FILE_WRITE);
#else
        (void)name;
        return File();
#endif
    }

    // Opens `name` (a bare filename, no path separators) at the SD root for
    // reading. Returns an invalid File on failure or if no card is mounted.
    File openForRead(const String& name) {
#if BL_SD_CARD_HW
        if (!_mounted) return File();
        return SD_MMC.open("/" + name, FILE_READ);
#else
        (void)name;
        return File();
#endif
    }

    bool deleteFile(const String& name) {
#if BL_SD_CARD_HW
        if (!_mounted) return false;
        return SD_MMC.remove("/" + name);
#else
        (void)name;
        return false;
#endif
    }

   private:
    bool _mounted = false;
};

#undef BL_SD_CARD_HW
