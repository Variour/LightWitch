#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"
#include "MatrixLayout.h"

// Renders a horizontal color gradient across a matrix light — the same
// left-to-right ramp on every row. Stop colors are sourced from a scene's
// palette (first-seen distinct colors in the scene's first frame).
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop at the end of each row.
// With morphEnabled, each gradient stop continuously wanders to a freshly
// chosen random palette color, and the whole ramp is resampled from the
// stops' live colors every tick — so entire columns (the ones spanning each
// stop) move together in real time, rather than individual LEDs shimmering.
class GradientMatrix : public Pattern {
public:
    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _layout.setDimensions(w, h); }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _layout.setWiring(start, dir, serpentine);
    }

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
        uint32_t total = (uint32_t)_layout.width() * _layout.height();
        if (_base.size() != total) _computeBase();

        const std::vector<Color>* colors = &_base;
        if (_cfg.morphEnabled && _stops.size() > 1) {
            _morph.tick(now, _stops, _palette, _cfg.speed, _liveStops);
            _resample(_liveStops, _out);
            colors = &_out;
        }

        for (uint16_t row = 0; row < _layout.height(); row++) {
            for (uint16_t col = 0; col < _layout.width(); col++) {
                const Color& c = (*colors)[row * _layout.width() + col];
                uint16_t idx = _layout.ledIndex(row, col);
                _led->setPixel(idx, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
            }
        }
        _led->show();
    }

private:
    MatrixLayout         _layout;
    bool                 _wrap    = false;
    std::vector<Color>   _palette;   // full distinct-color list from the scene
    std::vector<Color>   _stops;     // reduced set actually used as gradient stops
    std::vector<float>   _positions; // jittered physical position of each stop
    std::vector<Color>   _liveStops; // _stops after live morph interpolation
    std::vector<Color>   _base;
    std::vector<Color>   _out;
    GradientCommon::StopMorph _morph;

    void _computeBase() {
        GradientCommon::reduceToStops(_palette, _layout.width(), _stops, _cfg.gradientStopCount);
        GradientCommon::computeStopPositions(_stops, (float)_layout.width(), _wrap, _positions);
        _resample(_stops, _base);
    }

    void _resample(const std::vector<Color>& stops, std::vector<Color>& out) {
        uint32_t total = (uint32_t)_layout.width() * _layout.height();
        out.resize(total);
        for (uint16_t row = 0; row < _layout.height(); row++)
            for (uint16_t col = 0; col < _layout.width(); col++)
                out[row * _layout.width() + col] = GradientCommon::sample(stops, _positions, (float)col, (float)_layout.width(), _wrap);
    }
};
