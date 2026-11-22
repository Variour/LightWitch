#pragma once
#include <Arduino.h>
#include <math.h>

#include "../led/LampDriver.h"
#include "Pattern.h"

// Fire that rises upward (docs/lamp/README.md, section 4). Heat is 3D value
// noise drifting up in lamp coordinates, so neighbouring emitters on one
// side flicker together and opposite sides independently; it fades with
// height. Without a lamp, the LED index stands in for height. Brightness
// is applied per sample (255 in the lamp's physical modes, see PatternRunner).
class FirePattern : public Pattern {
   public:
    static constexpr float CELL_MM = 18.0f;    // noise feature size
    static constexpr float RISE_MM_S = 45.0f;  // upward drift at speed 1
    static constexpr uint32_t FRAME_MS = 30;

    // Sets the lamp driver of this light, or nullptr for plain strips.
    void setLamp(LampDriver* lamp) { _lamp = lamp; }
    void setNumLeds(uint16_t n) { _numLeds = n ? n : 1; }

    float getPeriod() const override { return 0.0f; }

    void tick(uint32_t now) override {
        if (now - _last < FRAME_MS) return;
        _last = now;
        float speed = _cfg.speed > 0.1f ? _cfg.speed : 0.1f;
        _t = (float)(now - _startMs) / 1000.0f * speed;
        // Slow common flicker of the whole flame.
        _flick += ((float)random(0, 1000) / 1000.0f * 0.25f + 0.75f - _flick) * 0.2f;
        if (_lamp && _lamp->output()) {
            _z0 = lamp::Lamp::zMin();
            float h = lamp::Lamp::zMax() - _z0;
            _zSpan = h > 1.0f ? h : 1.0f;
            _lamp->renderField(&FirePattern::_sample, this);
        } else {
            // Strip: index 0 is the bottom, spread over an assumed 300 mm.
            _z0 = 0;
            _zSpan = 300.0f;
            uint8_t rgb[3];
            for (uint16_t i = 0; i < _numLeds; i++) {
                float pos[3] = {0, 0, _numLeds > 1 ? 300.0f * i / (_numLeds - 1) : 0};
                _sample(this, pos, rgb);
                _led->setPixel(i, rgb[0], rgb[1], rgb[2]);
            }
        }
        _led->show();
    }

   private:
    LampDriver* _lamp = nullptr;
    uint16_t _numLeds = 1;
    uint32_t _last = 0;
    float _t = 0, _flick = 1.0f, _z0 = 0, _zSpan = 1;

    static float _hash(int x, int y, int z) {
        uint32_t h =
            (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    }
    static float _smooth(float t) { return t * t * (3.0f - 2.0f * t); }
    static float _noise(float x, float y, float z) {
        int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
        float fx = _smooth(x - xi), fy = _smooth(y - yi), fz = _smooth(z - zi);
        float v[2][2];
        for (int dz = 0; dz < 2; dz++)
            for (int dy = 0; dy < 2; dy++) {
                float a = _hash(xi, yi + dy, zi + dz), b = _hash(xi + 1, yi + dy, zi + dz);
                v[dz][dy] = a + (b - a) * fx;
            }
        float lo = v[0][0] + (v[0][1] - v[0][0]) * fy;
        float hi = v[1][0] + (v[1][1] - v[1][0]) * fy;
        return lo + (hi - lo) * fz;
    }

    static void _sample(void* ctx, const float* pos, uint8_t* rgb) {
        auto* self = static_cast<FirePattern*>(ctx);
        float zn = (pos[2] - self->_z0) / self->_zSpan;
        if (zn < 0) zn = 0;
        if (zn > 1) zn = 1;
        float zr = pos[2] - self->_t * RISE_MM_S;
        float n = 0.65f * _noise(pos[0] / CELL_MM, pos[1] / CELL_MM, zr / CELL_MM) +
                  0.35f * _noise(pos[0] / (CELL_MM * 0.5f), pos[1] / (CELL_MM * 0.5f),
                                 zr / (CELL_MM * 0.5f) + 17.0f);
        float heat = (1.0f - 0.8f * zn) * (0.2f + 1.0f * n) * self->_flick;
        if (heat > 1) heat = 1;
        // Heat to colour: dark red, orange, yellow, warm white.
        float r = heat * 2.2f, g = (heat - 0.3f) * 1.4f, b = (heat - 0.8f) * 2.5f;
        rgb[0] = self->applyBrightness((uint8_t)(255.0f * (r > 1 ? 1 : r)));
        rgb[1] = self->applyBrightness((uint8_t)(255.0f * (g < 0 ? 0 : g > 0.85f ? 0.85f : g)));
        rgb[2] = self->applyBrightness((uint8_t)(255.0f * (b < 0 ? 0 : b > 0.6f ? 0.6f : b)));
    }
};
