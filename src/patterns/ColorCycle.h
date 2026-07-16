#pragma once
#include "Pattern.h"

class ColorCycle : public Pattern {
   public:
    float getPeriod() const override { return 10000.0f / _cfg.speed; }

    void tick(uint32_t now) override {
        float hue = _computePhase(now);
        uint8_t r, g, b;
        _hsvToRgb(hue, 1.0f, _cfg.brightness / 255.0f, r, g, b);
        _led->setColor(r, g, b);
    }

   private:
    static void _hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
        int i = (int)(h * 6.0f);
        float f = h * 6.0f - i;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);
        float fr, fg, fb;
        switch (i % 6) {
            case 0:
                fr = v;
                fg = t;
                fb = p;
                break;
            case 1:
                fr = q;
                fg = v;
                fb = p;
                break;
            case 2:
                fr = p;
                fg = v;
                fb = t;
                break;
            case 3:
                fr = p;
                fg = q;
                fb = v;
                break;
            case 4:
                fr = t;
                fg = p;
                fb = v;
                break;
            default:
                fr = v;
                fg = p;
                fb = q;
                break;
        }
        r = (uint8_t)(fr * 255.0f);
        g = (uint8_t)(fg * 255.0f);
        b = (uint8_t)(fb * 255.0f);
    }
};
