#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "../config/Config.h"
#include "../led/LedDriver.h"
#include "../logging/Logger.h"
#include "../mesh/PeerRegistry.h"
#include "../scenes/SceneManager.h"
#include "Breathing.h"
#include "Candle.h"
#include "ColorCycle.h"
#include "GradientMatrix.h"
#include "GradientString.h"
#include "Pattern.h"
#include "Proximity.h"
#include "SceneMatrix.h"
#include "SceneString.h"
#include "StaticColor.h"
#include "Strobe.h"
#include "TextMatrix.h"
#include "TimeMatrix.h"

// Owns the active pattern instance and switches patterns on config change.
class PatternRunner {
   public:
    void begin(LedDriver& led) { _led = &led; }

    void setDimensions(uint16_t w, uint16_t h) {
        _width = w;
        _height = h;
    }

    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _matrixStart = start;
        _matrixDir = dir;
        _matrixSerpentine = serpentine;
        _sceneMatrix.setMatrixLayout(start, dir, serpentine);
    }

    // Topology: whether the last LED on an axis wraps back to the first (ring/cylinder/torus).
    // Not yet consumed by any pattern — stored here so position-aware modes can query it.
    void setWrap(bool wrapWidth, bool wrapHeight) {
        _wrapWidth = wrapWidth;
        _wrapHeight = wrapHeight;
        _gradientString.setWrap(wrapWidth);
        _gradientMatrix.setWrap(wrapWidth);
    }

    bool wrapWidth() const { return _wrapWidth; }
    bool wrapHeight() const { return _wrapHeight; }

    void setPeerRegistry(PeerRegistry* peers) { _proximity.setPeers(peers); }
    void setGroupId(uint8_t groupId) { _proximity.setGroupId(groupId); }

    // Matrix-only: marks pixel 0 red and the first row/column green, so the
    // user can visually confirm matrixStart/matrixDir/matrixSerpentine wiring.
    void showTest(uint32_t durationMs) {
        if (!_led || _height < 2) return;
        _testStart = millis();
        _testDuration = durationMs;
        _testMode = TestMode::Orientation;
        _testActive = true;
        _renderTest();
    }

    // Cycles the whole light through solid red, green, blue (stepMs each) so
    // the user can visually confirm colorOrder matches the strip's physical
    // wiring. Unlike showTest() (orientation), this applies to every light
    // shape — string or matrix.
    void showColorOrderTest(uint32_t stepMs = 1000) {
        if (!_led) return;
        _testStart = millis();
        _testStepMs = stepMs;
        _testDuration = stepMs * 3;
        _testMode = TestMode::ColorOrder;
        _testPhase = 0xFF;  // forces the first tick() to render immediately
        _testActive = true;
    }

    // Renders firmware-update progress as an orange fill proportional to percent
    // (0-100), overriding whatever pattern is currently configured. Called
    // directly from the main loop while an update is in progress.
    void showUpdateProgress(uint8_t percent) {
        if (!_led) return;
        uint16_t n = _width * _height;
        uint16_t lit = (uint16_t)((uint32_t)n * min(percent, (uint8_t)100) / 100);
        for (uint16_t i = 0; i < n; i++) {
            if (i < lit)
                _led->setPixel(i, 255, 80, 0);
            else
                _led->setPixel(i, 0, 0, 0);
        }
        _led->show();
    }

    // Solid green — shown briefly once the update has finished, just before reboot.
    void showUpdateDone() {
        if (!_led) return;
        uint16_t n = _width * _height;
        for (uint16_t i = 0; i < n; i++) _led->setPixel(i, 0, 200, 0);
        _led->show();
    }

    // Thread-safe entry point. Callers include the web server's request
    // handler and the mesh ESP-NOW receive callback, both of which run on
    // FreeRTOS tasks other than the Arduino loop() task — and neither is
    // pinned to the same core loop() runs on. Actually mutating _current /
    // _sceneMode or touching a pattern's internal buffers must only ever
    // happen on the same thread that renders every frame (tick(), below),
    // otherwise a pattern's own continuous tick()-driven show() call can
    // race with begin()'s show() call here and silently win, leaving the
    // strip stuck showing the old pattern. So this only stashes the config;
    // tick() picks it up and applies it before rendering the next frame.
    void applyConfig(const LightConfig& cfg) {
        portENTER_CRITICAL(&_mux);
        _pendingConfig = cfg;
        _hasPendingConfig = true;
        portEXIT_CRITICAL(&_mux);
    }

    void tick() {
        LightConfig pending;
        bool hasPending;
        portENTER_CRITICAL(&_mux);
        hasPending = _hasPendingConfig;
        if (hasPending) {
            pending = _pendingConfig;
            _hasPendingConfig = false;
        }
        portEXIT_CRITICAL(&_mux);
        if (hasPending) _applyConfig(pending);

        if (_testActive) {
            uint32_t elapsed = millis() - _testStart;
            if (elapsed >= _testDuration) {
                _testActive = false;
                _testMode = TestMode::None;
                _applyConfig(_savedConfig);
                return;
            }
            if (_testMode == TestMode::ColorOrder) {
                uint8_t phase = (uint8_t)(elapsed / _testStepMs);
                if (phase != _testPhase) {
                    _testPhase = phase;
                    _renderColorOrderTest(phase);
                }
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
        else if (_sceneMode == SceneMode::GradientMatrix)
            _gradientMatrix.reloadIfCurrent(sceneId);
        else if (_sceneMode == SceneMode::GradientString)
            _gradientString.reloadIfCurrent(sceneId);
    }

    float getPhase() const { return _current ? _current->getPhase() : 0.0f; }
    void snapPhase(float p) {
        if (_current) _current->snapPhase(p);
    }
    void resetPhase() {
        if (_current) _current->resetPhase();
    }

   private:
    void _applyConfig(const LightConfig& cfg) {
        _savedConfig = cfg;
        if (_testActive) return;
        if (cfg.mode == GroupMode::Proximity) {
            _sceneMode = SceneMode::None;
            if (_current != &_proximity) {
                Logger::i("[pattern] → Proximity  rgb(%u,%u,%u) br=%u scale=%.1f", cfg.color.r,
                          cfg.color.g, cfg.color.b, cfg.brightness, cfg.proximityScale);
                _proximity.begin(*_led, cfg);
                _current = &_proximity;
                _currentId = (PatternId)0xFF;
            } else {
                _proximity.applyConfig(cfg);
            }
            return;
        }

        if (cfg.mode == GroupMode::Scene) {
            if (_height > 1) {
                if (_sceneMode != SceneMode::Matrix) {
                    Logger::i(
                        "[pattern] → SceneMatrix  scene=%s br=%u frameDur=%.1fs blend=%s t=%.1fs",
                        cfg.sceneId, cfg.brightness, cfg.frameDuration,
                        cfg.transitionEnabled ? "on" : "off", cfg.transitionTime);
                    _sceneMatrix.setDimensions(_width, _height);
                    _sceneMatrix.setMatrixLayout(_matrixStart, _matrixDir, _matrixSerpentine);
                    _sceneMatrix.begin(*_led, cfg);
                    _current = &_sceneMatrix;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::Matrix;
                } else {
                    _sceneMatrix.applyConfig(cfg);
                }
            } else {
                if (_sceneMode != SceneMode::String) {
                    Logger::i("[pattern] → SceneString  scene=%s br=%u transition=%s t=%.1fs",
                              cfg.sceneId, cfg.brightness, cfg.transitionEnabled ? "on" : "off",
                              cfg.transitionTime);
                    _sceneString.setNumLeds(_width);
                    _sceneString.begin(*_led, cfg);
                    _current = &_sceneString;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::String;
                } else {
                    _sceneString.applyConfig(cfg);
                }
            }
            return;
        }

        if (cfg.mode == GroupMode::Time) {
            if (_sceneMode != SceneMode::Time) {
                Logger::i("[pattern] → Time  24h=%s br=%u", cfg.time24h ? "on" : "off",
                          cfg.brightness);
                _time.setDimensions(_width, _height);
                _time.setMatrixLayout(_matrixStart, _matrixDir, _matrixSerpentine);
                _time.begin(*_led, cfg);
                _current = &_time;
                _currentId = (PatternId)0xFF;
                _sceneMode = SceneMode::Time;
            } else {
                _time.applyConfig(cfg);
            }
            return;
        }

        if (cfg.mode == GroupMode::Gradient) {
            if (_height > 1) {
                if (_sceneMode != SceneMode::GradientMatrix) {
                    Logger::i("[pattern] → GradientMatrix  scene=%s br=%u morph=%s spd=%.1f",
                              cfg.sceneId, cfg.brightness, cfg.morphEnabled ? "on" : "off",
                              cfg.speed);
                    _gradientMatrix.setDimensions(_width, _height);
                    _gradientMatrix.setMatrixLayout(_matrixStart, _matrixDir, _matrixSerpentine);
                    _gradientMatrix.setWrap(_wrapWidth);
                    _gradientMatrix.begin(*_led, cfg);
                    _current = &_gradientMatrix;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::GradientMatrix;
                } else {
                    _gradientMatrix.applyConfig(cfg);
                }
            } else {
                if (_sceneMode != SceneMode::GradientString) {
                    Logger::i("[pattern] → GradientString  scene=%s br=%u morph=%s spd=%.1f",
                              cfg.sceneId, cfg.brightness, cfg.morphEnabled ? "on" : "off",
                              cfg.speed);
                    _gradientString.setNumLeds(_width);
                    _gradientString.setWrap(_wrapWidth);
                    _gradientString.begin(*_led, cfg);
                    _current = &_gradientString;
                    _currentId = (PatternId)0xFF;
                    _sceneMode = SceneMode::GradientString;
                } else {
                    _gradientString.applyConfig(cfg);
                }
            }
            return;
        }

        if (cfg.mode == GroupMode::Text && _height > 1) {
            _sceneMode = SceneMode::None;
            if (_current != &_textMatrix) {
                Logger::i("[pattern] → TextMatrix  \"%s\" rgb(%u,%u,%u) br=%u anim=%s spd=%.1f",
                          cfg.text, cfg.color.r, cfg.color.g, cfg.color.b, cfg.brightness,
                          cfg.textAnimation == TextAnimation::Bounce ? "bounce" : "scroll",
                          cfg.speed);
                _textMatrix.setDimensions(_width, _height);
                _textMatrix.setMatrixLayout(_matrixStart, _matrixDir, _matrixSerpentine);
                _textMatrix.begin(*_led, cfg);
                _current = &_textMatrix;
                _currentId = (PatternId)0xFF;
            } else {
                _textMatrix.applyConfig(cfg);
            }
            return;
        }

        _sceneMode = SceneMode::None;

        // GroupMode::Text on a string light (height == 1) has no supported
        // rendering — falls through to the Pattern-mode switch below and
        // just shows whatever cfg.pattern last held (defaults to Static).
        if (_current == nullptr || cfg.pattern != _currentId) {
            static const char* const _names[] = {"Static", "Breathing", "ColorCycle", "Strobe",
                                                 "Candle"};
            const char* name = (uint8_t)cfg.pattern < 5 ? _names[(uint8_t)cfg.pattern] : "?";
            Logger::i("[pattern] → %s  rgb(%u,%u,%u) br=%u spd=%.1f", name, cfg.color.r,
                      cfg.color.g, cfg.color.b, cfg.brightness, cfg.speed);
            _currentId = cfg.pattern;
            switch (cfg.pattern) {
                case PatternId::Breathing:
                    _current = &_breathing;
                    break;
                case PatternId::ColorCycle:
                    _current = &_colorCycle;
                    break;
                case PatternId::Strobe:
                    _current = &_strobe;
                    break;
                case PatternId::Candle:
                    _current = &_candle;
                    break;
                default:
                    _current = &_static;
                    break;
            }
            _current->begin(*_led, cfg);
        } else {
            _current->applyConfig(cfg);
        }
    }

    enum class SceneMode { None, Matrix, String, GradientMatrix, GradientString, Time };

    // ── state ────────────────────────────────────────────────────────────────
    LedDriver* _led = nullptr;
    Pattern* _current = nullptr;
    PatternId _currentId = PatternId::Static;
    SceneMode _sceneMode = SceneMode::None;
    uint16_t _width = 1;
    uint16_t _height = 1;
    MatrixStart _matrixStart = MatrixStart::TopLeft;
    MatrixDirection _matrixDir = MatrixDirection::Horizontal;
    bool _matrixSerpentine = false;
    bool _wrapWidth = false;
    bool _wrapHeight = false;

    enum class TestMode : uint8_t { None, Orientation, ColorOrder };

    LightConfig _savedConfig;
    bool _testActive = false;
    TestMode _testMode = TestMode::None;
    uint32_t _testStart = 0;
    uint32_t _testDuration = 0;
    uint32_t _testStepMs = 0;
    uint8_t _testPhase = 0xFF;

    // ── cross-task config handoff ───────────────────────────────────────────
    // applyConfig() may be called from the web server's request-handling
    // task or the mesh ESP-NOW receive callback, neither of which is
    // guaranteed to run on the same core as tick(). Guarded by _mux so the
    // handoff itself is safe; _applyConfig() only ever runs from tick().
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    LightConfig _pendingConfig;
    bool _hasPendingConfig = false;

    // ── pattern instances ─────────────────────────────────────────────────────
    StaticColor _static;
    Breathing _breathing;
    ColorCycle _colorCycle;
    Strobe _strobe;
    CandlePattern _candle;
    SceneMatrix _sceneMatrix;
    SceneString _sceneString;
    GradientMatrix _gradientMatrix;
    GradientString _gradientString;
    Proximity _proximity;
    TextMatrix _textMatrix;
    TimeMatrix _time;

    void _renderTest() {
        uint16_t n = _width * _height;
        uint16_t show = (_width < _height) ? _width : _height;
        for (uint16_t i = 0; i < n; i++) {
            if (i == 0)
                _led->setPixel(i, 255, 0, 0);
            else if (i < show)
                _led->setPixel(i, 0, 200, 0);
            else
                _led->setPixel(i, 0, 0, 0);
        }
        _led->show();
    }

    // phase 0 = red, 1 = green, 2 = blue — driven by ColorOrder's configured
    // permutation, so a wrong colorOrder setting shows up as the wrong colour.
    void _renderColorOrderTest(uint8_t phase) {
        uint16_t n = _width * _height;
        uint8_t r = phase == 0 ? 255 : 0;
        uint8_t g = phase == 1 ? 255 : 0;
        uint8_t b = phase == 2 ? 255 : 0;
        for (uint16_t i = 0; i < n; i++) _led->setPixel(i, r, g, b);
        _led->show();
    }
};
