#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include "Pattern.h"
#include "../scenes/SceneManager.h"

// Continuously transitions between randomly sampled colors from a scene's first frame.
class SceneTransition : public Pattern {
public:
    float getPeriod() const override { return (float)_durationMs(); }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led     = &led;
        _cfg     = cfg;
        _startMs = millis();
        _loadPalette(cfg.sceneId);
        _from = _pick();
        _to   = _pick();
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        _cfg = cfg;
        if (sceneChanged) {
            _loadPalette(cfg.sceneId);
            _from    = _pick();
            _to      = _pick();
            _startMs = millis();
        }
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        uint32_t dur = _durationMs();
        float t = (float)(now - _startMs) / (float)dur;
        if (t >= 1.0f) {
            _from    = _to;
            _to      = _pick();
            _startMs = now;
            t        = 0.0f;
        }
        float s = t * t * (3.0f - 2.0f * t);  // smoothstep
        _led->setColor(
            applyBrightness(_lerp(_from.r, _to.r, s)),
            applyBrightness(_lerp(_from.g, _to.g, s)),
            applyBrightness(_lerp(_from.b, _to.b, s))
        );
    }

private:
    std::vector<Color> _palette;
    Color    _from{}, _to{};
    uint32_t _startMs = 0;

    uint32_t _durationMs() const {
        float t = _cfg.transitionTime;
        if (!isfinite(t) || t < 0.5f) t = 10.0f;
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
        const char* frame = frames[0] | "";
        size_t len = strlen(frame);
        for (size_t i = 0; i + 5 < len; i += 6) {
            char buf[7] = {};
            strncpy(buf, frame + i, 6);
            unsigned long v = strtoul(buf, nullptr, 16);
            _palette.push_back({(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v});
        }
    }

    Color _pick() const {
        if (_palette.empty()) return _cfg.color;
        return _palette[random(0, _palette.size())];
    }

    static uint8_t _lerp(uint8_t a, uint8_t b, float t) {
        return (uint8_t)((float)a + ((float)b - (float)a) * t);
    }
};
