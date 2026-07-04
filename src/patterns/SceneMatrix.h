#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include "Pattern.h"
#include "../scenes/SceneManager.h"

// Renders a scene as a pixel image on a matrix light.
// Supports multi-frame animation with optional blending between frames.
class SceneMatrix : public Pattern {
public:
    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _lightW = w; _lightH = h; }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir) { _matrixStart = start; _matrixDir = dir; }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        _load(cfg.sceneId);
        _frameIdx     = 0;
        _prevFrameIdx = 0;
        _blending     = false;
        _frameStartMs = millis();
        _render(_frameIdx, _frameIdx, 0.0f);
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        _cfg = cfg;
        if (sceneChanged) {
            _load(cfg.sceneId);
            _frameIdx     = 0;
            _prevFrameIdx = 0;
            _blending     = false;
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
        _frameIdx     = 0;
        _prevFrameIdx = 0;
        _blending     = false;
        _frameStartMs = millis();
        _render(_frameIdx, _frameIdx, 0.0f);
    }

    void tick(uint32_t now) override {
        if (!_led || _frames.empty()) return;

        uint32_t holdMs  = _holdMs();
        uint32_t blendMs = _blendMs();

        if (_blending) {
            uint32_t elapsed = now - _blendStartMs;
            if (elapsed >= blendMs) {
                _blending     = false;
                _frameStartMs = now;
                _render(_frameIdx, _frameIdx, 0.0f);
            } else {
                _renderCurrentBlendState(now);
            }
            return;
        }

        if (_frames.size() > 1 && holdMs > 0 && (now - _frameStartMs) >= holdMs) {
            _prevFrameIdx = _frameIdx;
            _frameIdx     = (_frameIdx + 1) % (uint8_t)_frames.size();
            if (blendMs > 0) {
                _blending     = true;
                _blendStartMs = now;
                _render(_prevFrameIdx, _frameIdx, 0.0f);
            } else {
                _frameStartMs = now;
                _render(_frameIdx, _frameIdx, 0.0f);
            }
        }
    }

private:
    uint16_t        _lightW      = 1, _lightH = 1;
    MatrixStart     _matrixStart = MatrixStart::TopLeft;
    MatrixDirection _matrixDir   = MatrixDirection::Horizontal;
    uint16_t _sceneW = 0, _sceneH = 0;
    std::vector<std::vector<Color>> _frames;
    uint8_t  _frameIdx = 0, _prevFrameIdx = 0;
    bool     _blending     = false;
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
        _frames.clear();
        _sceneW = _sceneH = 0;
        if (!sceneId || !sceneId[0]) return;
        File f = LittleFS.open(SceneManager::path(sceneId).c_str(), "r");
        if (!f) return;
        JsonDocument doc;
        if (deserializeJson(doc, f)) { f.close(); return; }
        f.close();
        _sceneW = doc["w"] | (uint16_t)0;
        _sceneH = doc["h"] | (uint16_t)0;
        JsonArray frames = doc["frames"].as<JsonArray>();
        if (!frames || !frames.size()) return;
        for (JsonArray fr : frames) {
            std::vector<Color> pixels;
            pixels.reserve(fr.size());
            for (JsonVariant v : fr) {
                const char* hex = v | "";
                if (strlen(hex) < 6) { pixels.push_back({}); continue; }
                unsigned long rgb = strtoul(hex, nullptr, 16);
                pixels.push_back({(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb});
            }
            _frames.push_back(std::move(pixels));
        }
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
        if (_frames.empty()) return;
        const auto& fa = _frames[frameA < _frames.size() ? frameA : 0];
        const auto& fb = _frames[frameB < _frames.size() ? frameB : 0];
        bool blend = (t > 0.0f && frameA != frameB);

        uint16_t renderW = min(_lightW, _sceneW);
        uint16_t renderH = min(_lightH, _sceneH);
        uint16_t total   = (uint16_t)_lightW * _lightH;

        for (uint16_t i = 0; i < total; i++) _led->setPixel(i, 0, 0, 0);

        for (uint16_t row = 0; row < renderH; row++) {
            for (uint16_t col = 0; col < renderW; col++) {
                uint16_t si = row * _sceneW + col;
                uint16_t li = _ledIndex(row, col);
                if (si >= fa.size()) continue;
                Color ca = fa[si];
                Color out = ca;
                if (blend && si < fb.size()) {
                    Color cb = fb[si];
                    out = {_lerp(ca.r, cb.r, t), _lerp(ca.g, cb.g, t), _lerp(ca.b, cb.b, t)};
                }
                _led->setPixel(li,
                    applyBrightness(out.r),
                    applyBrightness(out.g),
                    applyBrightness(out.b));
            }
        }
        _led->show();
    }

    uint16_t _ledIndex(uint16_t row, uint16_t col) const {
        uint16_t r = (_matrixStart == MatrixStart::BottomLeft || _matrixStart == MatrixStart::BottomRight)
                     ? (_lightH - 1 - row) : row;
        uint16_t c = (_matrixStart == MatrixStart::TopRight  || _matrixStart == MatrixStart::BottomRight)
                     ? (_lightW - 1 - col) : col;
        return (_matrixDir == MatrixDirection::Vertical)
               ? c * _lightH + r
               : r * _lightW + c;
    }

    static uint8_t _lerp(uint8_t a, uint8_t b, float t) {
        return (uint8_t)((float)a + ((float)b - (float)a) * t);
    }
};
