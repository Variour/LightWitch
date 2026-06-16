#pragma once
#include "LedDriver.h"
#include <FastLED.h>

#ifndef LED_DATA_PIN
#define LED_DATA_PIN 25
#endif
#ifndef LED_CLOCK_PIN
#define LED_CLOCK_PIN 26
#endif

class Ws2801Driver : public LedDriver {
public:
    static constexpr uint8_t DATA_PIN    = LED_DATA_PIN;  // SPI MOSI
    static constexpr uint8_t CLOCK_PIN   = LED_CLOCK_PIN;  // SPI SCK
    static constexpr uint8_t NUM_LEDS    = 100; // full strip length — cleared on boot
    static constexpr uint8_t ACTIVE_LEDS = 1;   // LEDs driven by patterns

    void begin() override {
        FastLED.addLeds<WS2801, DATA_PIN, CLOCK_PIN, RGB>(_leds, NUM_LEDS);
        FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
        off();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        for (uint8_t i = 0; i < ACTIVE_LEDS; i++) _leds[i] = CRGB(r, g, b);
        FastLED.show();
    }

    void off() override {
        fill_solid(_leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
    }

private:
    CRGB _leds[NUM_LEDS];
};
