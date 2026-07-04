#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"

// Renders a horizontal color gradient across a matrix light — the same
// left-to-right ramp on every row. Stop colors are sourced from a scene's
// palette (first-seen distinct colors in the scene's first frame).
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop at the end of each row.
// With morphEnabled, a sparse, ever-changing subset of individual LEDs
// shimmer to another palette color and back while the rest hold their base
// gradient color, so the overall ramp stays recognizable.
class GradientMatrix : public Pattern {
public:
    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _width = w; _height = h; }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir) { _matrixStart = start; _matrixDir = dir; }

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
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        _cfg = cfg;
        if (sceneChanged) {
            GradientCommon::loadPalette(cfg.sceneId, _palette);
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
        uint32_t total = (uint32_t)_width * _height;
        if (_base.size() != total) _computeBase();

        const std::vector<Color>* colors = &_base;
        if (_cfg.morphEnabled) {
            _morph.tick(now, _base, _stops, _cfg.speed, _out);
            colors = &_out;
        }

        for (uint16_t row = 0; row < _height; row++) {
            for (uint16_t col = 0; col < _width; col++) {
                const Color& c = (*colors)[row * _width + col];
                uint16_t idx = _ledIndex(row, col);
                _led->setPixel(idx, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
            }
        }
        _led->show();
    }

private:
    uint16_t        _width       = 1, _height = 1;
    MatrixStart     _matrixStart = MatrixStart::TopLeft;
    MatrixDirection _matrixDir   = MatrixDirection::Horizontal;
    bool                 _wrap    = false;
    std::vector<Color>   _palette;  // full distinct-color list from the scene
    std::vector<Color>   _stops;    // reduced set actually used as gradient stops
    std::vector<Color>   _base;
    std::vector<Color>   _out;
    GradientCommon::Morph _morph;

    void _computeBase() {
        GradientCommon::reduceToStops(_palette, _width, _stops);
        uint32_t total = (uint32_t)_width * _height;
        _base.resize(total);
        for (uint16_t row = 0; row < _height; row++)
            for (uint16_t col = 0; col < _width; col++)
                _base[row * _width + col] = GradientCommon::sample(_stops, (float)col, (float)_width, _wrap);
    }

    uint16_t _ledIndex(uint16_t row, uint16_t col) const {
        uint16_t r = (_matrixStart == MatrixStart::BottomLeft || _matrixStart == MatrixStart::BottomRight)
                     ? (_height - 1 - row) : row;
        uint16_t c = (_matrixStart == MatrixStart::TopRight  || _matrixStart == MatrixStart::BottomRight)
                     ? (_width - 1 - col) : col;
        return (_matrixDir == MatrixDirection::Vertical)
               ? c * _height + r
               : r * _width + c;
    }
};
