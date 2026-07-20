#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>

#include <vector>

#include "../scenes/SceneManager.h"
#include "MatrixLayout.h"
#include "Pattern.h"

// Renders a scene as a pixel image on a matrix light.
// The scene's own w/h can differ from the light's physical w/h: it is
// stretched (nearest-neighbor, aspect ratio ignored) to fill the light exactly.
// Supports multi-frame animation with optional blending between frames.
class SceneMatrix : public Pattern {
   public:
    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _layout.setDimensions(w, h); }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _layout.setWiring(start, dir, serpentine);
    }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _load(cfg.sceneId);
        _frameIdx = 0;
        _prevFrameIdx = 0;
        _blending = false;
        _frameStartMs = millis();
        _render(_frameIdx, _frameIdx, 0.0f);
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        _cfg = cfg;
        if (sceneChanged) {
            _load(cfg.sceneId);
            _frameIdx = 0;
            _prevFrameIdx = 0;
            _blending = false;
            _frameStartMs = millis();
            _render(_frameIdx, _frameIdx, 0.0f);
        } else {
            // Re-render the current frame so config-only changes (e.g. brightness)
            // take effect immediately instead of waiting for the next frame advance.
            _renderCurrentBlendState(millis());
        }
    }

    // Re-read the scene file if it matches the currently loaded scene.
    // Call this when a scene file is written so in-memory frames stay current.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        _load(sceneId);
        _frameIdx = 0;
        _prevFrameIdx = 0;
        _blending = false;
        _frameStartMs = millis();
        _render(_frameIdx, _frameIdx, 0.0f);
    }

    void tick(uint32_t now) override {
        if (!_led || _frames.empty()) return;

        uint32_t holdMs = _holdMs();
        uint32_t blendMs = _blendMs();

        if (_blending) {
            uint32_t elapsed = now - _blendStartMs;
            if (elapsed >= blendMs) {
                _blending = false;
                _frameStartMs = now;
                _render(_frameIdx, _frameIdx, 0.0f);
            } else {
                _renderCurrentBlendState(now);
            }
            return;
        }

        if (_frames.size() > 1 && holdMs > 0 && (now - _frameStartMs) >= holdMs) {
            _prevFrameIdx = _frameIdx;
            _frameIdx = (_frameIdx + 1) % (uint8_t)_frames.size();
            if (blendMs > 0) {
                _blending = true;
                _blendStartMs = now;
                _render(_prevFrameIdx, _frameIdx, 0.0f);
            } else {
                _frameStartMs = now;
                _render(_frameIdx, _frameIdx, 0.0f);
            }
        }
    }

   private:
    MatrixLayout _layout;
    uint16_t _sceneW = 0, _sceneH = 0;
    std::vector<std::vector<Color>> _frames;
    uint8_t _frameIdx = 0, _prevFrameIdx = 0;
    bool _blending = false;
    uint32_t _frameStartMs = 0, _blendStartMs = 0;

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

    void _load(const char* sceneId) {
        SceneManager::loadFrames(sceneId, _frames, _sceneW, _sceneH);
    }

    // Render the frame(s) that should currently be on screen, given the
    // in-progress blend state (if any), without advancing that state.
    void _renderCurrentBlendState(uint32_t now) {
        if (!_blending) {
            _render(_frameIdx, _frameIdx, 0.0f);
            return;
        }
        uint32_t blendMs = _blendMs();
        uint32_t elapsed = now - _blendStartMs;
        if (blendMs == 0 || elapsed >= blendMs) {
            _render(_frameIdx, _frameIdx, 0.0f);
            return;
        }
        float t = (float)elapsed / (float)blendMs;
        float s = t * t * (3.0f - 2.0f * t);  // smoothstep
        _render(_prevFrameIdx, _frameIdx, s);
    }

    // Render a blend between frameA (t=0) and frameB (t=1).
    // Pass same index for both to render a single frame.
    void _render(uint8_t frameA, uint8_t frameB, float t) {
        if (_frames.empty() || _sceneW == 0 || _sceneH == 0) return;
        const auto& fa = _frames[frameA < _frames.size() ? frameA : 0];
        const auto& fb = _frames[frameB < _frames.size() ? frameB : 0];
        bool blend = (t > 0.0f && frameA != frameB);

        for (uint16_t row = 0; row < _layout.height(); row++) {
            uint16_t srcRow = MatrixLayout::nearest(row, _layout.height(), _sceneH);
            for (uint16_t col = 0; col < _layout.width(); col++) {
                uint16_t srcCol = MatrixLayout::nearest(col, _layout.width(), _sceneW);
                uint16_t si = srcRow * _sceneW + srcCol;
                uint16_t li = _layout.ledIndex(row, col);
                if (si >= fa.size()) {
                    _led->setPixel(li, 0, 0, 0);
                    continue;
                }
                Color ca = fa[si];
                Color out = ca;
                if (blend && si < fb.size()) {
                    Color cb = fb[si];
                    out = {Color::lerp(ca.r, cb.r, t), Color::lerp(ca.g, cb.g, t),
                           Color::lerp(ca.b, cb.b, t)};
                }
                _led->setPixel(li, applyBrightness(out.r), applyBrightness(out.g),
                               applyBrightness(out.b));
            }
        }
        _led->show();
    }
};
