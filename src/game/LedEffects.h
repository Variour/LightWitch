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

// ── Fine pixels ──────────────────────────────────────────────────────────────

// A pixel of the session frame with fractional channel values, 0-255 like
// Rgb. The frame stays fine until the driver, which dithers it by position
// (led/Dither.h), so fades and pulses glide also at a low brightness
// (Timo, 2026-11-20). An Rgb converts to it unchanged.
struct RgbF {
    float r = 0, g = 0, b = 0;
    RgbF() = default;
    constexpr RgbF(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
    constexpr RgbF(Rgb c) : r(c.r), g(c.g), b(c.b) {}
};

// c at level (0-255, fractional).
inline RgbF scaleF(RgbF c, float level) {
    float k = level / 255.0f;
    return {c.r * k, c.g * k, c.b * k};
}

// a toward b by t (0-1).
inline RgbF blendF(RgbF a, RgbF b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// ── Levels over time ─────────────────────────────────────────────────────────

// The eye sees LED brightness about as the 1/2.2 power of the PWM value.
// Pulses move evenly in that perceived brightness, so the dark end does not
// jump and the bright end does not hang.
static constexpr float kPerceptGamma = 2.2f;

// 0 at the start of each periodMs, 1 at its middle, back to 0 at its end,
// as a raised cosine: soft at both turns, unlike a triangle's hard corner.
inline float softWave(uint32_t now, uint32_t periodMs) {
    float ph = (float)(now % periodMs) / (float)periodMs;
    return 0.5f - 0.5f * cosf(6.2831853f * ph);
}

// Blend factor for a pulse between two colours, 0-1 (softWave).
inline float softMix(uint32_t now, uint32_t periodMs) { return softWave(now, periodMs); }

// Level between lo and hi (0-255), one cycle per periodMs: lo at the start,
// hi at the middle, evenly in perceived brightness. lo may be above hi.
inline float softPulse(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi) {
    float pl = powf(lo / 255.0f, 1.0f / kPerceptGamma);
    float ph = powf(hi / 255.0f, 1.0f / kPerceptGamma);
    float p = pl + (ph - pl) * softWave(now, periodMs);
    return 255.0f * powf(p, kPerceptGamma);
}

// Pulse between kLevelDimmed and kLevelBright, one cycle per kPulsePeriodMs.
inline float pulseLevel(uint32_t now) {
    return softPulse(now, kPulsePeriodMs, kLevelDimmed, kLevelBright);
}

// On for the first half of each kBlinkPeriodMs (1 Hz).
inline bool blinkOn(uint32_t now) { return now % kBlinkPeriodMs < kBlinkPeriodMs / 2; }

// ── Zone effects ─────────────────────────────────────────────────────────────

// All LEDs of the chain off.
inline void allOff(const LedZones& z, RgbF* px) {
    for (uint16_t i = 0; i < z.count; i++) px[i] = RgbF{};
}

inline void fillRing(const LedZones& z, RgbF* px, RgbF c) {
    for (uint8_t i : z.ring) px[i] = c;
}

// Bar: the first count column LEDs in reading direction.
inline void fillColumn(const LedZones& z, RgbF* px, uint8_t count, RgbF c, bool reversed = false) {
    for (uint8_t i = 0; i < count && i < z.columnCount; i++) px[z.columnLed(i, reversed)] = c;
}

// Timer bar: lit column LEDs in reading direction, the last one partly lit
// by its fraction, evenly in perceived brightness. Draws nothing for
// lit <= 0.
inline void columnLevel(const LedZones& z, RgbF* px, float lit, RgbF c, bool reversed) {
    for (uint8_t n = 0; n < z.columnCount; n++) {
        float on = lit - (float)n;
        if (on >= 1.0f)
            px[z.columnLed(n, reversed)] = c;
        else if (on > 0.0f)
            px[z.columnLed(n, reversed)] = scaleF(c, 255.0f * powf(on, kPerceptGamma));
    }
}

// Run: a head in colour head runs up the column in reading direction once
// per runMs, from 2 LEDs before the column to 2 past its end in 80 % of a
// run; behind it the column takes trail. t is the time since the effect
// began; after the first run the whole column holds trail.
inline void columnRun(const LedZones& z, RgbF* px, uint32_t t, uint32_t runMs, RgbF trail,
                      RgbF head, bool reversed) {
    uint32_t run = t % runMs;
    uint32_t sweep = runMs * 4 / 5;
    float pos = run < sweep ? (float)run * (z.columnCount + 4) / (float)sweep - 2.0f : 100.0f;
    for (uint8_t n = 0; n < z.columnCount; n++) {
        float d = pos - (float)n;
        RgbF c = d >= 0.0f || t >= runMs ? trail : RgbF{};
        float a = fabsf(d);
        if (a < 2.5f) {
            float w = 1.0f - a / 2.5f;  // smoothstep: the head has soft flanks
            c = blendF(c, head, w * w * (3.0f - 2.0f * w));
        }
        px[z.columnLed(n, reversed)] = c;
    }
}

}  // namespace game
