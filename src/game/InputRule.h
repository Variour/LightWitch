#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "GameFrames.h"
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
    // Once, Pick, Log: value per logical key slot, 0 = the key takes no
    // input. Counter: unused, its keys are upKey, downKey and confirmKey.
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
    uint16_t sendDelayMs = 0;

    bool takes(Key k) const {
        if (mode == InputMode::Counter)
            return k != Key::None && (k == upKey || k == downKey || k == confirmKey);
        return (uint8_t)k < kKeySlots && keyValue[(uint8_t)k] != 0;
    }
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
            if (keyMask & (1u << (uint8_t)Key::Enter)) r.confirmKey = Key::Enter;  // link `confirm`
            r.min = min;
            r.max = max;
            // Proposal 2: the first tap sets the middle.
            uint8_t steps = max >= min ? (uint8_t)(max - min + 1) : 0;
            r.firstUp = (uint8_t)(min + steps / 2);
            r.firstDown = (uint8_t)(min + (steps ? (steps - 1) / 2 : 0));
            r.repeat = true;
            r.firstOnly = firstOnly;
            r.sendDelayMs = (uint16_t)(firstOnly ? kScaleCommitMs : kScaleSendDelayMs);
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

static constexpr uint16_t kMaxSendDelayMs = 5000;

inline bool keyOrNone(uint8_t k) { return k < kKeySlots || k == (uint8_t)Key::None; }

// Whether a rule is complete and consistent with the round's keys.
inline bool ruleValid(const InputRule& r, uint16_t keyMask) {
    if (r.mode == InputMode::None || (uint8_t)r.mode > (uint8_t)InputMode::Log) return false;
    if (r.sendDelayMs > kMaxSendDelayMs) return false;
    bool any = false;
    for (uint8_t i = 0; i < kKeySlots; i++) {
        if (r.keyValue[i] == 0) continue;
        if (!(keyMask & (1u << i)) || r.keyValue[i] == kNoValue) return false;
        any = true;
    }
    if (r.mode != InputMode::Counter) return any;
    if (any) return false;
    auto inMask = [&](Key k) { return (uint8_t)k < kKeySlots && (keyMask & (1u << (uint8_t)k)); };
    if (!inMask(r.upKey) || !inMask(r.downKey) || r.upKey == r.downKey) return false;
    if (r.confirmKey != Key::None &&
        (!inMask(r.confirmKey) || r.confirmKey == r.upKey || r.confirmKey == r.downKey))
        return false;
    return r.min <= r.firstDown && r.firstDown <= r.firstUp && r.firstUp <= r.max &&
           r.max != kNoValue;
}

inline void ruleToFrame(const InputRule& r, QuizRulesMsg& m) {
    m.mode = (uint8_t)r.mode;
    m.flags = (uint8_t)((r.firstOnly ? kRuleFirstOnly : 0) | (r.repeat ? kRuleRepeat : 0));
    for (uint8_t i = 0; i < kKeySlots; i++) m.keyValue[i] = r.keyValue[i];
    m.upKey = (uint8_t)r.upKey;
    m.downKey = (uint8_t)r.downKey;
    m.confirmKey = (uint8_t)r.confirmKey;
    m.min = r.min;
    m.max = r.max;
    m.firstUp = r.firstUp;
    m.firstDown = r.firstDown;
    m.sendDelayMs = r.sendDelayMs;
}

// False for a frame whose fields are out of range.
inline bool ruleFromFrame(const QuizRulesMsg& m, uint16_t keyMask, InputRule& out) {
    if (!keyOrNone(m.upKey) || !keyOrNone(m.downKey) || !keyOrNone(m.confirmKey)) return false;
    InputRule r;
    r.mode = (InputMode)m.mode;
    r.firstOnly = (m.flags & kRuleFirstOnly) != 0;
    r.repeat = (m.flags & kRuleRepeat) != 0;
    for (uint8_t i = 0; i < kKeySlots; i++) r.keyValue[i] = m.keyValue[i];
    r.upKey = (Key)m.upKey;
    r.downKey = (Key)m.downKey;
    r.confirmKey = (Key)m.confirmKey;
    r.min = m.min;
    r.max = m.max;
    r.firstUp = m.firstUp;
    r.firstDown = m.firstDown;
    r.sendDelayMs = m.sendDelayMs;
    if (!ruleValid(r, keyMask)) return false;
    out = r;
    return true;
}

}  // namespace game
