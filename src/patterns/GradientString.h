#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"

// Renders a color gradient across a string light. Stop colors are sourced
// from a scene's palette (first-seen distinct colors in the scene's first
// frame) rather than being picked directly.
// When the light's wrapWidth topology is set, or scrolling is enabled, the
// gradient loops seamlessly back to its first stop instead of running
// linearly from end to end.
class GradientString : public Pattern {
public:
    float getPeriod() const override { return 8000.0f / (_cfg.speed > 0 ? _cfg.speed : 1.0f); }

    void setNumLeds(uint16_t n) { _numLeds = n; }
    void setWrap(bool wrap)     { _wrap = wrap; }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        GradientCommon::loadPalette(cfg.sceneId, _palette);
        _startMs = millis();
        tick(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        _cfg = cfg;
        if (sceneChanged) GradientCommon::loadPalette(cfg.sceneId, _palette);
    }

    // Re-read the scene palette if it matches the currently loaded scene.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        GradientCommon::loadPalette(sceneId, _palette);
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        bool  circular = _wrap || _cfg.scrollEnabled;
        float offset   = _cfg.scrollEnabled ? _computePhase(now) * (float)_numLeds : 0.0f;
        for (uint16_t i = 0; i < _numLeds; i++) {
            Color c = GradientCommon::sample(_palette, (float)i + offset, (float)_numLeds, circular);
            _led->setPixel(i, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
        }
        _led->show();
    }

private:
    uint16_t           _numLeds = 1;
    bool                _wrap    = false;
    std::vector<Color>  _palette;
};
