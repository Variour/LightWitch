#pragma once
#include "LedDriver.h"
#include <Arduino.h>

// Common-cathode RGB LED driven via ESP32 LEDC PWM.
// Adjust PIN_R/G/B and FREQ/RESOLUTION to match your wiring.
class RgbLedDriver : public LedDriver {
public:
    static constexpr uint8_t PIN_R = 25;
    static constexpr uint8_t PIN_G = 26;
    static constexpr uint8_t PIN_B = 27;

    // LEDC channels — pick any three free channels (0-15)
    static constexpr uint8_t CH_R = 0;
    static constexpr uint8_t CH_G = 1;
    static constexpr uint8_t CH_B = 2;

    static constexpr uint32_t FREQ_HZ   = 5000;
    static constexpr uint8_t  BITS      = 8;   // 0-255 duty

    void begin() override {
        ledcSetup(CH_R, FREQ_HZ, BITS);
        ledcSetup(CH_G, FREQ_HZ, BITS);
        ledcSetup(CH_B, FREQ_HZ, BITS);
        ledcAttachPin(PIN_R, CH_R);
        ledcAttachPin(PIN_G, CH_G);
        ledcAttachPin(PIN_B, CH_B);
        off();
    }

    void setColor(uint8_t r, uint8_t g, uint8_t b) override {
        ledcWrite(CH_R, gamma8(r));
        ledcWrite(CH_G, gamma8(g));
        ledcWrite(CH_B, gamma8(b));
    }

private:
    // Simple gamma-2.2 correction lookup for perceptually linear brightness
    static uint8_t gamma8(uint8_t v) {
        static const uint8_t PROGMEM table[256] = {
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
            1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,
            2,3,3,3,3,3,3,3,4,4,4,4,4,5,5,5,
            5,6,6,6,6,7,7,7,7,8,8,8,9,9,9,10,
            10,10,11,11,11,12,12,13,13,13,14,14,15,15,16,16,
            17,17,18,18,19,19,20,20,21,21,22,22,23,24,24,25,
            25,26,27,27,28,29,29,30,31,32,32,33,34,35,35,36,
            37,38,39,39,40,41,42,43,44,45,46,47,48,49,50,50,
            51,52,54,55,56,57,58,59,60,61,62,63,64,66,67,68,
            69,70,72,73,74,75,77,78,79,81,82,83,85,86,87,89,
            90,92,93,95,96,98,99,101,102,104,105,107,109,110,112,114,
            115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
            144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
            177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
            215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255
        };
        return pgm_read_byte(&table[v]);
    }
};
