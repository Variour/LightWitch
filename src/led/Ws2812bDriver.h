#pragma once
#include <Adafruit_NeoPixel.h>

#include <memory>

#include "LedDriver.h"

class Ws2812bDriver : public LedDriver {
   public:
    void setup(uint8_t dataPin, uint16_t numLeds) {
        _pin = dataPin;
        _numLeds = numLeds;
    }

    void begin() override {
        _neo = std::make_unique<Adafruit_NeoPixel>(_numLeds, _pin, NEO_GRB + NEO_KHZ800);
        _neo->begin();
        _neo->clear();
        _neo->show();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint32_t c = _neo->Color(r, g, b);
        for (uint16_t i = 0; i < _numLeds; i++) _neo->setPixelColor(i, c);
        _neo->show();
    }

    void setPixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        _neo->setPixelColor(idx, r, g, b);
    }

    void show() override { _neo->show(); }

    void off() override {
        _neo->clear();
        _neo->show();
    }

   private:
    uint8_t _pin = 25;
    uint16_t _numLeds = 1;
    std::unique_ptr<Adafruit_NeoPixel> _neo;
};
