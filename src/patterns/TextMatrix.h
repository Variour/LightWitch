#pragma once
#include <vector>
#include <math.h>
#include "Pattern.h"
#include "Font5x7.h"
#include "MatrixLayout.h"

// Renders a text message on a matrix light using the fixed 5x7 font.
// Message shorter than the matrix width is centered and held static
// (no scrolling), regardless of the configured animation. A longer
// message either scrolls continuously right-to-left (Scroll) or
// ping-pongs back and forth (Bounce), per cfg.textAnimation, at a rate
// scaled by cfg.speed. The glyph rows are centered vertically within the
// matrix height, cropping evenly top/bottom on matrices shorter than 7
// rows and padding evenly on taller ones, so the same message renders
// sensibly across differently sized matrices.
class TextMatrix : public Pattern {
public:
    float getPeriod() const override {
        if (!_animated) return 0.0f;
        float speed   = _cfg.speed > 0.01f ? _cfg.speed : 0.01f;
        float pxPerSec = kBasePixelsPerSecond * speed;
        if (_cfg.textAnimation == TextAnimation::Bounce) {
            float travel = (float)((int)_textWidthPx - (int)_layout.width());
            return (2.0f * travel / pxPerSec) * 1000.0f;
        }
        float dist = (float)((int)_layout.width() + (int)_textWidthPx);
        return (dist / pxPerSec) * 1000.0f;
    }

    void setDimensions(uint16_t w, uint16_t h) {
        if (w == _layout.width() && h == _layout.height()) return;
        _layout.setDimensions(w, h);
        _recompute();
    }

    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _layout.setWiring(start, dir, serpentine);
    }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _recompute();
        resetPhase();
        tick(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool textChanged = strncmp(cfg.text, _cfg.text, sizeof(cfg.text)) != 0;
        _cfg = cfg;
        if (textChanged) {
            _recompute();
            resetPhase();
        }
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        int xOffset;
        if (!_animated) {
            xOffset = ((int)_layout.width() - (int)_textWidthPx) / 2;
        } else if (_cfg.textAnimation == TextAnimation::Bounce) {
            float phase = _computePhase(now);
            float t = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;  // 0→1→0
            xOffset = (int)roundf(t * (float)((int)_layout.width() - (int)_textWidthPx));
        } else {
            float phase = _computePhase(now);
            xOffset = (int)roundf((float)_layout.width() - phase * (float)((int)_layout.width() + (int)_textWidthPx));
        }
        _render(xOffset);
    }

private:
    static constexpr float kBasePixelsPerSecond = 6.0f;  // scroll/bounce rate at speed=1.0

    MatrixLayout _layout;

    std::vector<uint8_t> _columns;      // one entry per pixel-column of the full message; bit r = row r lit
    uint16_t             _textWidthPx = 0;
    bool                 _animated    = false;

    void _recompute() {
        _columns.clear();
        for (const char* p = _cfg.text; *p; p++) {
            const Font5x7::Glyph& g = Font5x7::glyph(*p);
            for (uint8_t col = 0; col < Font5x7::GLYPH_WIDTH; col++) {
                uint8_t colBits = 0;
                for (uint8_t row = 0; row < Font5x7::GLYPH_HEIGHT; row++) {
                    if ((g.rows[row] >> (Font5x7::GLYPH_WIDTH - 1 - col)) & 1) colBits |= (uint8_t)(1 << row);
                }
                _columns.push_back(colBits);
            }
            _columns.push_back(0);  // 1px gap between characters
        }
        if (!_columns.empty()) _columns.pop_back();  // no trailing gap
        _textWidthPx = (uint16_t)_columns.size();
        _animated    = _textWidthPx > _layout.width();
    }

    void _render(int xOffset) {
        int yOffset = ((int)_layout.height() - (int)Font5x7::GLYPH_HEIGHT) / 2;
        for (uint16_t row = 0; row < _layout.height(); row++) {
            int fontRow = (int)row - yOffset;
            for (uint16_t col = 0; col < _layout.width(); col++) {
                int  srcCol = (int)col - xOffset;
                bool on = fontRow >= 0 && fontRow < Font5x7::GLYPH_HEIGHT &&
                          srcCol >= 0 && srcCol < (int)_columns.size() &&
                          ((_columns[srcCol] >> fontRow) & 1);
                uint16_t idx = _layout.ledIndex(row, col);
                if (on) _led->setPixel(idx, applyBrightness(_cfg.color.r), applyBrightness(_cfg.color.g), applyBrightness(_cfg.color.b));
                else    _led->setPixel(idx, 0, 0, 0);
            }
        }
        _led->show();
    }
};
