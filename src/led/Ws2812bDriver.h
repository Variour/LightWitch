#pragma once
#include <Adafruit_NeoPixel.h>

#include <memory>

#include "LedDriver.h"

class Ws2812bDriver : public LedDriver {
   public:
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

    void show() override { _neo->show(); }

    void off() override {
        _neo->clear();
        _neo->show();
    }

    void setColorOrder(ColorOrder order) override { _order = order; }

   protected:
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        uint32_t col = _neo->Color(a, bb, c);
        for (uint16_t i = 0; i < _numLeds; i++) _neo->setPixelColor(i, col);
        _neo->show();
    }

    void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _neo->setPixelColor(idx, a, bb, c);
    }

   private:
    uint8_t _pin = 25;
    uint16_t _numLeds = 1;
    ColorOrder _order = ColorOrder::GRB;
    std::unique_ptr<Adafruit_NeoPixel> _neo;
};
