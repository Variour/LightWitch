#pragma once
#include <Arduino.h>

#include "Pattern.h"

class CandlePattern : public Pattern {
   public:
    // No meaningful period -- candle is inherently random, not phase-syncable.
    float getPeriod() const override { return 0.0f; }

    void tick(uint32_t now) override {
        if (now - _lastUpdate < _interval) return;
        _lastUpdate = now;

        float rate = (_cfg.speed > 0.1f) ? _cfg.speed : 0.1f;
        _interval = 30u + (uint32_t)random(0L, (long)(120.0f / rate) + 1L);

        float flicker;
        if (random(0L, 10L) == 0)
            flicker = 0.30f + (float)random(0L, 200L) / 1000.0f;  // rare dip: 0.30-0.50
        else
            flicker = 0.65f + (float)random(0L, 350L) / 1000.0f;  // normal: 0.65-1.00

        // Warm candle tones: red near full, green shifts, almost no blue
        uint8_t rv = applyBrightness(255u);
        uint8_t gv = applyBrightness((uint8_t)(55u + (uint8_t)random(0L, 60L)));
        uint8_t bv = applyBrightness((uint8_t)random(0L, 8L));

        _led->setColor((uint8_t)((float)rv * flicker), (uint8_t)((float)gv * flicker),
                       (uint8_t)((float)bv * flicker));
    }

   private:
    uint32_t _lastUpdate = 0;
    uint32_t _interval = 50;
};
