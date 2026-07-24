#pragma once
#include <stdint.h>

#include "../config/Config.h"

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
    virtual void show() {}
    virtual void off() { writeColor(0, 0, 0); }
    // Changes the RGB→wire permutation at runtime — no re-init needed.
    virtual void setColorOrder(ColorOrder order) {}

   protected:
    // Hardware-specific output — receives already-clamped values.
    virtual void writeColor(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {}

   private:
    uint8_t _clamp(uint8_t v) const {
        uint16_t scaled = (uint16_t)v * _brightnessScale / 255;
        return scaled < _brightnessLimit ? (uint8_t)scaled : _brightnessLimit;
    }
    uint8_t _brightnessLimit = 255;
    uint8_t _brightnessScale = 255;
};
