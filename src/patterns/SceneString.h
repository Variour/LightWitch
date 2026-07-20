#pragma once
#include <vector>

#include "../scenes/SceneManager.h"
#include "GradientCommon.h"
#include "MatrixLayout.h"
#include "Pattern.h"

// Renders a scene onto a string light, per LightConfig::sceneStringMode:
// - PerLed: each LED independently picks its own random color from the
//   scene's (undeduplicated, all-frames) palette.
// - WholeString: the whole string shares one random palette color.
// With transitionEnabled, PerLed/WholeString colors cycle through the
// palette over time — transitionTime controls both the hold duration and
// the fade duration (total cycle = 2 x transitionTime).
// - OneToOne: the scene image is flattened row-major and stretched
//   (nearest-neighbor) onto the string, the same way SceneMatrix maps a
//   scene onto a matrix light. frameDuration/transitionEnabled/
//   transitionTime control frame hold/blend timing, same as SceneMatrix.
class SceneString : public Pattern {
   public:
    float getPeriod() const override { return 0.0f; }

    void setNumLeds(uint16_t n) { _numLeds = n; }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _load(cfg.sceneId);
        _resetFrameState();
        _initLeds(millis());
        _renderCurrent(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        bool modeChanged = cfg.sceneStringMode != _cfg.sceneStringMode;
        bool transChanged = cfg.transitionEnabled != _cfg.transitionEnabled;
        _cfg = cfg;
        if (sceneChanged || modeChanged) {
            _load(cfg.sceneId);
            _resetFrameState();
            _initLeds(millis());
            _renderCurrent(millis());
        } else if (_cfg.sceneStringMode == SceneStringMode::OneToOne) {
            // Re-render the current frame so config-only changes (e.g. brightness)
            // take effect immediately instead of waiting for the next frame advance.
            _renderOneToOneBlendState(millis());
        } else if (transChanged) {
            _initLeds(millis());
        }
    }

    // Re-read the scene data if it matches the currently loaded scene.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        _load(sceneId);
        _resetFrameState();
        _initLeds(millis());
        _renderCurrent(millis());
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        if (_cfg.sceneStringMode == SceneStringMode::OneToOne) {
            _tickOneToOne(now);
            return;
        }
        if (_cfg.transitionEnabled && !_states.empty()) {
            uint32_t cycleMs = _cycleMs();
            if (_cfg.sceneStringMode == SceneStringMode::WholeString) {
                auto& s = _states[0];
                uint32_t elapsed = now - s.startMs;
                if (elapsed >= cycleMs * 2) {
                    s.from = s.to;
                    s.to = _pick();
                    s.startMs = now;
                    _copyStateToAll(s);
                }
            } else {
                for (auto& s : _states) {
                    uint32_t elapsed = now - s.startMs;
                    if (elapsed >= cycleMs * 2) {
                        s.from = s.to;
                        s.to = _pick();
                        s.startMs = now;
                    }
                }
            }
        }
        _renderAll(now);
    }

   private:
    struct LedState {
        Color from, to;
        uint32_t startMs;
    };

    uint16_t _numLeds = 1;
    std::vector<Color> _palette;
    std::vector<LedState> _states;

    // OneToOne mode: spatial scene frames, mirroring SceneMatrix.
    std::vector<std::vector<Color>> _frames;
    uint16_t _sceneW = 0, _sceneH = 0;
    uint8_t _frameIdx = 0, _prevFrameIdx = 0;
    bool _blending = false;
    uint32_t _frameStartMs = 0, _blendStartMs = 0;

    uint32_t _cycleMs() const {
        float t = _cfg.transitionTime;
        if (!isfinite(t) || t < 0.1f) t = 2.0f;
        return (uint32_t)(t * 1000.0f);
    }

    uint32_t _holdMs() const {
        float d = _cfg.frameDuration;
        if (!isfinite(d) || d < 0.0f) d = 1.0f;
        return (uint32_t)(d * 1000.0f);
    }

    uint32_t _blendMs() const {
        if (!_cfg.transitionEnabled) return 0;
        float t = _cfg.transitionTime;
        if (!isfinite(t) || t < 0.0f) t = 0.5f;
        return (uint32_t)(t * 1000.0f);
    }

    // Loads whatever scene data the active mode needs: OneToOne needs the
    // scene's spatial frame data (like SceneMatrix), PerLed/WholeString only
    // need the flattened, undeduplicated color palette.
    void _load(const char* sceneId) {
        if (_cfg.sceneStringMode == SceneStringMode::OneToOne) {
            _palette.clear();
            SceneManager::loadFrames(sceneId, _frames, _sceneW, _sceneH);
        } else {
            _frames.clear();
            _sceneW = _sceneH = 0;
            _loadPalette(sceneId);
        }
    }

    // All colors from every frame, in order and undeduplicated (unlike the
    // gradient patterns' first-frame, distinct-colors-only palette).
    void _loadPalette(const char* sceneId) {
        GradientCommon::loadPalette(sceneId, _palette, /*allFrames=*/true, /*dedupe=*/false);
    }

    void _resetFrameState() {
        _frameIdx = 0;
        _prevFrameIdx = 0;
        _blending = false;
        _frameStartMs = millis();
    }

    void _renderCurrent(uint32_t now) {
        if (_cfg.sceneStringMode == SceneStringMode::OneToOne) {
            _renderOneToOne(_frameIdx, _frameIdx, 0.0f);
        } else {
            _renderAll(now);
        }
    }

    void _initLeds(uint32_t now) {
        _states.clear();
        if (_cfg.sceneStringMode == SceneStringMode::OneToOne) return;
        if (_palette.empty()) return;
        uint32_t cycleMs = _cycleMs();
        randomSeed(micros() ^ (uint32_t)millis() ^ esp_random());
        _states.reserve(_numLeds);

        if (_cfg.sceneStringMode == SceneStringMode::WholeString) {
            LedState s;
            s.from = _pick();
            s.to = _pick();
            s.startMs = _cfg.transitionEnabled ? now - (uint32_t)random(0, cycleMs * 2) : now;
            _states.assign(_numLeds, s);
            return;
        }

        for (uint16_t i = 0; i < _numLeds; i++) {
            LedState s;
            s.from = _pick();
            s.to = _pick();
            // Stagger start times across 2× the cycle so LEDs are fully offset
            s.startMs = now - (uint32_t)random(0, cycleMs * 2);
            _states.push_back(s);
        }
    }

    void _copyStateToAll(const LedState& state) {
        for (auto& s : _states) s = state;
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
                    r = applyBrightness(Color::lerp(s.from.r, s.to.r, smooth));
                    g = applyBrightness(Color::lerp(s.from.g, s.to.g, smooth));
                    b = applyBrightness(Color::lerp(s.from.b, s.to.b, smooth));
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

    void _tickOneToOne(uint32_t now) {
        if (_frames.empty()) return;
        uint32_t holdMs = _holdMs();
        uint32_t blendMs = _blendMs();

        if (_blending) {
            uint32_t elapsed = now - _blendStartMs;
            if (elapsed >= blendMs) {
                _blending = false;
                _frameStartMs = now;
                _renderOneToOne(_frameIdx, _frameIdx, 0.0f);
            } else {
                _renderOneToOneBlendState(now);
            }
            return;
        }

        if (_frames.size() > 1 && holdMs > 0 && (now - _frameStartMs) >= holdMs) {
            _prevFrameIdx = _frameIdx;
            _frameIdx = (_frameIdx + 1) % (uint8_t)_frames.size();
            if (blendMs > 0) {
                _blending = true;
                _blendStartMs = now;
                _renderOneToOne(_prevFrameIdx, _frameIdx, 0.0f);
            } else {
                _frameStartMs = now;
                _renderOneToOne(_frameIdx, _frameIdx, 0.0f);
            }
        }
    }

    // Render the frame(s) that should currently be on screen, given the
    // in-progress blend state (if any), without advancing that state.
    void _renderOneToOneBlendState(uint32_t now) {
        if (!_blending) {
            _renderOneToOne(_frameIdx, _frameIdx, 0.0f);
            return;
        }
        uint32_t blendMs = _blendMs();
        uint32_t elapsed = now - _blendStartMs;
        if (blendMs == 0 || elapsed >= blendMs) {
            _renderOneToOne(_frameIdx, _frameIdx, 0.0f);
            return;
        }
        float t = (float)elapsed / (float)blendMs;
        float s = t * t * (3.0f - 2.0f * t);  // smoothstep
        _renderOneToOne(_prevFrameIdx, _frameIdx, s);
    }

    // Render a blend between frameA (t=0) and frameB (t=1) onto the string.
    // Pass same index for both to render a single frame. The scene's w*h
    // pixels (row-major) are flattened into one sequence and nearest-
    // neighbor stretched/compressed onto the string's LED count.
    void _renderOneToOne(uint8_t frameA, uint8_t frameB, float t) {
        if (_frames.empty() || _sceneW == 0 || _sceneH == 0) return;
        const auto& fa = _frames[frameA < _frames.size() ? frameA : 0];
        const auto& fb = _frames[frameB < _frames.size() ? frameB : 0];
        bool blend = (t > 0.0f && frameA != frameB);
        uint16_t sceneLen = _sceneW * _sceneH;

        for (uint16_t i = 0; i < _numLeds; i++) {
            uint16_t si = MatrixLayout::nearest(i, _numLeds, sceneLen);
            if (si >= fa.size()) {
                _led->setPixel(i, 0, 0, 0);
                continue;
            }
            Color ca = fa[si];
            Color out = ca;
            if (blend && si < fb.size()) {
                Color cb = fb[si];
                out = {Color::lerp(ca.r, cb.r, t), Color::lerp(ca.g, cb.g, t),
                       Color::lerp(ca.b, cb.b, t)};
            }
            _led->setPixel(i, applyBrightness(out.r), applyBrightness(out.g),
                           applyBrightness(out.b));
        }
        _led->show();
    }
};
