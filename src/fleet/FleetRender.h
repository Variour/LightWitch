#pragma once
#include <math.h>
#include <stdint.h>

#include "../game/GameConfig.h"
#include "../game/GameTypes.h"
#include "FleetConfig.h"
#include "FleetTypes.h"

// The fleet package's frames on the 34 Buzzerboard LEDs (owner's display
// sketch "Was zeigt ein Buzzer im Lademodus?"): charging, full, fault and
// update. Plain C++, no driver access. USB-C is on the top edge and LED1-24
// run bottom to top (Buzzerboard hardware/README.md). Owner (2026-11-09):
// every level on the score column fills from LED1 upwards.
namespace fleet {

using game::Rgb;

inline Rgb level(Rgb c, uint8_t lvl) { return game::scaleRgb(c, lvl); }

inline void clearFrame(Rgb* px) {
    for (uint16_t i = 0; i < game::kLedCount; i++) px[i] = Rgb{};
}

// Column LED n (0 = the USB-C end) -> chain index.
inline uint8_t columnLed(uint8_t n) {
    return (uint8_t)(game::kScoreFirst + game::kScoreCount - 1 - n);
}

// Column LED n counted from LED1 (0 = the end away from USB-C) -> chain index.
inline uint8_t columnLedFromLed1(uint8_t n) { return (uint8_t)(game::kScoreFirst + n); }

// The first count column LEDs from LED1 in c.
inline void fillColumn(Rgb* px, uint8_t count, Rgb c) {
    for (uint8_t n = 0; n < count && n < game::kScoreCount; n++) px[columnLedFromLed1(n)] = c;
}

// Column LEDs for a percentage: at least one while above 0.
inline uint8_t columnCount(uint8_t percent) {
    if (percent == 0) return 0;
    if (percent >= 100) return game::kScoreCount;
    uint8_t n = (uint8_t)(((uint16_t)percent * game::kScoreCount + 99) / 100);
    return n == 0 ? 1 : n;
}

// Triangle from top / 4 up to top, one cycle per period.
inline uint8_t slowPulse(uint32_t now, uint32_t period, uint8_t top) {
    uint32_t ph = now % period;
    uint32_t half = period / 2;
    uint32_t tri = ph < half ? ph : period - ph;
    uint8_t lo = top / 4;
    return (uint8_t)(lo + (uint32_t)(top - lo) * tri / half);
}

struct ChargeView {
    Charge charge = Charge::None;
    uint8_t battery = kNoBattery;
    uint32_t fullForMs = 0;  // how long the charger has reported full
    int16_t ibatMa = 0;      // charge current, + = charging
};

inline Rgb levelRounded(Rgb c, uint8_t lvl) {
    return {(uint8_t)((c.r * lvl + 127) / 255), (uint8_t)((c.g * lvl + 127) / 255),
            (uint8_t)((c.b * lvl + 127) / 255)};
}

// Raised cosine from lo up to hi and back, one cycle per period.
inline uint8_t softPulse(uint32_t now, uint32_t period, uint8_t lo, uint8_t hi) {
    float x = (float)(now % period) / (float)period;
    float s = 0.5f - 0.5f * cosf(6.2831853f * x);
    return (uint8_t)(lo + (uint8_t)lroundf((float)(hi - lo) * s));
}

// Level LED n (0 = the first LED of the level): red through yellow to green.
inline Rgb columnGradient(uint8_t n) {
    uint16_t t = (uint16_t)((uint32_t)n * 510 / (game::kScoreCount - 1));
    if (t <= 255) return Rgb{255, (uint8_t)t, 0};
    return Rgb{(uint8_t)(510 - t), 255, 0};
}

// The charged LEDs while charging and when full: kChargeFillPercent of lvl,
// at least kOffLevel.
inline uint8_t chargeFillLevel(uint8_t lvl) {
    uint8_t f = (uint8_t)((uint16_t)lvl * kChargeFillPercent / 100);
    return f < kOffLevel ? kOffLevel : f;
}

// Peak of the glowing LEDs while charging: kChargeGlowPercent of lvl, at
// least kOffTipLevel.
inline uint8_t chargeGlowLevel(uint8_t lvl) {
    uint8_t g = (uint8_t)((uint16_t)lvl * kChargeGlowPercent / 100);
    return g < kOffTipLevel ? kOffTipLevel : g;
}

// Battery level on the column at lvl, filling from LED1, red at LED1 to green
// at the USB-C end (columnGradient), status dot in the colour of the level's
// last LED. While the battery charges with at least kFlowMinMa, the level
// shows at chargeFillLevel(lvl) and its last kChargeGlowLeds LEDs glow up
// together to chargeGlowLevel(lvl) once per kTipGlowMs.
inline void renderLevel(uint8_t battery, int16_t ibatMa, uint32_t now, Rgb* px, uint8_t lvl) {
    clearFrame(px);
    if (battery == kNoBattery) return;
    uint8_t n = columnCount(battery);
    if (n == 0) return;
    bool charging = ibatMa >= kFlowMinMa;
    uint8_t fill = charging ? chargeFillLevel(lvl) : lvl;
    for (uint8_t i = 0; i < n; i++)
        px[columnLedFromLed1(i)] = levelRounded(columnGradient(i), fill);
    if (charging) {
        uint8_t glow = softPulse(now, kTipGlowMs, fill, chargeGlowLevel(lvl));
        uint8_t from = n > kChargeGlowLeds ? (uint8_t)(n - kChargeGlowLeds) : 0;
        for (uint8_t i = from; i < n; i++)
            px[columnLedFromLed1(i)] = levelRounded(columnGradient(i), glow);
    }
    px[game::kStatusDot] = levelRounded(columnGradient(n - 1), lvl);
}

// LEDs switched off with a key, on USB: the level on the column at
// kOffLevel, filling from LED1 towards USB-C, red at LED1 to green at the
// USB-C end (Timo, 2026-11-09). While charging, the last LED of the level
// pulses up to kOffTipLevel. Ring and status dot stay dark.
inline void renderOffLevel(uint8_t battery, bool charging, uint32_t now, Rgb* px) {
    clearFrame(px);
    if (battery == kNoBattery) return;
    uint8_t n = columnCount(battery);
    for (uint8_t i = 0; i < n; i++)
        px[columnLedFromLed1(i)] = levelRounded(columnGradient(i), kOffLevel);
    if (charging && n > 0)
        px[columnLedFromLed1(n - 1)] = levelRounded(
            columnGradient(n - 1), softPulse(now, kTipGlowMs, kOffLevel, kOffTipLevel));
}

// The device's own colours: the look's colour per column LED from LED1, one
// breath per kIdlePulseMs from lvl / 5 up to lvl and back, t = time since the
// breath began. Ring and status dot stay dark.
static_assert(kLookLeds == game::kScoreCount, "one look colour per column LED");
inline void renderLook(const Look& look, uint32_t t, uint8_t lvl, Rgb* px) {
    clearFrame(px);
    if (look.count != kLookLeds) return;
    uint8_t b = softPulse(t, kIdlePulseMs, (uint8_t)(lvl / 5), lvl);
    for (uint8_t i = 0; i < kLookLeds; i++)
        px[columnLedFromLed1(i)] =
            levelRounded(Rgb{look.rgb[i][0], look.rgb[i][1], look.rgb[i][2]}, b);
}

// The gateway (the console's USB link): the device's own colours at lvl, or a
// blue column without them, and a soft white pulse that runs from the USB-C
// end down the column in the first 70 % of every kGatewayFlowMs.
inline void renderGateway(const Look& look, uint32_t now, uint8_t lvl, Rgb* px) {
    clearFrame(px);
    uint32_t ph = now % kGatewayFlowMs;
    uint32_t run = kGatewayFlowMs * 7 / 10;
    // Pulse centre from 2 LEDs above the top to 2 below LED1.
    float pos =
        ph < run ? (float)(kLookLeds + 1) - (float)ph * (kLookLeds + 4) / (float)run : -100.0f;
    for (uint8_t i = 0; i < kLookLeds; i++) {
        Rgb c = look.count == kLookLeds ? Rgb{look.rgb[i][0], look.rgb[i][1], look.rgb[i][2]}
                                        : Rgb{0, 48, 255};
        float d = fabsf((float)i - pos);
        if (d < 2.5f) {
            float w = 0.7f * (1.0f - d / 2.5f);
            c = Rgb{(uint8_t)(c.r + (255 - c.r) * w), (uint8_t)(c.g + (255 - c.g) * w),
                    (uint8_t)(c.b + (255 - c.b) * w)};
        }
        px[columnLedFromLed1(i)] = levelRounded(c, lvl);
    }
}

// Charging mode at lvl. Charging: renderLevel(). Full: whole column in
// columnGradient at chargeFillLevel(lvl), status dot green at lvl, after
// kFullColumnMs only the dot.
// Fault: ring blinks red, column off, status dot red.
inline void renderCharging(const ChargeView& v, uint32_t now, Rgb* px, uint8_t lvl) {
    using game::Color;
    clearFrame(px);
    if (v.charge == Charge::Fault) {
        Rgb red = level(game::colorRgb(Color::Red), lvl);
        if (now % kFaultBlinkMs < kFaultBlinkMs / 2)
            for (uint8_t i : game::kRingLeds) px[i] = red;
        px[game::kStatusDot] = red;
        return;
    }
    if (v.charge == Charge::Full) {
        Rgb green = level(game::colorRgb(Color::Green), lvl);
        if (v.fullForMs < kFullColumnMs)
            for (uint8_t i = 0; i < game::kScoreCount; i++)
                px[columnLedFromLed1(i)] = levelRounded(columnGradient(i), chargeFillLevel(lvl));
        px[game::kStatusDot] = green;
        return;
    }
    if (v.battery == kNoBattery) {
        px[game::kStatusDot] =
            level(game::colorRgb(Color::White), slowPulse(now, kTipPulseMs, lvl));
        return;
    }
    renderLevel(v.battery, v.ibatMa, now, px, lvl);
}

// Update running at lvl: column blue = progress, one ring LED runs around in
// blue, status dot blue.
inline void renderUpdate(uint8_t percent, uint32_t now, Rgb* px, uint8_t lvl) {
    using game::Color;
    clearFrame(px);
    Rgb blue = level(game::colorRgb(Color::Blue), lvl);
    fillColumn(px, columnCount(percent), blue);
    // Ring order around the circle: lower left, upper left, upper right, lower right.
    px[game::kRingLeds[(now / kRingChaseMs) % 4]] = blue;
    px[game::kStatusDot] = blue;
}

}  // namespace fleet
