#pragma once
#include <stdint.h>

#include "../config/Config.h"

// Abstract LED output interface. Swap this implementation when the hardware changes.
class LedDriver {
   public:
    virtual ~LedDriver() = default;
    virtual void begin() = 0;
    // r, g, b: 0-255 linear (driver applies any gamma correction internally)
    virtual void setColor(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void setPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {}
    virtual void show() {}
    virtual void off() { setColor(0, 0, 0); }
    // Changes the RGB→wire permutation at runtime — no re-init needed.
    virtual void setColorOrder(ColorOrder order) {}
};
