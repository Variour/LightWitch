#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Pattern.h"
#include "StaticColor.h"
#include "Breathing.h"
#include "ColorCycle.h"
#include "Strobe.h"
#include "Candle.h"
#include "SceneMatrix.h"
#include "SceneString.h"
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
    }

    void setDimensions(uint16_t w, uint16_t h) {
        _width  = w;
        _height = h;
    }

    void setMatrixLayout(MatrixStart start, MatrixDirection dir) {
        _matrixStart = start;
        _matrixDir   = dir;
        _sceneMatrix.setMatrixLayout(start, dir);
    }

    // Topology: whether the last LED on an axis wraps back to the first (ring/cylinder/torus).
    // Not yet consumed by any pattern — stored here so position-aware modes can query it.
    void setWrap(bool wrapWidth, bool wrapHeight) {
        _wrapWidth  = wrapWidth;
        _wrapHeight = wrapHeight;
    }

    bool wrapWidth()  const { return _wrapWidth; }
    bool wrapHeight() const { return _wrapHeight; }

    void setPeerRegistry(PeerRegistry* peers) { _proximity.setPeers(peers); }
    void setGroupId(uint8_t groupId)          { _proximity.setGroupId(groupId); }

    void showTest(uint32_t durationMs) {
        if (!_led || _height < 2) return;
        _testStart    = millis();
        _testDuration = durationMs;
        _testActive   = true;
        _renderTest();
    }

    void applyConfig(const LightConfig& cfg) {
        _savedConfig = cfg;
        if (_testActive) return;
        if (cfg.mode == GroupMode::Proximity) {
            _sceneMode = SceneMode::None;
            if (_current != &_proximity) {
                Logger::i("[pattern] → Proximity  rgb(%u,%u,%u) br=%u scale=%.1f",
                          cfg.color.r, cfg.color.g, cfg.color.b, cfg.brightness, cfg.proximityScale);
                _proximity.begin(*_led, cfg);
                _current   = &_proximity;
                _currentId = (PatternId)0xFF;
            } else {
                _proximity.applyConfig(cfg);
            }
            return;
        }

        if (cfg.mode == GroupMode::Scene) {
            if (_height > 1) {
                if (_sceneMode != SceneMode::Matrix) {
                    Logger::i("[pattern] → SceneMatrix  scene=%s br=%u frameDur=%.1fs blend=%s t=%.1fs",
                              cfg.sceneId, cfg.brightness, cfg.frameDuration,
                              cfg.transitionEnabled ? "on" : "off", cfg.transitionTime);
                    _sceneMatrix.setDimensions(_width, _height);
                    _sceneMatrix.setMatrixLayout(_matrixStart, _matrixDir);
                    _sceneMatrix.begin(*_led, cfg);
                    _current   = &_sceneMatrix;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::Matrix;
                } else {
                    _sceneMatrix.applyConfig(cfg);
                }
            } else {
                if (_sceneMode != SceneMode::String) {
                    Logger::i("[pattern] → SceneString  scene=%s br=%u transition=%s t=%.1fs",
                              cfg.sceneId, cfg.brightness,
                              cfg.transitionEnabled ? "on" : "off", cfg.transitionTime);
                    _sceneString.setNumLeds(_width);
                    _sceneString.begin(*_led, cfg);
                    _current   = &_sceneString;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::String;
                } else {
                    _sceneString.applyConfig(cfg);
                }
            }
            return;
        }

        _sceneMode = SceneMode::None;

        if (_current == nullptr || cfg.pattern != _currentId) {
            static const char* const _names[] = { "Static", "Breathing", "ColorCycle", "Strobe", "Candle" };
            const char* name = (uint8_t)cfg.pattern < 5 ? _names[(uint8_t)cfg.pattern] : "?";
            Logger::i("[pattern] → %s  rgb(%u,%u,%u) br=%u spd=%.1f",
                      name, cfg.color.r, cfg.color.g, cfg.color.b, cfg.brightness, cfg.speed);
            _currentId = cfg.pattern;
            switch (cfg.pattern) {
                case PatternId::Breathing:  _current = &_breathing;  break;
                case PatternId::ColorCycle: _current = &_colorCycle; break;
                case PatternId::Strobe:     _current = &_strobe;     break;
                case PatternId::Candle:     _current = &_candle;     break;
                default:                    _current = &_static;     break;
            }
            _current->begin(*_led, cfg);
        } else {
            _current->applyConfig(cfg);
        }
    }

    void tick() {
        if (_testActive) {
            if (millis() - _testStart >= _testDuration) {
                _testActive = false;
                applyConfig(_savedConfig);
            }
            return;
        }
        if (_current) _current->tick(millis());
    }

    void notifySceneUpdated(const char* sceneId) {
        if (_sceneMode == SceneMode::Matrix)
            _sceneMatrix.reloadIfCurrent(sceneId);
        else if (_sceneMode == SceneMode::String)
            _sceneString.reloadIfCurrent(sceneId);
    }

    float getPhase()        const { return _current ? _current->getPhase()    : 0.0f; }
    void  snapPhase(float p)      { if (_current) _current->snapPhase(p); }
    void  resetPhase()            { if (_current) _current->resetPhase(); }

private:
    enum class SceneMode { None, Matrix, String };

    // ── state ────────────────────────────────────────────────────────────────
    LedDriver*  _led       = nullptr;
    Pattern*    _current   = nullptr;
    PatternId   _currentId = PatternId::Static;
    SceneMode   _sceneMode = SceneMode::None;
    uint16_t        _width       = 1;
    uint16_t        _height      = 1;
    MatrixStart     _matrixStart = MatrixStart::TopLeft;
    MatrixDirection _matrixDir   = MatrixDirection::Horizontal;
    bool            _wrapWidth   = false;
    bool            _wrapHeight  = false;

    LightConfig  _savedConfig;
    bool         _testActive   = false;
    uint32_t     _testStart    = 0;
    uint32_t     _testDuration = 0;

    // ── pattern instances ─────────────────────────────────────────────────────
    StaticColor   _static;
    Breathing     _breathing;
    ColorCycle    _colorCycle;
    Strobe        _strobe;
    CandlePattern _candle;
    SceneMatrix   _sceneMatrix;
    SceneString   _sceneString;
    Proximity     _proximity;

    void _renderTest() {
        uint16_t n    = _width * _height;
        uint16_t show = (_width < _height) ? _width : _height;
        for (uint16_t i = 0; i < n; i++) {
            if      (i == 0)    _led->setPixel(i, 255, 0,   0);
            else if (i < show)  _led->setPixel(i, 0,   200, 0);
            else                _led->setPixel(i, 0,   0,   0);
        }
        _led->show();
    }
};
