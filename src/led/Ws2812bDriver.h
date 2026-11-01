#pragma once
#include <Adafruit_NeoPixel.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <vector>

#include "LedDriver.h"

class Ws2812bDriver : public LedDriver {
   public:
    // Current of one colour channel at full scale (255), the model behind the
    // optional current cap: ~12 mA per channel, 36 mA per pixel at full white
    // (SK6812MINI-E, Buzzerboard hardware/README.md "LED rail = PMID":
    // 34 x 36 mA ≈ 1.2 A). A pixel's idle current is not part of the estimate.
    // UNVERIFIED: 12 mA comes from the board README's 36 mA per white pixel,
    // not from the SK6812MINI-E datasheet; measure the chain at full white.
    static constexpr uint32_t CHANNEL_FULL_SCALE_MA = 12;
    // With a cap, the estimated current rises by at most this much per shown
    // frame, so a bright frame after a dark one (or after the LED rail comes
    // up) ramps up over a few frames instead of one step.
    static constexpr uint32_t CAP_SLEW_MA_PER_FRAME = 50;

    void setup(uint8_t dataPin, uint16_t numLeds, ColorOrder order = ColorOrder::GRB) {
        _pin = dataPin;
        _numLeds = numLeds;
        _order = order;
    }

    void begin() override {
        // NEO_RGB: the library sends channels in raw call order with no reordering
        // of its own — applyColorOrder() below is the single place that permutes
        // r,g,b, so it isn't double-applied on top of the library's own remapping.
        _neo = std::make_unique<Adafruit_NeoPixel>(_numLeds, _pin, NEO_RGB + NEO_KHZ800);
        _neo->begin();
        _neo->clear();
        _neo->show();
    }

    void show() override {
        _shownSinceService = true;
        _syncCurrentLimit();
        if (_limitMa) _writeCappedFrame();
        _neo->show();
    }

    void off() override {
        _syncCurrentLimit();
        if (_limitMa) memset(_frame.data(), 0, _frame.size());
        _shownCounts = 0;
        _rampPending = false;
        _neo->clear();
        _neo->show();
    }

    void setRailPowered(bool powered) override {
        _railPowered = powered;
        if (!powered) _shownCounts = 0;
    }

    // Continues a slew-limited ramp for frames that are not redrawn (static
    // patterns), one step per render tick.
    void service() override {
        if (_rampPending && _railPowered && !_shownSinceService) show();
        _shownSinceService = false;
    }

    void setColorOrder(ColorOrder order) override { _order = order; }

    // Safe to call from any task: the new cap takes effect on the next write
    // or show(), which run on the render (loop) task.
    void setCurrentLimitMa(uint16_t mA) override { _requestedLimitMa.store(mA); }

   protected:
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _syncCurrentLimit();
        if (_limitMa) {
            for (size_t k = 0; k + 2 < _frame.size(); k += 3) {
                _frame[k] = a;
                _frame[k + 1] = bb;
                _frame[k + 2] = c;
            }
        } else {
            uint32_t col = _neo->Color(a, bb, c);
            for (uint16_t i = 0; i < _numLeds; i++) _neo->setPixelColor(i, col);
        }
        show();
    }

    void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _syncCurrentLimit();
        if (_limitMa) {
            if (idx >= _numLeds) return;
            _frame[(size_t)idx * 3] = a;
            _frame[(size_t)idx * 3 + 1] = bb;
            _frame[(size_t)idx * 3 + 2] = c;
            return;
        }
        _neo->setPixelColor(idx, a, bb, c);
    }

   private:
    uint8_t _pin = 25;
    uint16_t _numLeds = 1;
    ColorOrder _order = ColorOrder::GRB;
    std::unique_ptr<Adafruit_NeoPixel> _neo;

    // Current cap. With a cap, pixel writes go to _frame (the frame as
    // requested, in wire order) and show() writes a scaled copy into the
    // NeoPixel buffer; without one (0), writes go straight to the NeoPixel
    // buffer as before and _frame stays empty.
    std::atomic<uint16_t> _requestedLimitMa{0};
    uint16_t _limitMa = 0;
    std::vector<uint8_t> _frame;
    // Capped mode only: channel sum of what the LEDs show now (0 while the
    // rail is down), and whether the slew limit held the last frame back.
    uint32_t _shownCounts = 0;
    bool _rampPending = false;
    bool _shownSinceService = false;
    bool _railPowered = true;

    // Applies a cap change from setCurrentLimitMa() on the render task. The
    // NeoPixel buffer holds 3 bytes per pixel in call (= wire) order for
    // NEO_RGB, the same layout as _frame.
    void _syncCurrentLimit() {
        uint16_t want = _requestedLimitMa.load();
        if (want == _limitMa || !_neo) return;
        size_t bytes = (size_t)_numLeds * 3;
        if (_limitMa == 0) {
            // Cap switches on: the uncapped NeoPixel buffer becomes the
            // requested frame.
            const uint8_t* p = _neo->getPixels();
            _frame.assign(p, p + bytes);
            _shownCounts = 0;
            if (_railPowered)
                for (uint8_t v : _frame) _shownCounts += v;
        } else if (want == 0) {
            // Cap switches off: the NeoPixel buffer gets the requested frame
            // back, unscaled.
            memcpy(_neo->getPixels(), _frame.data(), bytes);
            std::vector<uint8_t>().swap(_frame);
        }
        _limitMa = want;
    }

    // Estimates the frame's current as sum(channel values) x
    // CHANNEL_FULL_SCALE_MA / 255 and, if it exceeds the budget, scales every
    // channel by the same factor (rounded down, so the result stays within
    // it). The budget is the cap, or less while the slew limit ramps up from
    // the current shown frame.
    void _writeCappedFrame() {
        uint8_t* p = _neo->getPixels();
        uint32_t sum = 0;
        for (uint8_t v : _frame) sum += v;
        uint32_t cap = (uint32_t)_limitMa * 255 / CHANNEL_FULL_SCALE_MA;
        uint32_t slewed = _shownCounts + CAP_SLEW_MA_PER_FRAME * 255 / CHANNEL_FULL_SCALE_MA;
        uint32_t budget = slewed < cap ? slewed : cap;
        uint32_t shown = 0;
        if (sum <= budget) {
            memcpy(p, _frame.data(), _frame.size());
            shown = sum;
        } else {
            for (size_t k = 0; k < _frame.size(); k++) {
                p[k] = (uint8_t)((uint32_t)_frame[k] * budget / sum);
                shown += p[k];
            }
        }
        _rampPending = sum > budget && budget < cap;
        _shownCounts = _railPowered ? shown : 0;
    }
};
