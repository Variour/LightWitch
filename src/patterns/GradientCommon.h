#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <math.h>

#include <vector>

#include "../config/Config.h"
#include "../scenes/SceneManager.h"
#include "Pattern.h"

// Shared helpers for gradient-mode patterns: extracting an ordered color
// palette from a scene, and sampling a smooth ramp across that palette.
namespace GradientCommon {

// Extract colors from a scene's frames.
// allFrames=false (default) reads only the first frame; true walks every
// frame's colors in order.
// dedupe=true (default) collapses repeats to their first-seen occurrence;
// false keeps every color, including repeats, as encountered.
inline void loadPalette(const char* sceneId, std::vector<Color>& out, bool allFrames = false,
                        bool dedupe = true) {
    out.clear();
    if (!sceneId || !sceneId[0]) return;
    File f = LittleFS.open(SceneManager::path(sceneId).c_str(), "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        return;
    }
    f.close();
    JsonArray frames = doc["frames"].as<JsonArray>();
    if (!frames || !frames.size()) return;
    size_t frameCount = allFrames ? frames.size() : 1;
    for (size_t fi = 0; fi < frameCount; fi++) {
        JsonArray frame = frames[fi];
        for (JsonVariant v : frame) {
            const char* hex = v | "";
            if (strlen(hex) < 6) continue;
            unsigned long rgb = strtoul(hex, nullptr, 16);
            Color c{(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};
            if (dedupe) {
                bool dup = false;
                for (const auto& p : out) {
                    if (p.r == c.r && p.g == c.g && p.b == c.b) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
            }
            out.push_back(c);
        }
    }
}

// Deterministic pseudo-random value in [-1, 1] for a stop, seeded from its
// index and its own color so a given scene's stops always jitter the same
// way on every device and in the web preview (a murmur3-style bit mixer).
inline float stopJitter(size_t i, const Color& c) {
    uint32_t h = (uint32_t)i * 374761393u;
    h += ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    h = (h ^ (h >> 15)) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFFu * 2.0f - 1.0f;
}

// How far a stop may drift from its perfectly-even grid position, as a
// fraction of the even spacing between stops. Bounded well under 0.5 so
// adjacent stops can never cross regardless of jitter direction.
constexpr float kStopJitterFraction = 0.3f;

// Physical positions of each stop along [0, length), off the perfectly even
// grid by a deterministic jitter (see stopJitter) instead of sitting at
// exact multiples of the even spacing. circular=true lays stops around a
// closed ring; otherwise stops sit inset by roughly half a spacing unit
// from the physical ends, as before, with the same jitter applied to those
// end stops too.
inline void computeStopPositions(const std::vector<Color>& stops, float length, bool circular,
                                 std::vector<float>& out) {
    size_t n = stops.size();
    out.assign(n, 0.0f);
    if (n == 0 || length <= 0) return;
    float spacing = length / (float)n;
    float amplitude = spacing * kStopJitterFraction;
    float lo = circular ? 0.0f : spacing * 0.5f;
    for (size_t i = 0; i < n; i++)
        out[i] = lo + (float)i * spacing + stopJitter(i, stops[i]) * amplitude;
}

// Sample a color at continuous position x along [0, length), using the
// (possibly jittered) stop positions computed by computeStopPositions.
// circular=true loops the palette seamlessly back to its first stop
// (ring topology). When not circular, the ramp mirrors back on itself
// beyond the outermost stops — each end folds back toward its neighboring
// stop instead of holding a flat block of solid color.
inline Color sample(const std::vector<Color>& palette, const std::vector<float>& positions, float x,
                    float length, bool circular) {
    size_t n = palette.size();
    if (n == 0) return Color{0, 0, 0};
    if (n == 1 || length <= 0) return palette[0];

    size_t i0, i1;
    float t;
    if (circular) {
        float xm = fmodf(x - positions[0], length);
        if (xm < 0) xm += length;
        i0 = n - 1;
        for (size_t i = 0; i < n; i++) {
            float upper = (i + 1 < n) ? (positions[i + 1] - positions[0]) : length;
            if (xm < upper) {
                i0 = i;
                break;
            }
        }
        i1 = (i0 + 1) % n;
        float lo2 = (i0 == 0) ? 0.0f : positions[i0] - positions[0];
        float hi2 = (i0 + 1 < n) ? positions[i0 + 1] - positions[0] : length;
        t = (hi2 > lo2) ? (xm - lo2) / (hi2 - lo2) : 0.0f;
    } else {
        float lo2 = positions[0];
        float span = positions[n - 1] - positions[0];
        if (span <= 0.0f) return palette[0];
        float period = 2.0f * span;
        float r = fmodf(x - lo2, period);
        if (r < 0) r += period;
        if (r > span) r = period - r;
        i0 = 0;
        for (size_t i = 0; i + 2 < n; i++) {
            if (r >= positions[i + 1] - lo2) i0 = i + 1;
        }
        i1 = i0 + 1;
        float a = positions[i0] - lo2;
        float b = positions[i1] - lo2;
        t = (b > a) ? (r - a) / (b - a) : 0.0f;
    }
    const Color& a = palette[i0];
    const Color& b = palette[i1];
    return Color{Color::lerp(a.r, b.r, t), Color::lerp(a.g, b.g, t), Color::lerp(a.b, b.b, t)};
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
        const float kFraction = 0.10f;
        const size_t kMin = 2;
        const size_t kMax = 8;
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
    if (target >= n) {
        out = full;
        return;
    }

    std::vector<bool> picked(n, false);
    std::vector<int> minDist(n, 0);
    picked[0] = true;
    for (size_t i = 0; i < n; i++) minDist[i] = colorDistSq(full[i], full[0]);

    for (size_t k = 1; k < target; k++) {
        size_t best = n;
        int bestDist = -1;
        for (size_t i = 0; i < n; i++) {
            if (!picked[i] && minDist[i] > bestDist) {
                bestDist = minDist[i];
                best = i;
            }
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
    for (size_t i = 0; i < n; i++)
        if (picked[i]) out.push_back(full[i]);
}

// Continuously morphs each gradient stop toward a freshly chosen random
// palette color. On arrival it doesn't ease back to where it came from —
// it immediately picks another random target — so every stop endlessly
// wanders the palette instead of oscillating between two fixed colors.
// Callers resample the gradient ramp from the morphed stop colors every
// tick, so the whole ramp (and every column/position it covers) updates
// in real time rather than only a cached, mostly-static base.
class StopMorph {
   public:
    // out.size() ends up equal to stops.size(); called every tick.
    void tick(uint32_t now, const std::vector<Color>& stops, const std::vector<Color>& palette,
              float speed, std::vector<Color>& out) {
        size_t n = stops.size();
        if (_cells.size() != n) {
            _cells.assign(n, Cell{});
            for (size_t i = 0; i < n; i++) _cells[i].current = stops[i];
        }
        out.resize(n);
        float spd = speed > 0.05f ? speed : 0.05f;

        for (size_t i = 0; i < n; i++) {
            Cell& c = _cells[i];
            if (c.durationMs == 0) _startLeg(c, now, palette, spd);

            uint32_t elapsed = now - c.startMs;
            if (elapsed >= c.durationMs) {
                c.current = c.to;
                _startLeg(c, now, palette, spd);
                elapsed = 0;
            }

            float t = c.durationMs > 0 ? (float)elapsed / (float)c.durationMs : 1.0f;
            if (t > 1.0f) t = 1.0f;
            float s = t * t * (3.0f - 2.0f * t);
            out[i] = Color{Color::lerp(c.from.r, c.to.r, s), Color::lerp(c.from.g, c.to.g, s),
                           Color::lerp(c.from.b, c.to.b, s)};
        }
    }

    void reset() { _cells.clear(); }

   private:
    struct Cell {
        Color current;  // color at the start of the current leg
        Color from, to;
        uint32_t startMs = 0;
        uint32_t durationMs = 0;
    };

    std::vector<Cell> _cells;

    static uint32_t _jitterMs(uint32_t lo, uint32_t hi, float speed) {
        uint32_t base = lo + (uint32_t)random(0, hi - lo + 1);
        return (uint32_t)((float)base / speed);
    }

    static void _startLeg(Cell& c, uint32_t now, const std::vector<Color>& palette, float speed) {
        c.from = c.current;
        c.to = _pickDifferent(palette, c.current);
        c.startMs = now;
        c.durationMs = _jitterMs(1500, 4000, speed);
    }

    // Picks a random palette color, retrying a few times to avoid landing
    // back on the color it's already morphing from (not guaranteed if the
    // palette is tiny or full of near-duplicates, but good enough in
    // practice since it's purely cosmetic).
    static Color _pickDifferent(const std::vector<Color>& palette, const Color& current) {
        if (palette.empty()) return current;
        if (palette.size() == 1) return palette[0];
        for (int attempt = 0; attempt < 8; attempt++) {
            const Color& c = palette[random(0, palette.size())];
            if (c.r != current.r || c.g != current.g || c.b != current.b) return c;
        }
        return palette[random(0, palette.size())];
    }
};

// Shared Pattern implementation for gradient-mode renderers (GradientMatrix,
// GradientString): palette load/reload, stop reduction, live morphing, and
// per-tick resampling — everything except how gradient positions map onto
// physical LEDs. Derived (via CRTP) supplies that geometry:
//   _totalPixels()         number of LEDs to render this tick
//   _gradientLength()      number of distinct positions along the ramp
//   _gradientCoord(i)      ramp position [0, _gradientLength()) for pixel i
//   _ledIndex(i)           physical LED index to write pixel i to
template <typename Derived>
class Base : public Pattern {
   public:
    void setWrap(bool wrap) {
        if (wrap == _wrap) return;
        _wrap = wrap;
        _computeBase();
        _morph.reset();
    }

    void begin(LedDriver& led, const LightConfig& cfg) override {
        _led = &led;
        _cfg = cfg;
        loadPalette(cfg.sceneId, _palette);
        _computeBase();
        _morph.reset();
        tick(millis());
    }

    void applyConfig(const LightConfig& cfg) override {
        bool sceneChanged = strncmp(cfg.sceneId, _cfg.sceneId, sizeof(cfg.sceneId)) != 0;
        bool stopCountChanged = cfg.gradientStopCount != _cfg.gradientStopCount;
        _cfg = cfg;
        if (sceneChanged) {
            loadPalette(cfg.sceneId, _palette);
            _computeBase();
            _morph.reset();
        } else if (stopCountChanged) {
            _computeBase();
            _morph.reset();
        }
    }

    // Re-read the scene palette if it matches the currently loaded scene.
    void reloadIfCurrent(const char* sceneId) {
        if (!sceneId || strncmp(sceneId, _cfg.sceneId, sizeof(_cfg.sceneId)) != 0) return;
        loadPalette(sceneId, _palette);
        _computeBase();
        _morph.reset();
    }

    void tick(uint32_t now) override {
        if (!_led) return;
        Derived* self = static_cast<Derived*>(this);
        uint32_t total = self->_totalPixels();
        if (_base.size() != total) _computeBase();

        const std::vector<Color>* colors = &_base;
        if (_cfg.morphEnabled && _stops.size() > 1) {
            _morph.tick(now, _stops, _palette, _cfg.speed, _liveStops);
            _resample(_liveStops, _out);
            colors = &_out;
        }

        for (uint32_t i = 0; i < total; i++) {
            const Color& c = (*colors)[i];
            _led->setPixel(self->_ledIndex(i), applyBrightness(c.r), applyBrightness(c.g),
                           applyBrightness(c.b));
        }
        _led->show();
    }

   protected:
    bool _wrap = false;
    std::vector<Color> _palette;    // full distinct-color list from the scene
    std::vector<Color> _stops;      // reduced set actually used as gradient stops
    std::vector<float> _positions;  // jittered physical position of each stop
    std::vector<Color> _liveStops;  // _stops after live morph interpolation
    std::vector<Color> _base;
    std::vector<Color> _out;
    StopMorph _morph;

    void _computeBase() {
        Derived* self = static_cast<Derived*>(this);
        float len = (float)self->_gradientLength();
        reduceToStops(_palette, self->_gradientLength(), _stops, _cfg.gradientStopCount);
        computeStopPositions(_stops, len, _wrap, _positions);
        _resample(_stops, _base);
    }

    void _resample(const std::vector<Color>& stops, std::vector<Color>& out) {
        Derived* self = static_cast<Derived*>(this);
        uint32_t total = self->_totalPixels();
        float len = (float)self->_gradientLength();
        out.resize(total);
        for (uint32_t i = 0; i < total; i++)
            out[i] = sample(stops, _positions, self->_gradientCoord(i), len, _wrap);
    }
};

}  // namespace GradientCommon
