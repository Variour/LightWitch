#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"

// Renders a color gradient across a string light. Stop colors are sourced
// from a scene's palette (first-seen distinct colors in the scene's first
// frame) rather than being picked directly.
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop instead of running linearly from end to end.
// With morphEnabled, a sparse, ever-changing subset of LEDs shimmer to
// another palette color and back while the rest hold their base gradient
// color, so the overall ramp stays recognizable.
class GradientString : public Pattern {
public:
    float getPeriod() const override { return 0.0f; }

    void setNumLeds(uint16_t n) { _numLeds = n; }

    void setWrap(bool wrap) {
        if (wrap == _wrap) return;
        _wrap = wrap;
        _computeBase();
        _morph.reset();
    }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        GradientCommon::loadPalette(cfg.sceneId, _palette);
        _computeBase();
        _morph.reset();
        tick(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged      = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        bool stopCountChanged  = cfg.gradientStopCount != _cfg.gradientStopCount;
        _cfg = cfg;
        if (sceneChanged) {
            GradientCommon::loadPalette(cfg.sceneId, _palette);
            _computeBase();
            _morph.reset();
        } else if (stopCountChanged) {
            _computeBase();
            _morph.reset();
        }
    }

    // Re-read the scene palette if it matches the currently loaded scene.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        GradientCommon::loadPalette(sceneId, _palette);
        _computeBase();
        _morph.reset();
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        if (_base.size() != _numLeds) _computeBase();

        if (_cfg.morphEnabled) {
            _morph.tick(now, _base, _stops, _cfg.speed, _out);
            for (uint16_t i = 0; i < _numLeds; i++) {
                const Color& c = _out[i];
                _led->setPixel(i, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
            }
        } else {
            for (uint16_t i = 0; i < _numLeds; i++) {
                const Color& c = _base[i];
                _led->setPixel(i, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
            }
        }
        _led->show();
    }

private:
    uint16_t            _numLeds = 1;
    bool                 _wrap    = false;
    std::vector<Color>   _palette;   // full distinct-color list from the scene
    std::vector<Color>   _stops;     // reduced set actually used as gradient stops
    std::vector<float>   _positions; // jittered physical position of each stop
    std::vector<Color>   _base;
    std::vector<Color>   _out;
    GradientCommon::Morph _morph;

    void _computeBase() {
        GradientCommon::reduceToStops(_palette, _numLeds, _stops, _cfg.gradientStopCount);
        GradientCommon::computeStopPositions(_stops, (float)_numLeds, _wrap, _positions);
        _base.resize(_numLeds);
        for (uint16_t i = 0; i < _numLeds; i++)
            _base[i] = GradientCommon::sample(_stops, _positions, (float)i, (float)_numLeds, _wrap);
    }
};
