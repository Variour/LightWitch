#pragma once
#include "Pattern.h"

// Full period = on-half + off-half. speed=1 → 100 ms/cycle (~10 Hz).
class Strobe : public Pattern {
   public:
    float getPeriod() const override { return 100.0f / _cfg.speed; }

    void tick(uint32_t now) override {
        bool on = _computePhase(now) < 0.5f;
        if (on) {
            _led->setColor(applyBrightness(_cfg.color.r), applyBrightness(_cfg.color.g),
                           applyBrightness(_cfg.color.b));
        } else {
            _led->off();
        }
    }
};
