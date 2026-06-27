#pragma once
#include "LedDriver.h"
#include <Adafruit_WS2801.h>

class Ws2801Driver : public LedDriver {
public:
    void setup(uint8_t dataPin, uint8_t clockPin, uint16_t numLeds) {
        _dataPin  = dataPin;
        _clockPin = clockPin;
        _numLeds  = numLeds;
    }

    void begin() override {
        _ws = new Adafruit_WS2801(_numLeds, _dataPin, _clockPin);
        _ws->begin();
        _ws->show();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        for (uint16_t i = 0; i < _numLeds; i++) _ws->setPixelColor(i, r, g, b);
        _ws->show();
    }

    void off() override {
        for (uint16_t i = 0; i < _numLeds; i++) _ws->setPixelColor(i, 0);
        _ws->show();
    }

private:
    uint8_t          _dataPin  = 25;
    uint8_t          _clockPin = 26;
    uint16_t         _numLeds  = 1;
    Adafruit_WS2801* _ws       = nullptr;
};
