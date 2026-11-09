#pragma once
#include <math.h>
#include <stdint.h>

#include "../game/GameConfig.h"
#include "../game/GameTypes.h"
#include "FleetConfig.h"
#include "FleetTypes.h"

// The fleet package's frames on the 34 Buzzerboard LEDs (owner's display
// sketch "Was zeigt ein Buzzer im Lademodus?"): charging, full, fault and
// update. Plain C++, no driver access. The score column fills from the
// USB-C end: USB-C is on the top edge and LED1-24 run bottom to top
// (Buzzerboard hardware/README.md), so the fill starts at LED24 and grows
// upwards while the device stands USB-C down in the case.
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

inline void fillColumn(Rgb* px, uint8_t count, Rgb c) {
    for (uint8_t n = 0; n < count && n < game::kScoreCount; n++) px[columnLed(n)] = c;
}

// Column LEDs for a percentage: at least one while above 0.
inline uint8_t columnCount(uint8_t percent) {
    if (percent == 0) return 0;
    if (percent >= 100) return game::kScoreCount;
    uint8_t n = (uint8_t)(((uint16_t)percent * game::kScoreCount + 99) / 100);
    return n == 0 ? 1 : n;
}

inline Rgb batteryColor(uint8_t percent) {
    using game::Color;
    if (percent < kRedBelowPercent) return game::colorRgb(Color::Red);
    if (percent < kGreenFromPercent) return game::colorRgb(Color::Yellow);
    return game::colorRgb(Color::Green);
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

// Battery level on the column in red/yellow/green at lvl, status dot in the
// level colour. While the battery charges with at least kFlowMinMa, a white
// dot at twice lvl runs from the USB-C end to the level, faster the higher
// the current.
inline void renderLevel(uint8_t battery, int16_t ibatMa, uint32_t now, Rgb* px, uint8_t lvl) {
    using game::Color;
    clearFrame(px);
    if (battery == kNoBattery) return;
    Rgb c = level(batteryColor(battery), lvl);
    uint8_t n = columnCount(battery);
    fillColumn(px, n, c);
    if (ibatMa >= kFlowMinMa && n > 0) {
        uint32_t period = kFlowMaMs / (uint32_t)ibatMa;
        if (period < kFlowMinMs) period = kFlowMinMs;
        if (period > kFlowMaxMs) period = kFlowMaxMs;
        uint8_t pos = (uint8_t)((now % period) * n / period);
        uint8_t flow = lvl > 127 ? 255 : (uint8_t)(2 * lvl);
        px[columnLed(pos)] = level(game::colorRgb(Color::White), flow);
    }
    px[game::kStatusDot] = c;
}

inline Rgb levelRounded(Rgb c, uint8_t lvl) {
    return {(uint8_t)((c.r * lvl + 127) / 255), (uint8_t)((c.g * lvl + 127) / 255),
            (uint8_t)((c.b * lvl + 127) / 255)};
}

// Level LED n (0 = the first LED of the level): red through yellow to green.
inline Rgb columnGradient(uint8_t n) {
    uint16_t t = (uint16_t)((uint32_t)n * 510 / (game::kScoreCount - 1));
    if (t <= 255) return Rgb{255, (uint8_t)t, 0};
    return Rgb{(uint8_t)(510 - t), 255, 0};
}

// Raised cosine from lo up to hi and back, one cycle per period.
inline uint8_t softPulse(uint32_t now, uint32_t period, uint8_t lo, uint8_t hi) {
    float x = (float)(now % period) / (float)period;
    float s = 0.5f - 0.5f * cosf(6.2831853f * x);
    return (uint8_t)(lo + (uint8_t)lroundf((float)(hi - lo) * s));
}

// Column LED n counted from LED1 (0 = the end away from USB-C) -> chain index.
inline uint8_t columnLedFromLed1(uint8_t n) { return (uint8_t)(game::kScoreFirst + n); }

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
            columnGradient(n - 1), softPulse(now, kOffTipPulseMs, kOffLevel, kOffTipLevel));
}

// The device's own colours: the look's gradient on the column from LED1, one
// breath per kIdlePulseMs from lvl / 5 up to lvl and back, t = time since the
// breath began. Ring and status dot stay dark.
inline void renderLook(const Look& look, uint32_t t, uint8_t lvl, Rgb* px) {
    clearFrame(px);
    if (look.count < 2) return;
    uint8_t b = softPulse(t, kIdlePulseMs, (uint8_t)(lvl / 5), lvl);
    uint8_t last = (uint8_t)(look.count - 1);
    for (uint8_t i = 0; i < game::kScoreCount; i++) {
        Rgb c;
        if (i <= look.pos[0]) {
            c = Rgb{look.rgb[0][0], look.rgb[0][1], look.rgb[0][2]};
        } else if (i >= look.pos[last]) {
            c = Rgb{look.rgb[last][0], look.rgb[last][1], look.rgb[last][2]};
        } else {
            uint8_t k = 0;
            while (k + 1 < last && i > look.pos[k + 1]) k++;
            uint16_t span = (uint16_t)(look.pos[k + 1] - look.pos[k]);
            uint16_t off = (uint16_t)(i - look.pos[k]);
            uint8_t v[3];
            for (uint8_t j = 0; j < 3; j++)
                v[j] = (uint8_t)((look.rgb[k][j] * (span - off) + look.rgb[k + 1][j] * off) / span);
            c = Rgb{v[0], v[1], v[2]};
        }
        px[columnLedFromLed1(i)] = levelRounded(c, b);
    }
}

// Charging mode at lvl. Charging: renderLevel() (level and charge current).
// Full: whole column and status dot green, after kFullColumnMs only the dot.
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
        if (v.fullForMs < kFullColumnMs) fillColumn(px, game::kScoreCount, green);
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
