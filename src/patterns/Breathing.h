#pragma once
#include "Pattern.h"

class Breathing : public Pattern {
   public:
    float getPeriod() const override { return 4000.0f / _cfg.speed; }

    void tick(uint32_t now) override {
        float phase = _computePhase(now);
        float sine = (sinf(phase * 2.0f * M_PI - M_PI_2) + 1.0f) * 0.5f;
        uint8_t scale = (uint8_t)(sine * _cfg.brightness);
        _led->setColor((uint16_t)_cfg.color.r * scale / 255, (uint16_t)_cfg.color.g * scale / 255,
                       (uint16_t)_cfg.color.b * scale / 255);
    }
};
