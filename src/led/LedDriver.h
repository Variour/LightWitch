#pragma once
#include <stdint.h>

#include "../config/Config.h"
#include "Dither.h"

// Abstract LED output interface. Swap this implementation when the hardware changes.
//
// setColor/setPixel are the single write boundary for every render path
// (patterns, tests, future overrides/overlays) — they apply the per-light
// hardware brightness clamp and hand the result to the hardware-specific
// writeColor/writePixel, so no caller can bypass the clamp.
class LedDriver {
   public:
    virtual ~LedDriver() = default;
    virtual void begin() = 0;
    // Per-light hardware clamp (LightHardwareConfig::brightnessLimit/
    // brightnessScale): scale proportionally damps (255 = ×1.0), limit
    // hard-caps the result. Takes effect on the next write — no re-init needed.
    void setBrightnessClamp(uint8_t limit, uint8_t scale) {
        _brightnessLimit = limit;
        _brightnessScale = scale;
    }
    // r, g, b: 0-255 linear (driver applies any gamma correction internally)
    void setColor(uint8_t r, uint8_t g, uint8_t b) { writeColor(_clamp(r), _clamp(g), _clamp(b)); }
    void setPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
        writePixel(idx, _clamp(r), _clamp(g), _clamp(b));
    }
    // One colour on every pixel with 8 fractional bits per channel
    // (0-65280 for 0-255), dithered by pixel position (Dither.h), then shown.
    void setColorFine(uint16_t r, uint16_t g, uint16_t b) {
        r = _clampFine(r);
        g = _clampFine(g);
        b = _clampFine(b);
        for (uint16_t i = 0; i < pixelCount(); i++)
            writePixel(i, dither::channel(r, i), dither::channel(g, i), dither::channel(b, i));
        show();
    }
    virtual uint16_t pixelCount() const { return 1; }
    virtual void show() {}
    virtual void off() { writeColor(0, 0, 0); }
    // Changes the RGB→wire permutation at runtime — no re-init needed.
    virtual void setColorOrder(ColorOrder order) {}
    // Optional total-current cap in mA (LightHardwareConfig::currentLimitMa),
    // 0 = off. Only Ws2812bDriver implements it.
    virtual void setCurrentLimitMa(uint16_t mA) {}
    // Whether the LEDs' supply rail has power (boards whose LED rail can drop,
    // see PowerManager); true unless told otherwise. After false, the LEDs
    // count as dark.
    virtual void setRailPowered(bool powered) {}
    // Send the last frame again every ms while the rail has power (0 = only
    // on change). Only Ws2812bDriver implements it.
    virtual void setRefreshMs(uint16_t ms) {}
    // Called on every render tick (PatternRunner::tick()), so a driver can
    // finish work spread over several frames.
    virtual void service() {}

   protected:
    // Hardware-specific output — receives already-clamped values.
    virtual void writeColor(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {}

   private:
    uint8_t _clamp(uint8_t v) const {
        uint16_t scaled = (uint16_t)v * _brightnessScale / 255;
        return scaled < _brightnessLimit ? (uint8_t)scaled : _brightnessLimit;
    }
    uint16_t _clampFine(uint16_t v) const {
        uint32_t scaled = (uint32_t)v * _brightnessScale / 255;
        uint32_t limit = (uint32_t)_brightnessLimit << 8;
        return (uint16_t)(scaled < limit ? scaled : limit);
    }
    uint8_t _brightnessLimit = 255;
    uint8_t _brightnessScale = 255;
};
