#pragma once
#include <math.h>
#include <stdint.h>

#include "GameConfig.h"
#include "GameTypes.h"
#include "LedZones.h"

// LED effect primitives on zones (docs/game-building-blocks.md, "LED
// output"): off, fill, bar, pulse, blink, run and timer. Plain C++, no
// driver access. Nothing blinks faster than 3 Hz, pulses are soft.
namespace game {

// ── Levels over time ─────────────────────────────────────────────────────────

// Triangle between lo and hi, one cycle per periodMs.
inline uint8_t softPulse(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi) {
    uint32_t ph = now % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t tri = ph < half ? ph : periodMs - ph;  // 0..half
    return (uint8_t)(lo + (uint32_t)(hi - lo) * tri / half);
}

// Triangle between kLevelDimmed and kLevelBright, one cycle per kPulsePeriodMs.
inline uint8_t pulseLevel(uint32_t now) {
    return softPulse(now, kPulsePeriodMs, kLevelDimmed, kLevelBright);
}

// On for the first half of each kBlinkPeriodMs (1 Hz).
inline bool blinkOn(uint32_t now) { return now % kBlinkPeriodMs < kBlinkPeriodMs / 2; }

// a toward b by t/255.
inline Rgb blendRgb(Rgb a, Rgb b, uint8_t t) {
    return {(uint8_t)(a.r + ((int)b.r - a.r) * t / 255),
            (uint8_t)(a.g + ((int)b.g - a.g) * t / 255),
            (uint8_t)(a.b + ((int)b.b - a.b) * t / 255)};
}

// ── Zone effects ─────────────────────────────────────────────────────────────

// All LEDs of the chain off.
inline void allOff(const LedZones& z, Rgb* px) {
    for (uint16_t i = 0; i < z.count; i++) px[i] = Rgb{};
}

inline void fillRing(const LedZones& z, Rgb* px, Rgb c) {
    for (uint8_t i : z.ring) px[i] = c;
}

// Bar: the first count column LEDs in reading direction.
inline void fillColumn(const LedZones& z, Rgb* px, uint8_t count, Rgb c, bool reversed = false) {
    for (uint8_t i = 0; i < count && i < z.columnCount; i++) px[z.columnLed(i, reversed)] = c;
}

// Timer bar: lit column LEDs in reading direction, the last one partly lit
// by its fraction. Draws nothing for lit <= 0.
inline void columnLevel(const LedZones& z, Rgb* px, float lit, Rgb c, bool reversed) {
    for (uint8_t n = 0; n < z.columnCount; n++) {
        float on = lit - (float)n;
        if (on >= 1.0f)
            px[z.columnLed(n, reversed)] = c;
        else if (on > 0.0f)
            px[z.columnLed(n, reversed)] = scaleRgb(c, (uint8_t)(255.0f * on));
    }
}

// Run: a head in colour head runs up the column in reading direction once
// per runMs, from 2 LEDs before the column to 2 past its end in 80 % of a
// run; behind it the column takes trail. t is the time since the effect
// began; after the first run the whole column holds trail.
inline void columnRun(const LedZones& z, Rgb* px, uint32_t t, uint32_t runMs, Rgb trail, Rgb head,
                      bool reversed) {
    uint32_t run = t % runMs;
    uint32_t sweep = runMs * 4 / 5;
    float pos = run < sweep ? (float)run * (z.columnCount + 4) / (float)sweep - 2.0f : 100.0f;
    for (uint8_t n = 0; n < z.columnCount; n++) {
        float d = pos - (float)n;
        Rgb c = d >= 0.0f || t >= runMs ? trail : Rgb{};
        float a = fabsf(d);
        if (a < 2.5f) c = blendRgb(c, head, (uint8_t)(255.0f * (1.0f - a / 2.5f)));
        px[z.columnLed(n, reversed)] = c;
    }
}

}  // namespace game
