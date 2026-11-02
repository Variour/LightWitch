#pragma once
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
};

// Charging mode. Charging: column = battery level in red/yellow/green with a
// slowly pulsing white tip, status dot in the level colour. Full: whole
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
    Rgb c = level(batteryColor(v.battery), kDisplayLevel);
    uint8_t n = columnCount(v.battery);
    if (n > 0) {
        fillColumn(px, (uint8_t)(n - 1), c);
        px[columnLed((uint8_t)(n - 1))] =
            level(game::colorRgb(Color::White), slowPulse(now, kTipPulseMs));
    }
    px[game::kStatusDot] = c;
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
