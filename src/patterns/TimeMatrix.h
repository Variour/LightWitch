#pragma once
#include "../logging/Logger.h"
#include "../timesync/TimeSync.h"
#include "MatrixLayout.h"
#include "Pattern.h"

// Renders the current local time (HH:MM) on a matrix light using a small
// fixed-resolution digit font, stretched (nearest-neighbor) to fill the
// light's actual size — the same approach SceneMatrix uses for scene images.
//
// Requires a matrix at least TIME_BASE_W x TIME_BASE_H; smaller matrices
// (including string lights, height==1) can't legibly show digits and fall
// back to the same blinking "unavailable" indicator used while there's no
// synced time yet.
class TimeMatrix : public Pattern {
   public:
    static constexpr uint16_t BASE_W = 17;
    static constexpr uint16_t BASE_H = 5;

    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _layout.setDimensions(w, h); }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _layout.setWiring(start, dir, serpentine);
    }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _lastMinuteKey = -1;
        tick(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        _cfg = cfg;
        _lastMinuteKey = -1;  // force redraw so brightness/color/format changes apply now
        tick(millis());
    }

    void tick(uint32_t now) override {
        if (!_led) return;

        bool tooSmall = _layout.width() < BASE_W || _layout.height() < BASE_H;
        if (tooSmall && !_loggedTooSmall) {
            Logger::w("[time] matrix %ux%u smaller than required %ux%u — time mode unavailable",
                      _layout.width(), _layout.height(), BASE_W, BASE_H);
            _loggedTooSmall = true;
        }

        struct tm ti;
        if (tooSmall || !TimeSync::getLocalTime(ti)) {
            // Force a redraw on the first tick entering the indicator state (the
            // strip may currently be showing stale HH:MM digits from before),
            // even if the blink phase happens to match what was last drawn.
            bool enteringIndicator = !_showingIndicator;
            _showingIndicator = true;
            bool on = (now / 500) % 2 == 0;
            if (enteringIndicator || on != _indicatorOn) {
                _indicatorOn = on;
                _renderIndicator(on);
            }
            return;
        }

        int hour = ti.tm_hour;
        if (!_cfg.time24h) {
            hour = hour % 12;
            if (hour == 0) hour = 12;
        }
        int minuteKey = hour * 100 + ti.tm_min;
        // Force a redraw on the first valid tick after showing the indicator,
        // even if the minute happens to match what was last drawn before that.
        if (minuteKey == _lastMinuteKey && !_showingIndicator) return;
        _showingIndicator = false;
        _lastMinuteKey = minuteKey;
        _renderTime(hour, ti.tm_min);
    }

   private:
    MatrixLayout _layout;
    int _lastMinuteKey = -1;
    bool _loggedTooSmall = false;
    bool _showingIndicator = true;
    int8_t _indicatorOn = -1;  // -1 = not drawn yet, forces the first redraw

    // 3 columns x 5 rows per digit; each row's 3 bits are left-to-right (bit 2 = leftmost).
    static constexpr uint8_t DIGIT_FONT[10][5] = {
        {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
        {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
        {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
        {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
        {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
        {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
        {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
        {0b111, 0b001, 0b001, 0b001, 0b001},  // 7
        {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
        {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
    };

    static void _drawDigit(bool base[BASE_H][BASE_W], uint16_t col0, uint8_t digit) {
        for (uint16_t row = 0; row < 5; row++) {
            uint8_t bits = DIGIT_FONT[digit][row];
            for (uint16_t c = 0; c < 3; c++) base[row][col0 + c] = (bits >> (2 - c)) & 1;
        }
    }

    // Layout: HH : MM across 17 columns — digit(3) gap digit(3) gap colon(1) gap digit(3) gap
    // digit(3).
    void _renderTime(int hour, int minute) {
        bool base[BASE_H][BASE_W] = {};
        _drawDigit(base, 0, hour / 10);
        _drawDigit(base, 4, hour % 10);
        base[1][8] = true;
        base[3][8] = true;
        _drawDigit(base, 10, minute / 10);
        _drawDigit(base, 14, minute % 10);
        _blit(base);
    }

    void _renderIndicator(bool on) {
        bool base[BASE_H][BASE_W];
        for (auto& row : base)
            for (auto& v : row) v = on;
        _blit(base, /*dim=*/true);
    }

    // dim: renders the "no clock available" indicator at a fixed low intensity,
    // ignoring the user's brightness setting — it needs to stay visibly an
    // error state even when Time mode's brightness is turned down or to 0.
    void _blit(bool base[BASE_H][BASE_W], bool dim = false) {
        Color c = dim ? Color{40, 0, 0} : _cfg.color;
        for (uint16_t row = 0; row < _layout.height(); row++) {
            uint16_t srcRow = MatrixLayout::nearest(row, _layout.height(), BASE_H);
            for (uint16_t col = 0; col < _layout.width(); col++) {
                uint16_t srcCol = MatrixLayout::nearest(col, _layout.width(), BASE_W);
                uint16_t li = _layout.ledIndex(row, col);
                if (!base[srcRow][srcCol]) {
                    _led->setPixel(li, 0, 0, 0);
                } else if (dim) {
                    _led->setPixel(li, c.r, c.g, c.b);
                } else {
                    _led->setPixel(li, applyBrightness(c.r), applyBrightness(c.g),
                                   applyBrightness(c.b));
                }
            }
        }
        _led->show();
    }
};
