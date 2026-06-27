#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include "Pattern.h"
#include "../scenes/SceneManager.h"

// Assigns each LED in a string a random color from the scene palette.
// With transitionEnabled, each LED independently cycles through colors
// with per-LED jitter so transitions are staggered.
// transitionTime controls both the hold duration and the fade duration
// (total cycle per LED = 2 × transitionTime).
class SceneString : public Pattern {
public:
    float getPeriod() const override { return 0.0f; }

    void setNumLeds(uint16_t n) { _numLeds = n; }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _loadPalette(cfg.sceneId);
        _initLeds(millis());
        _renderAll(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        bool transChanged = cfg.transitionEnabled != _cfg.transitionEnabled;
        _cfg = cfg;
        if (sceneChanged) {
            _loadPalette(cfg.sceneId);
            _initLeds(millis());
        } else if (transChanged) {
            _initLeds(millis());
        }
    }

    // Re-read the scene palette if it matches the currently loaded scene.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        _loadPalette(sceneId);
        _initLeds(millis());
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        if (_cfg.transitionEnabled && !_states.empty()) {
            uint32_t cycleMs = _cycleMs();
            for (auto& s : _states) {
                uint32_t elapsed = now - s.startMs;
                if (elapsed >= cycleMs * 2) {
                    s.from    = s.to;
                    s.to      = _pick();
                    s.startMs = now;
                }
            }
        }
        _renderAll(now);
    }

private:
    struct LedState {
        Color    from, to;
        uint32_t startMs;
    };

    uint16_t           _numLeds = 1;
    std::vector<Color>    _palette;
    std::vector<LedState> _states;

    uint32_t _cycleMs() const {
        float t = _cfg.transitionTime;
        if (!isfinite(t) || t < 0.1f) t = 2.0f;
        return (uint32_t)(t * 1000.0f);
    }

    void _loadPalette(const char* sceneId) {
        _palette.clear();
        if (!sceneId || !sceneId[0]) return;
        File f = LittleFS.open(SceneManager::path(sceneId).c_str(), "r");
        if (!f) return;
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); return; }
        f.close();
        JsonArray frames = doc["frames"].as<JsonArray>();
        if (!frames || !frames.size()) return;
        for (JsonArray frame : frames) {
            for (JsonVariant v : frame) {
                const char* hex = v | "";
                if (strlen(hex) < 6) continue;
                unsigned long rgb = strtoul(hex, nullptr, 16);
                _palette.push_back({(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb});
            }
        }
    }

    void _initLeds(uint32_t now) {
        _states.clear();
        if (_palette.empty()) return;
        uint32_t cycleMs = _cycleMs();
        randomSeed(micros() ^ (uint32_t)millis() ^ esp_random());
        _states.reserve(_numLeds);
        for (uint16_t i = 0; i < _numLeds; i++) {
            LedState s;
            s.from = _pick();
            s.to   = _pick();
            // Stagger start times across 2× the cycle so LEDs are fully offset
            s.startMs = now - (uint32_t)random(0, cycleMs * 2);
            _states.push_back(s);
        }
    }

    void _renderAll(uint32_t now) {
        if (_states.empty()) {
            // No palette: fall back to cfg color for all LEDs
            Color c = _cfg.color;
            for (uint16_t i = 0; i < _numLeds; i++)
                _led->setPixel(i, applyBrightness(c.r), applyBrightness(c.g), applyBrightness(c.b));
            _led->show();
            return;
        }

        uint32_t cycleMs = _cycleMs();
        for (uint16_t i = 0; i < _numLeds && i < (uint16_t)_states.size(); i++) {
            const auto& s = _states[i];
            uint8_t r, g, b;
            if (!_cfg.transitionEnabled) {
                // Static: show the initial random color
                r = applyBrightness(s.from.r);
                g = applyBrightness(s.from.g);
                b = applyBrightness(s.from.b);
            } else {
                uint32_t elapsed = now - s.startMs;
                if (elapsed < cycleMs) {
                    // Holding current color
                    r = applyBrightness(s.from.r);
                    g = applyBrightness(s.from.g);
                    b = applyBrightness(s.from.b);
                } else {
                    // Fading to next color
                    float t = (float)(elapsed - cycleMs) / (float)cycleMs;
                    if (t > 1.0f) t = 1.0f;
                    float smooth = t * t * (3.0f - 2.0f * t);
                    r = applyBrightness(_lerp(s.from.r, s.to.r, smooth));
                    g = applyBrightness(_lerp(s.from.g, s.to.g, smooth));
                    b = applyBrightness(_lerp(s.from.b, s.to.b, smooth));
                }
            }
            _led->setPixel(i, r, g, b);
        }
        _led->show();
    }

    Color _pick() const {
        if (_palette.empty()) return _cfg.color;
        return _palette[random(0, _palette.size())];
    }

    static uint8_t _lerp(uint8_t a, uint8_t b, float t) {
        return (uint8_t)((float)a + ((float)b - (float)a) * t);
    }
};
