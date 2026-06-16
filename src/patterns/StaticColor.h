#pragma once
#include "Pattern.h"

class StaticColor : public Pattern {
public:
    float getPeriod() const override { return 1000.0f; }  // arbitrary, static has no cycle

    void applyConfig(const LightConfig& cfg) override {
        _cfg = cfg;
        if (_led) {
            _led->setColor(applyBrightness(cfg.color.r),
                           applyBrightness(cfg.color.g),
                           applyBrightness(cfg.color.b));
        }
    }

    void tick(uint32_t /*now*/) override {}
};
