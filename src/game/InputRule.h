#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "GameTypes.h"

// Input building block of a round (docs/game-building-blocks.md, "Inputs"):
// which logical keys take input and how a press turns into the device's
// value. Participant runs every round through one rule; the v0 round types
// are presets (presetRule). Plain C++, builds on the host.
namespace game {

enum class InputMode : uint8_t {
    None = 0,     // no input
    Once = 1,     // the first press of a key sets its value, then the value stays
    Pick = 2,     // each key maps to a value; the last press counts, or the first with firstOnly
    Counter = 3,  // upKey / downKey step a value between min and max
    Log = 4,      // every press goes into the key log (QuizKeys)
};

struct InputRule {
    InputMode mode = InputMode::None;
    // Value per logical key slot, 0 = the key gives no value (Once, Pick,
    // Log: a key with 0 is ignored).
    uint8_t keyValue[kKeySlots] = {};
    // Pick and Counter: the first value that goes out stays.
    bool firstOnly = false;
    // Counter.
    Key upKey = Key::None;
    Key downKey = Key::None;
    Key confirmKey = Key::None;  // Key::None: every step goes out, else this key sends it
    uint8_t min = 0, max = 0;
    uint8_t firstUp = 0, firstDown = 0;  // value of the first step up / down
    bool repeat = false;                 // holding upKey / downKey steps on
    // Send policy: a value goes out sendDelayMs after its input (0 = at once).
    uint32_t sendDelayMs = 0;

    bool takes(Key k) const { return (uint8_t)k < kKeySlots && keyValue[(uint8_t)k] != 0; }
    uint8_t valueOf(Key k) const { return (uint8_t)k < kKeySlots ? keyValue[(uint8_t)k] : 0; }
    bool confirmed() const { return confirmKey != Key::None; }
};

// Choice answer per logical key: up 1, right 2, down 3, left 4 (gateway
// protocol v0, "Erlaubte Tasten"), enter 5 (owner, 2026-11-10).
inline void setChoiceValues(InputRule& r) {
    r.keyValue[(uint8_t)Key::Up] = 1;
    r.keyValue[(uint8_t)Key::Right] = 2;
    r.keyValue[(uint8_t)Key::Down] = 3;
    r.keyValue[(uint8_t)Key::Left] = 4;
    r.keyValue[(uint8_t)Key::Enter] = 5;
}

// Only the keys of keyMask keep their value.
inline void maskKeys(InputRule& r, uint16_t keyMask) {
    for (uint8_t i = 0; i < kKeySlots; i++)
        if (!(keyMask & (1u << i))) r.keyValue[i] = 0;
}

// The rule of a v0 round type (round format v0, "Verhalten am Gerät").
inline InputRule presetRule(RoundType type, uint16_t keyMask, uint8_t min, uint8_t max,
                            bool firstOnly) {
    InputRule r;
    switch (type) {
        case RoundType::Race:
            r.mode = InputMode::Once;
            r.keyValue[(uint8_t)Key::Buzz] = 1;
            break;
        case RoundType::Choice:
            r.mode = InputMode::Pick;
            setChoiceValues(r);
            // proposal 8: the last press counts; firstOnly takes the first.
            r.firstOnly = firstOnly || !kChoiceChangeAllowed;
            break;
        case RoundType::Scale: {
            r.mode = InputMode::Counter;
            r.upKey = Key::Up;
            r.downKey = Key::Down;
            r.keyValue[(uint8_t)Key::Up] = 1;
            r.keyValue[(uint8_t)Key::Down] = 1;
            if (keyMask & (1u << (uint8_t)Key::Enter)) {
                r.confirmKey = Key::Enter;  // link `confirm`
                r.keyValue[(uint8_t)Key::Enter] = 1;
            }
            r.min = min;
            r.max = max;
            // Proposal 2: the first tap sets the middle.
            uint8_t steps = max >= min ? (uint8_t)(max - min + 1) : 0;
            r.firstUp = (uint8_t)(min + steps / 2);
            r.firstDown = (uint8_t)(min + (steps ? (steps - 1) / 2 : 0));
            r.repeat = true;
            r.firstOnly = firstOnly;
            r.sendDelayMs = firstOnly ? kScaleCommitMs : kScaleSendDelayMs;
            break;
        }
        case RoundType::Move:
            r.mode = InputMode::Log;
            r.keyValue[(uint8_t)Key::Up] = 1;
            r.keyValue[(uint8_t)Key::Right] = 2;
            r.keyValue[(uint8_t)Key::Down] = 3;
            r.keyValue[(uint8_t)Key::Left] = 4;
            break;
        default:
            return r;
    }
    maskKeys(r, keyMask);
    return r;
}

// The answer keys of the device that has the turn in a race (link `game
// answering`): the choice answers of the round's keys.
inline InputRule turnRule(uint16_t keyMask) {
    InputRule r;
    r.mode = InputMode::Once;
    setChoiceValues(r);
    maskKeys(r, keyMask);
    return r;
}

}  // namespace game
