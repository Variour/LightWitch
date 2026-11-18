#pragma once
#include "Pattern.h"

// The breath runs at a fine level (8 fractional bits) and the driver dithers
// it by pixel position (led/Dither.h): at low brightness the pixels reach
// each step one after another instead of all at once, and the colour keeps
// its hue (Timo, 2026-11-10).
class Breathing : public Pattern {
   public:
    float getPeriod() const override { return 4000.0f / _cfg.speed; }

    void tick(uint32_t now) override {
        float phase = _computePhase(now);
        float sine = (sinf(phase * 2.0f * M_PI - M_PI_2) + 1.0f) * 0.5f;
        uint16_t lvl = (uint16_t)(sine * _cfg.brightness * 256.0f);
        _led->setColorFine(dither::scale(_cfg.color.r, lvl), dither::scale(_cfg.color.g, lvl),
                           dither::scale(_cfg.color.b, lvl));
    }
};
