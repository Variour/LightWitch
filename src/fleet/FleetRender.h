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

// Triangle from kDisplayLevel / 4 up to kDisplayLevel, one cycle per period.
inline uint8_t slowPulse(uint32_t now, uint32_t period) {
    uint32_t ph = now % period;
    uint32_t half = period / 2;
    uint32_t tri = ph < half ? ph : period - ph;
    uint8_t lo = kDisplayLevel / 4;
    return (uint8_t)(lo + (uint32_t)(kDisplayLevel - lo) * tri / half);
}

struct ChargeView {
    Charge charge = Charge::None;
    uint8_t battery = kNoBattery;
    uint32_t fullForMs = 0;  // how long the charger has reported full
    int16_t ibatMa = 0;      // charge current, + = charging
};

// Battery level on the column in red/yellow/green, status dot in the level
// colour. While the battery charges with at least kFlowMinMa, a white dot
// runs from the USB-C end to the level, faster the higher the current.
inline void renderLevel(uint8_t battery, int16_t ibatMa, uint32_t now, Rgb* px) {
    using game::Color;
    clearFrame(px);
    if (battery == kNoBattery) return;
    Rgb c = level(batteryColor(battery), kDisplayLevel);
    uint8_t n = columnCount(battery);
    fillColumn(px, n, c);
    if (ibatMa >= kFlowMinMa && n > 0) {
        uint32_t period = kFlowMaMs / (uint32_t)ibatMa;
        if (period < kFlowMinMs) period = kFlowMinMs;
        if (period > kFlowMaxMs) period = kFlowMaxMs;
        uint8_t pos = (uint8_t)((now % period) * n / period);
        px[columnLed(pos)] = level(game::colorRgb(Color::White), kFlowLevel);
    }
    px[game::kStatusDot] = c;
}

inline Rgb levelRounded(Rgb c, uint8_t lvl) {
    return {(uint8_t)((c.r * lvl + 127) / 255), (uint8_t)((c.g * lvl + 127) / 255),
            (uint8_t)((c.b * lvl + 127) / 255)};
}

// Column LED n (0 = the USB-C end): red through yellow to green at the top.
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

// LEDs switched off with a key, on USB: the level on the column at
// kOffLevel in columnGradient(). While charging, the top LED of the level
// pulses up to kOffTipLevel. Ring and status dot stay dark.
inline void renderOffLevel(uint8_t battery, bool charging, uint32_t now, Rgb* px) {
    clearFrame(px);
    if (battery == kNoBattery) return;
    uint8_t n = columnCount(battery);
    for (uint8_t i = 0; i < n; i++) px[columnLed(i)] = levelRounded(columnGradient(i), kOffLevel);
    if (charging && n > 0)
        px[columnLed(n - 1)] =
            levelRounded(columnGradient(n - 1),
                         softPulse(now, kOffTipPulseMs, kOffLevel, kOffTipLevel));
}

// Charging mode. Charging: renderLevel() (level and charge current). Full: whole
// column and status dot green, after kFullColumnMs only the dot. Fault: ring
// blinks red, column off, status dot red.
inline void renderCharging(const ChargeView& v, uint32_t now, Rgb* px) {
    using game::Color;
    clearFrame(px);
    if (v.charge == Charge::Fault) {
        Rgb red = level(game::colorRgb(Color::Red), kDisplayLevel);
        if (now % kFaultBlinkMs < kFaultBlinkMs / 2)
            for (uint8_t i : game::kRingLeds) px[i] = red;
        px[game::kStatusDot] = red;
        return;
    }
    if (v.charge == Charge::Full) {
        Rgb green = level(game::colorRgb(Color::Green), kDisplayLevel);
        if (v.fullForMs < kFullColumnMs) fillColumn(px, game::kScoreCount, green);
        px[game::kStatusDot] = green;
        return;
    }
    if (v.battery == kNoBattery) {
        px[game::kStatusDot] = level(game::colorRgb(Color::White), slowPulse(now, kTipPulseMs));
        return;
    }
    renderLevel(v.battery, v.ibatMa, now, px);
}

// Update running: column blue = progress, one ring LED runs around in blue,
// status dot blue.
inline void renderUpdate(uint8_t percent, uint32_t now, Rgb* px) {
    using game::Color;
    clearFrame(px);
    Rgb blue = level(game::colorRgb(Color::Blue), kDisplayLevel);
    fillColumn(px, columnCount(percent), blue);
    // Ring order around the circle: lower left, upper left, upper right, lower right.
    px[game::kRingLeds[(now / kRingChaseMs) % 4]] = blue;
    px[game::kStatusDot] = blue;
}

}  // namespace fleet
