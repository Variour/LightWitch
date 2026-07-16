#pragma once
#include <Adafruit_WS2801.h>

#include <memory>

#include "LedDriver.h"

class Ws2801Driver : public LedDriver {
   public:
    void setup(uint8_t dataPin, uint8_t clockPin, uint16_t numLeds,
               ColorOrder order = ColorOrder::RGB) {
        _dataPin = dataPin;
        _clockPin = clockPin;
        _numLeds = numLeds;
        _order = order;
    }

    void begin() override {
        _ws = std::make_unique<Adafruit_WS2801>(_numLeds, _dataPin, _clockPin);
        _ws->begin();
        _ws->show();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        for (uint16_t i = 0; i < _numLeds; i++) _ws->setPixelColor(i, a, bb, c);
        _ws->show();
    }

    void setPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _ws->setPixelColor(idx, a, bb, c);
    }

    void show() override { _ws->show(); }

    void off() override {
        for (uint16_t i = 0; i < _numLeds; i++) _ws->setPixelColor(i, 0);
        _ws->show();
    }

    void setColorOrder(ColorOrder order) override { _order = order; }

   private:
    uint8_t _dataPin = 25;
    uint8_t _clockPin = 26;
    uint16_t _numLeds = 1;
    ColorOrder _order = ColorOrder::RGB;
    std::unique_ptr<Adafruit_WS2801> _ws;
};
