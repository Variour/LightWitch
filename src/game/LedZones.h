#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "GameTypes.h"

// LED zones of a device for the game session (docs/game-building-blocks.md,
// "LED output"): the column, the buzzer ring, the status dot and one LED per
// physical key (one or more chain neighbours from that LED). Effects (LedEffects.h) and the round
// pictures (GameRender.h) draw on zones, not on chain indices.
namespace game {

static constexpr uint8_t kNoLed = 0xFF;

struct LedZones {
    uint16_t count;               // LEDs in the chain
    uint8_t columnFirst;          // column LED where the fill starts in normal reading direction
    uint8_t columnCount;          // column LEDs
    uint8_t ring[kRingCount];     // ring LEDs, clockwise from the lower left
    uint8_t dot;                  // status dot
    uint8_t key[kKeySlots];       // first LED per physical key (Key index), kNoLed = none
    uint8_t keyCount[kKeySlots];  // LEDs per physical key, from key[]

    // Column LED n in reading direction (0 = where the fill starts).
    constexpr uint8_t columnLed(uint8_t n, bool reversed) const {
        return (uint8_t)(columnFirst + (reversed ? columnCount - 1 - n : n));
    }

    // First LED of a physical key; kNoLed for keys without one.
    constexpr uint8_t keyLed(Key k) const {
        return (uint8_t)k < kKeySlots ? key[(uint8_t)k] : kNoLed;
    }

    // LEDs of a physical key; 0 for keys without one.
    constexpr uint8_t keyLeds(Key k) const {
        return keyLed(k) == kNoLed ? 0 : keyCount[(uint8_t)k];
    }
};

// The Buzzerboard this build targets (GameConfig.h "LEDs", BUZZERBOARD_REV).
// The buzz key lights the ring zone, not a key LED.
static constexpr LedZones kBuzzerboardZones = {
    kLedCount,
    kScoreFirst,
    kScoreCount,
#if BUZZERBOARD_REV == 2
    {kRingLeds[0], kRingLeds[1], kRingLeds[2], kRingLeds[3], kRingLeds[4], kRingLeds[5],
     kRingLeds[6], kRingLeds[7]},
#else
    {kRingLeds[0], kRingLeds[1], kRingLeds[2], kRingLeds[3]},
#endif
    kStatusDot,
    {kNoLed, kLedUp, kLedDown, kLedLeft, kLedRight, kLedEnter, kNoLed, kNoLed, kNoLed, kNoLed},
    {0, 1, 1, 1, 1, kLedEnterCount, 0, 0, 0, 0},
};
static_assert(kBuzzerboardZones.key[(uint8_t)Key::Enter] == kLedEnter, "key LED order");
static_assert(kBuzzerboardZones.key[(uint8_t)Key::Right] == kLedRight, "key LED order");

}  // namespace game
