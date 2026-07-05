#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <math.h>
#include "../config/Config.h"
#include "../scenes/SceneManager.h"

// Shared helpers for gradient-mode patterns: extracting an ordered color
// palette from a scene, and sampling a smooth ramp across that palette.
namespace GradientCommon {

// Distinct colors from a scene's first frame, in first-seen order.
inline void loadPalette(const char* sceneId, std::vector<Color>& out) {
    out.clear();
    if (!sceneId || !sceneId[0]) return;
    File f = LittleFS.open(SceneManager::path(sceneId).c_str(), "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    JsonArray frames = doc["frames"].as<JsonArray>();
    if (!frames || !frames.size()) return;
    JsonArray frame0 = frames[0];
    for (JsonVariant v : frame0) {
        const char* hex = v | "";
        if (strlen(hex) < 6) continue;
        unsigned long rgb = strtoul(hex, nullptr, 16);
        Color c{(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};
        bool dup = false;
        for (const auto& p : out) {
            if (p.r == c.r && p.g == c.g && p.b == c.b) { dup = true; break; }
        }
        if (!dup) out.push_back(c);
    }
}

// Sample a color at continuous position x along [0, length).
// circular=true loops the palette seamlessly back to its first stop
// (ring topology). When not circular, the outermost stops are inset by
// half a stop-spacing rather than pinned to the physical ends, and the
// ramp mirrors back on itself beyond them — each end folds back toward
// its neighboring stop instead of holding a flat block of solid color.
inline Color sample(const std::vector<Color>& palette, float x, float length, bool circular) {
    size_t n = palette.size();
    if (n == 0) return Color{0, 0, 0};
    if (n == 1 || length <= 0) return palette[0];

    float seg;
    size_t i0, i1;
    if (circular) {
        float xm = fmodf(x, length);
        if (xm < 0) xm += length;
        seg = xm / length * (float)n;
        i0  = (size_t)seg % n;
        i1  = (i0 + 1) % n;
    } else {
        float spacing = length / (float)n;
        float lo       = spacing * 0.5f;
        float span      = length - spacing;
        float period    = 2.0f * span;
        float r = fmodf(x - lo, period);
        if (r < 0) r += period;
        if (r > span) r = period - r;
        seg = r / span * (float)(n - 1);
        i0  = (size_t)seg;
        if (i0 > n - 2) i0 = n - 2;
        i1  = i0 + 1;
    }
    float t = seg - floorf(seg);
    const Color& a = palette[i0];
    const Color& b = palette[i1];
    return Color{
        (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t),
        (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t),
        (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t),
    };
}

// How many of a scene's distinct colors to actually use as gradient stops:
// a slice of the strip's LED count (so longer strips get more stops), but
// never invented beyond how many genuinely distinct colors the scene has,
// and never fewer than 2 (a gradient needs at least two ends) unless the
// scene itself has fewer than that.
// stopCountOverride, if >= 2, replaces the auto-calculated count outright
// (still clamped to the scene's palette size below).
inline size_t targetStopCount(size_t numLeds, size_t paletteSize, uint8_t stopCountOverride = 0) {
    if (paletteSize < 2) return paletteSize;
    size_t target;
    if (stopCountOverride >= 2) {
        target = stopCountOverride;
    } else {
        const float  kFraction = 0.10f;
        const size_t kMin      = 2;
        const size_t kMax      = 8;
        target = (size_t)roundf((float)numLeds * kFraction);
        if (target < kMin) target = kMin;
        if (target > kMax) target = kMax;
    }
    if (target > paletteSize) target = paletteSize;
    return target;
}

inline int colorDistSq(const Color& a, const Color& b) {
    int dr = (int)a.r - (int)b.r;
    int dg = (int)a.g - (int)b.g;
    int db = (int)a.b - (int)b.b;
    return dr * dr + dg * dg + db * db;
}

// Reduce a scene's full distinct-color palette down to targetStopCount()
// stops, picked so the survivors are as different from each other as
// possible (greedy farthest-point sampling by RGB distance), then returned
// in their original first-seen order so the ramp still reads the way the
// scene laid its colors out.
inline void reduceToStops(const std::vector<Color>& full, size_t numLeds, std::vector<Color>& out,
                           uint8_t stopCountOverride = 0) {
    size_t target = targetStopCount(numLeds, full.size(), stopCountOverride);
    size_t n = full.size();
    if (target >= n) { out = full; return; }

    std::vector<bool> picked(n, false);
    std::vector<int>  minDist(n, 0);
    picked[0] = true;
    for (size_t i = 0; i < n; i++) minDist[i] = colorDistSq(full[i], full[0]);

    for (size_t k = 1; k < target; k++) {
        size_t best = n;
        int bestDist = -1;
        for (size_t i = 0; i < n; i++) {
            if (!picked[i] && minDist[i] > bestDist) { bestDist = minDist[i]; best = i; }
        }
        if (best == n) break;
        picked[best] = true;
        for (size_t i = 0; i < n; i++) {
            if (picked[i]) continue;
            int d = colorDistSq(full[i], full[best]);
            if (d < minDist[i]) minDist[i] = d;
        }
    }

    out.clear();
    for (size_t i = 0; i < n; i++) if (picked[i]) out.push_back(full[i]);
}

// Animates a sparse, ever-changing subset of cells away from their base
// gradient color to another palette color and back, so the light reads as
// a shimmering variation on the gradient rather than a static ramp.
// Cells not currently morphing simply show their base color.
class Morph {
public:
    // out.size() must equal base.size(); called every tick.
    void tick(uint32_t now, const std::vector<Color>& base, const std::vector<Color>& palette,
              float speed, std::vector<Color>& out) {
        size_t n = base.size();
        if (_cells.size() != n) _cells.assign(n, Cell{});
        out.resize(n);
        float spd = speed > 0.05f ? speed : 0.05f;

        if (palette.size() > 1 && now >= _nextActivationMs) {
            size_t activeCount = 0;
            for (const auto& c : _cells) if (c.phase != Idle) activeCount++;
            size_t target = _jitteredTargetCount(n);
            if (activeCount < target) _activateRandomIdle(now, base, palette, spd);
            _nextActivationMs = now + 200 + (uint32_t)random(0, 400);
        }

        for (size_t i = 0; i < n; i++) {
            Cell& c = _cells[i];
            if (c.phase == Idle) { out[i] = base[i]; continue; }

            uint32_t elapsed = now - c.startMs;
            if (elapsed >= c.durationMs) {
                switch (c.phase) {
                    case ToTarget:
                        c.phase      = Hold;
                        c.startMs    = now;
                        c.durationMs = _jitterMs(800, 2500, 1.0f);
                        break;
                    case Hold:
                        c.phase      = ToBase;
                        c.from       = c.to;
                        c.to         = base[i];
                        c.startMs    = now;
                        c.durationMs = _jitterMs(1500, 4000, spd);
                        break;
                    default:  // ToBase
                        c.phase = Idle;
                        break;
                }
                elapsed = 0;
            }

            if (c.phase == Idle)      out[i] = base[i];
            else if (c.phase == Hold) out[i] = c.to;
            else {
                float t = c.durationMs > 0 ? (float)elapsed / (float)c.durationMs : 1.0f;
                if (t > 1.0f) t = 1.0f;
                float s = t * t * (3.0f - 2.0f * t);
                out[i] = Color{
                    (uint8_t)((float)c.from.r + ((float)c.to.r - (float)c.from.r) * s),
                    (uint8_t)((float)c.from.g + ((float)c.to.g - (float)c.from.g) * s),
                    (uint8_t)((float)c.from.b + ((float)c.to.b - (float)c.from.b) * s),
                };
            }
        }
    }

    void reset() { _cells.clear(); _nextActivationMs = 0; }

private:
    enum Phase : uint8_t { Idle, ToTarget, Hold, ToBase };
    struct Cell {
        Phase    phase      = Idle;
        Color    from, to;
        uint32_t startMs    = 0;
        uint32_t durationMs = 0;
    };

    std::vector<Cell> _cells;
    uint32_t          _nextActivationMs = 0;

    static uint32_t _jitterMs(uint32_t lo, uint32_t hi, float speed) {
        uint32_t base = lo + (uint32_t)random(0, hi - lo + 1);
        return (uint32_t)((float)base / speed);
    }

    // ~12% of cells, jittered ±40%, recomputed on every activation check
    // so the pool size drifts organically over time.
    static size_t _jitteredTargetCount(size_t n) {
        float base   = (float)n * 0.12f;
        if (base < 1.0f) base = 1.0f;
        float jitter = 0.6f + ((float)random(0, 801) / 1000.0f);  // 0.6x .. 1.4x
        size_t t = (size_t)(base * jitter);
        return t < 1 ? 1 : t;
    }

    void _activateRandomIdle(uint32_t now, const std::vector<Color>& base,
                              const std::vector<Color>& palette, float speed) {
        size_t n = _cells.size();
        size_t start = random(0, n);
        for (size_t k = 0; k < n; k++) {
            size_t i = (start + k) % n;
            if (_cells[i].phase != Idle) continue;
            Cell& c      = _cells[i];
            c.phase      = ToTarget;
            c.from       = base[i];
            c.to         = palette[random(0, palette.size())];
            c.startMs    = now;
            c.durationMs = _jitterMs(1500, 4000, speed);
            return;
        }
    }
};

}  // namespace GradientCommon
