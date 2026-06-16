#pragma once
#include "../config/Config.h"
#include "../led/LedDriver.h"
#include <math.h>

class Pattern {
public:
    virtual ~Pattern() = default;

    virtual void begin(LedDriver& led, const LightConfig& cfg) {
        _led     = &led;
        _startMs = millis();
        applyConfig(cfg);
    }

    virtual void applyConfig(const LightConfig& cfg) { _cfg = cfg; }

    virtual void tick(uint32_t now) = 0;

    // Duration of one full animation cycle in ms (speed=1).
    virtual float getPeriod() const = 0;

    // Current phase [0, 1)
    float getPhase() const { return _computePhase(millis()); }

    // Jump to a specific phase [0, 1) without restarting the animation.
    void snapPhase(float phase) {
        float p = getPeriod();
        _startMs = millis() - (uint32_t)(fmod(phase, 1.0f) * p);
    }

    // Restart from phase 0.
    void resetPhase() { _startMs = millis(); }

protected:
    LedDriver*  _led     = nullptr;
    LightConfig _cfg;
    uint32_t    _startMs = 0;

    float _computePhase(uint32_t now) const {
        float p = getPeriod();
        return p > 0 ? fmod((float)(now - _startMs), p) / p : 0.0f;
    }

    uint8_t applyBrightness(uint8_t v) const {
        return (uint16_t)v * _cfg.brightness / 255;
    }
};
