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
// (ring topology, or tiling needed for a scrolling gradient).
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
        float xc = x < 0 ? 0 : (x > length - 1 ? length - 1 : x);
        seg = (length > 1) ? xc / (length - 1) * (float)(n - 1) : 0.0f;
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

}  // namespace GradientCommon
