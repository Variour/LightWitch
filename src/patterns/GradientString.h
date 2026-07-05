#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"

// Renders a color gradient across a string light. Stop colors are sourced
// from a scene's palette (first-seen distinct colors in the scene's first
// frame) rather than being picked directly.
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop instead of running linearly from end to end.
// With morphEnabled, each gradient stop continuously wanders to a freshly
// chosen random palette color, and the whole ramp is resampled from the
// stops' live colors every tick, so the gradient updates in real time.
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

        const std::vector<Color>* colors = &_base;
        if (_cfg.morphEnabled && _stops.size() > 1) {
            _morph.tick(now, _stops, _palette, _cfg.speed, _liveStops);
            _resample(_liveStops, _out);
            colors = &_out;
        }

        for (uint16_t i = 0; i < _numLeds; i++) {
            const Color& c = (*colors)[i];
            _led->setPixel(i, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
        }
        _led->show();
    }

private:
    uint16_t            _numLeds = 1;
    bool                 _wrap    = false;
    std::vector<Color>   _palette;   // full distinct-color list from the scene
    std::vector<Color>   _stops;     // reduced set actually used as gradient stops
    std::vector<float>   _positions; // jittered physical position of each stop
    std::vector<Color>   _liveStops; // _stops after live morph interpolation
    std::vector<Color>   _base;
    std::vector<Color>   _out;
    GradientCommon::StopMorph _morph;

    void _computeBase() {
        GradientCommon::reduceToStops(_palette, _numLeds, _stops, _cfg.gradientStopCount);
        GradientCommon::computeStopPositions(_stops, (float)_numLeds, _wrap, _positions);
        _resample(_stops, _base);
    }

    void _resample(const std::vector<Color>& stops, std::vector<Color>& out) {
        out.resize(_numLeds);
        for (uint16_t i = 0; i < _numLeds; i++)
            out[i] = GradientCommon::sample(stops, _positions, (float)i, (float)_numLeds, _wrap);
    }
};
