#pragma once
#include "../lamp/Lamp.h"
#include "LedDriver.h"

// Puts the lamp brightness model (docs/lamp/README.md) in front of a light's
// real driver. Patterns keep writing RGB colours; LampDriver turns them into
// channel duties per emitter with Lamp::emitterDuty, at the area brightness
// of the group level set with setLevel(). In lamp mode Raw it passes colours
// through unchanged. The inner driver runs with wire order RGB, the lamp
// file already lists channels in wire order.
class LampDriver : public LedDriver {
   public:
    // Called per emitter by renderField: pos in mm (lamp coordinates), writes
    // the colour for that place into rgb.
    using FieldFn = void (*)(void* ctx, const float* pos, uint8_t* rgb);

    void setup(LedDriver* inner, const lamp::Output* out) {
        _inner = inner;
        _out = out;
    }
    const lamp::Output* output() const { return _out; }

    // Group brightness (0-255) on the fixed lamp scale.
    void setLevel(uint8_t b) { _M = lamp::levelToDensity(b); }
    bool physical() const { return lamp::Lamp::mode() != lamp::Mode::Raw; }

    void begin() override { _inner->begin(); }
    void show() override { _inner->show(); }
    void off() override { _inner->off(); }
    void setCurrentLimitMa(uint16_t mA) override { _inner->setCurrentLimitMa(mA); }
    void setRailPowered(bool powered) override { _inner->setRailPowered(powered); }
    void service() override { _inner->service(); }

    // Renders a colour field over the emitters' positions; the caller shows.
    void renderField(FieldFn fn, void* ctx) {
        const lamp::Channel* ch = _out->ch.data();
        uint8_t rgb[3];
        float pos[3];
        for (uint16_t p = 0; p < _out->pixels; p++) {
            const lamp::Channel* c = ch + (size_t)p * 3;
            uint8_t duty[3] = {0, 0, 0};
            uint8_t i = 0;
            while (i < 3) {
                uint8_t n = 1;
                while (i + n < 3 && !c[i + n].firstOfEmitter) n++;
                for (uint8_t k = 0; k < 3; k++) pos[k] = c[i].pos[k] / 10.0f;
                fn(ctx, pos, rgb);
                rgb[0] = _clampValue(rgb[0]);
                rgb[1] = _clampValue(rgb[1]);
                rgb[2] = _clampValue(rgb[2]);
                if (physical()) {
                    lamp::Lamp::emitterDuty(c + i, n, rgb[0], rgb[1], rgb[2], _M, duty + i);
                } else {
                    for (uint8_t k = 0; k < n; k++) duty[i + k] = _rawFor(c[i + k].kind, rgb);
                }
                i += n;
            }
            _inner->setPixel(p, duty[0], duty[1], duty[2]);
        }
    }

   protected:
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override {
        for (uint16_t p = 0; p < _out->pixels; p++) _writeConverted(p, r, g, b);
        _inner->show();
    }
    void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        if (idx < _out->pixels) _writeConverted(idx, r, g, b);
    }

   private:
    LedDriver* _inner = nullptr;
    const lamp::Output* _out = nullptr;
    float _M = 0;

    // Raw mode: r/g/b kinds take their colour component, white and amber
    // chips the colour's maximum, so a raw colour still lights every chip.
    static uint8_t _rawFor(uint8_t kind, const uint8_t* rgb) {
        switch (kind) {
            case lamp::K_R:
                return rgb[0];
            case lamp::K_G:
                return rgb[1];
            case lamp::K_B:
                return rgb[2];
            case lamp::K_NONE:
                return 0;
            default: {
                uint8_t m = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
                return rgb[2] > m ? rgb[2] : m;
            }
        }
    }

    void _writeConverted(uint16_t p, uint8_t r, uint8_t g, uint8_t b) {
        const lamp::Channel* c = _out->ch.data() + (size_t)p * 3;
        uint8_t duty[3] = {0, 0, 0};
        uint8_t rgb[3] = {r, g, b};
        uint8_t i = 0;
        while (i < 3) {
            uint8_t n = 1;
            while (i + n < 3 && !c[i + n].firstOfEmitter) n++;
            if (physical())
                lamp::Lamp::emitterDuty(c + i, n, r, g, b, _M, duty + i);
            else
                for (uint8_t k = 0; k < n; k++) duty[i + k] = _rawFor(c[i + k].kind, rgb);
            i += n;
        }
        _inner->setPixel(p, duty[0], duty[1], duty[2]);
    }

    // The base class clamp (brightnessLimit/brightnessScale, LED blanking)
    // applies to the incoming colour, renderField bypasses setPixel.
    uint8_t _clampValue(uint8_t v) const { return clampValue(v); }
};
