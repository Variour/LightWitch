#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "GameTypes.h"

// LED zones of a device for the game session (docs/game-building-blocks.md,
// "LED output"): the column, the buzzer ring, the status dot and one LED per
// physical key. Effects (LedEffects.h) and the round pictures (GameRender.h)
// draw on zones, not on chain indices.
namespace game {

static constexpr uint8_t kNoLed = 0xFF;

struct LedZones {
    uint16_t count;          // LEDs in the chain
    uint8_t columnFirst;     // column LED where the fill starts in normal reading direction
    uint8_t columnCount;     // column LEDs
    uint8_t ring[4];         // ring corners: lower left, upper left, upper right, lower right
    uint8_t dot;             // status dot
    uint8_t key[kKeySlots];  // LED per physical key (Key index), kNoLed = none

    // Column LED n in reading direction (0 = where the fill starts).
    constexpr uint8_t columnLed(uint8_t n, bool reversed) const {
        return (uint8_t)(columnFirst + (reversed ? columnCount - 1 - n : n));
    }

    // LED of a physical key; kNoLed for keys without one.
    constexpr uint8_t keyLed(Key k) const {
        return (uint8_t)k < kKeySlots ? key[(uint8_t)k] : kNoLed;
    }
};

// Buzzerboard rev A0 (GameConfig.h "LEDs"). The buzz key lights the ring
// zone, not a key LED.
static constexpr LedZones kBuzzerboardA0Zones = {
    kLedCount,
    kScoreFirst,
    kScoreCount,
    {kRingLeds[0], kRingLeds[1], kRingLeds[2], kRingLeds[3]},
    kStatusDot,
    {kNoLed, kLedUp, kLedDown, kLedLeft, kLedRight, kLedEnter, kNoLed, kNoLed, kNoLed, kNoLed},
};
static_assert(kBuzzerboardA0Zones.key[(uint8_t)Key::Enter] == kLedEnter, "key LED order");
static_assert(kBuzzerboardA0Zones.key[(uint8_t)Key::Right] == kLedRight, "key LED order");

}  // namespace game
