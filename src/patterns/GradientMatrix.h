#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"

// Renders a horizontal color gradient across a matrix light — the same
// left-to-right ramp on every row. Stop colors are sourced from a scene's
// palette (first-seen distinct colors in the scene's first frame).
// When the light's wrapWidth topology is set, or scrolling is enabled, the
// gradient loops seamlessly back to its first stop at the end of each row.
class GradientMatrix : public Pattern {
public:
    float getPeriod() const override { return 8000.0f / (_cfg.speed > 0 ? _cfg.speed : 1.0f); }

    void setDimensions(uint16_t w, uint16_t h) { _width = w; _height = h; }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir) { _matrixStart = start; _matrixDir = dir; }
    void setWrap(bool wrap) { _wrap = wrap; }

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
        float offset   = _cfg.scrollEnabled ? _computePhase(now) * (float)_width : 0.0f;
        for (uint16_t row = 0; row < _height; row++) {
            for (uint16_t col = 0; col < _width; col++) {
                Color c = GradientCommon::sample(_palette, (float)col + offset, (float)_width, circular);
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
    bool                _wrap    = false;
    std::vector<Color>  _palette;

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
