#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Pattern.h"
#include "StaticColor.h"
#include "Breathing.h"
#include "ColorCycle.h"
#include "Strobe.h"
#include "Candle.h"
#include "SceneTransition.h"
#include "Proximity.h"
#include "../led/LedDriver.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../scenes/SceneManager.h"
#include "../mesh/PeerRegistry.h"

// Owns the active pattern instance and switches patterns on config change.
class PatternRunner {
public:
    void begin(LedDriver& led) {
        _led = &led;
        applyConfig(Config::light());
    }

    void setPeerRegistry(PeerRegistry* peers) { _proximity.setPeers(peers); }

    void applyConfig(const LightConfig& cfg) {
        if (Config::get().proximityEnabled && cfg.mode == GroupMode::Proximity) {
            if (_current != &_proximity) {
                Logger::i("[pattern] → Proximity  rgb(%u,%u,%u) br=%u scale=%.1f",
                          cfg.color.r, cfg.color.g, cfg.color.b, cfg.brightness, cfg.proximityScale);
                _proximity.begin(*_led, cfg);
                _current           = &_proximity;
                _currentId         = (PatternId)0xFF;
                _inSceneTransition = false;
            } else {
                _proximity.applyConfig(cfg);
            }
            return;
        }

        if (cfg.mode == GroupMode::Scene && cfg.transitionEnabled) {
            if (!_inSceneTransition) {
                Logger::i("[pattern] → SceneTransition  scene=%s br=%u t=%.1fs",
                          cfg.sceneId, cfg.brightness, cfg.transitionTime);
                _sceneTransition.begin(*_led, cfg);
                _current          = &_sceneTransition;
                _currentId        = (PatternId)0xFF;  // invalidate so exiting resets correctly
                _inSceneTransition = true;
            } else {
                _sceneTransition.applyConfig(cfg);
            }
            return;
        }
        _inSceneTransition = false;

        LightConfig effective = cfg;
        if (effective.mode == GroupMode::Scene) {
            effective.pattern = PatternId::Static;
            effective.color   = _sceneColor(cfg);
        }

        if (_current == nullptr || effective.pattern != _currentId) {
            static const char* const _names[] = { "Static", "Breathing", "ColorCycle", "Strobe", "Candle" };
            const char* name = (uint8_t)effective.pattern < 5 ? _names[(uint8_t)effective.pattern] : "?";
            Logger::i("[pattern] → %s  rgb(%u,%u,%u) br=%u spd=%.1f",
                      name, effective.color.r, effective.color.g, effective.color.b, effective.brightness, effective.speed);
            _currentId = effective.pattern;
            switch (effective.pattern) {
                case PatternId::Breathing:  _current = &_breathing;  break;
                case PatternId::ColorCycle: _current = &_colorCycle; break;
                case PatternId::Strobe:     _current = &_strobe;     break;
                case PatternId::Candle:     _current = &_candle;     break;
                default:                    _current = &_static;     break;
            }
            _current->begin(*_led, effective);
        } else {
            _current->applyConfig(effective);
        }
    }

    void tick() {
        if (_current) _current->tick(millis());
    }

    float getPhase()        const { return _current ? _current->getPhase()    : 0.0f; }
    void  snapPhase(float p)      {
        if (_current) {
            _current->snapPhase(p);
        }
    }
    void  resetPhase()            { if (_current)     _current->resetPhase(); }

private:
    Color _sceneColor(const LightConfig& cfg) const {
        Color fallback = cfg.color;
        if (cfg.mode != GroupMode::Scene || strlen(cfg.sceneId) == 0) return fallback;

        String path = SceneManager::path(cfg.sceneId);
        File f = LittleFS.open(path.c_str(), "r");
        if (!f) return fallback;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return fallback;

        JsonArray frames = doc["frames"].as<JsonArray>();
        if (!frames.size()) return fallback;

        JsonArray frame = frames[0].as<JsonArray>();
        if (!frame || !frame.size()) return fallback;

        randomSeed(micros() ^ (uint32_t)millis() ^ esp_random());
        size_t idx = random(0, frame.size());

        const char* hex = frame[idx] | "";
        if (strlen(hex) < 6) return fallback;
        unsigned long value = strtoul(hex, nullptr, 16);
        return Color((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    }

    // ── state ────────────────────────────────────────────────────────────────
    LedDriver*  _led               = nullptr;
    Pattern*    _current           = nullptr;
    PatternId   _currentId         = PatternId::Static;
    bool        _inSceneTransition = false;

    // ── pattern instances ─────────────────────────────────────────────────────
    StaticColor     _static;
    Breathing       _breathing;
    ColorCycle      _colorCycle;
    Strobe          _strobe;
    CandlePattern   _candle;
    SceneTransition _sceneTransition;
    Proximity       _proximity;
};
