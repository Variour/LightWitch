#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "GameTypes.h"

// Orientation ("Lage") of a Buzzerboard in a game session, the round field
// `hold`, and the logical inputs (firmware spec "Lage v0"). Directions are as
// seen by the person holding the device. Plain C++, no sensor access: the
// Arduino side feeds the LSM6DS3TR-C's 6D position into LageFilter.
namespace game {

// The four orientations. Unknown: none accepted yet; the device then
// behaves as Hochkant.
enum class Lage : uint8_t {
    Unknown = 0,
    Hochkant = 1,   // held upright in one hand, USB-C at the top
    Quer = 2,       // landscape like a game controller, buzzer right, LED column at the top
    QuerLinks = 3,  // landscape, buzzer left, LED column at the bottom
    Liegend = 4,    // flat on a table, front up
};
static constexpr uint8_t kLageCount = 5;
static constexpr const char* kLageNames[] = {"", "hochkant", "quer", "quer-links", "liegend"};

// Name on the USB link, nullptr for Unknown or an out-of-range value.
inline const char* lageName(uint8_t l) {
    return l >= 1 && l < kLageCount ? kLageNames[l] : nullptr;
}

// Round field `hold`.
enum class Hold : uint8_t {
    Frei = 0,      // the device follows its orientation
    Hochkant = 1,  // required orientation; locked in any other
    Quer = 2,      // required; quer-links counts as quer
    Liegend = 3,   // required
    Fest = 4,      // the orientation at the open stays for the whole round
};
static constexpr uint8_t kHoldCount = 5;
static constexpr const char* kHoldNames[] = {"frei", "hochkant", "quer", "liegend", "fest"};

// The orientation a hold requires, Unknown for frei and fest.
inline Lage requiredLage(Hold h) {
    switch (h) {
        case Hold::Hochkant:
            return Lage::Hochkant;
        case Hold::Quer:
            return Lage::Quer;
        case Hold::Liegend:
            return Lage::Liegend;
        default:
            return Lage::Unknown;
    }
}

// Unknown behaves as Hochkant.
inline Lage effectiveLage(Lage l) { return l == Lage::Unknown ? Lage::Hochkant : l; }

// Whether a device in orientation l meets the required one; quer-links counts as quer.
inline bool lageFits(Lage l, Lage required) {
    return l == required || (required == Lage::Quer && l == Lage::QuerLinks);
}

// Logical inputs. Up, right, down and left are the round's direction keys
// (Key::Up etc. in keyMask and keyColor); buzz and enter stay on their keys.
enum class Input : uint8_t { None, Up, Right, Down, Left, TriggerLeft, TriggerRight, Buzz, Enter };

// Which physical key gives each logical input, per orientation (firmware spec
// "Lage v0", section 2). Key::None: no key.
struct LageKeys {
    Key up, right, down, left, triggerLeft, triggerRight;
};
static constexpr LageKeys kLageKeys[kLageCount] = {
    // Unknown: as hochkant
    {Key::Up, Key::Right, Key::Down, Key::Left, Key::None, Key::None},
    // hochkant
    {Key::Up, Key::Right, Key::Down, Key::Left, Key::None, Key::None},
    // quer: the device turned a quarter clockwise
    {Key::Left, Key::Up, Key::Right, Key::Down, Key::L2, Key::L1},
    // quer-links: a quarter counter-clockwise
    {Key::Right, Key::Down, Key::Left, Key::Up, Key::R1, Key::R2},
    // liegend
    {Key::Up, Key::Right, Key::Down, Key::Left, Key::None, Key::None},
};

inline const LageKeys& lageKeys(Lage l) {
    return kLageKeys[(uint8_t)l < kLageCount ? (uint8_t)l : 0];
}

// The physical key of a logical input in orientation l.
inline Key physicalKey(Lage l, Input in) {
    const LageKeys& m = lageKeys(l);
    switch (in) {
        case Input::Up:
            return m.up;
        case Input::Right:
            return m.right;
        case Input::Down:
            return m.down;
        case Input::Left:
            return m.left;
        case Input::TriggerLeft:
            return m.triggerLeft;
        case Input::TriggerRight:
            return m.triggerRight;
        case Input::Buzz:
            return Key::Buzz;
        case Input::Enter:
            return Key::Enter;
        default:
            return Key::None;
    }
}

// The logical input of physical key k in orientation l; None if k has none.
inline Input logicalInput(Lage l, Key k) {
    if (k == Key::None) return Input::None;
    for (uint8_t i = (uint8_t)Input::Up; i <= (uint8_t)Input::Enter; i++)
        if (physicalKey(l, (Input)i) == k) return (Input)i;
    return Input::None;
}

// The round's key slot of a logical input (keyMask bit, keyColor, choice
// answer); Key::None for the triggers, which no round uses.
inline Key roundKey(Input in) {
    switch (in) {
        case Input::Up:
            return Key::Up;
        case Input::Right:
            return Key::Right;
        case Input::Down:
            return Key::Down;
        case Input::Left:
            return Key::Left;
        case Input::Buzz:
            return Key::Buzz;
        case Input::Enter:
            return Key::Enter;
        default:
            return Key::None;
    }
}

inline Input inputOf(Key roundSlot) {
    switch (roundSlot) {
        case Key::Up:
            return Input::Up;
        case Key::Right:
            return Input::Right;
        case Key::Down:
            return Input::Down;
        case Key::Left:
            return Input::Left;
        case Key::Buzz:
            return Input::Buzz;
        case Key::Enter:
            return Input::Enter;
        default:
            return Input::None;
    }
}

// The LED column fills in reading direction: from LED1 (bottom in hochkant,
// left in quer), reversed in quer-links.
inline bool columnReversed(Lage l) { return effectiveLage(l) == Lage::QuerLinks; }

// ── Change animation ─────────────────────────────────────────────────────────
// The direction keys clockwise as seen from the front: UP, RIGHT, DOWN, LEFT.
static constexpr Key kKeyCircle[4] = {Key::Up, Key::Right, Key::Down, Key::Left};

inline uint8_t circleIndex(Key k) {
    for (uint8_t i = 0; i < 4; i++)
        if (kKeyCircle[i] == k) return i;
    return 0;
}

// Quarter turns between the hochkant mapping and l's: logical direction j
// (0 up, 1 right, 2 down, 3 left) sits on kKeyCircle[(j + rotation) % 4].
inline uint8_t lageRotation(Lage l) { return circleIndex(physicalKey(l, Input::Up)); }

// The direction colours run once around the circle and on to their keys in
// the new orientation, one key per kLageAnimStepMs, the short way round.
struct LageAnimPlan {
    uint8_t steps;  // 4-6
    int8_t dir;     // +1 clockwise, -1 counter-clockwise
};

inline LageAnimPlan lageAnimPlan(Lage from, Lage to) {
    uint8_t d = (uint8_t)((lageRotation(to) + 4 - lageRotation(from)) % 4);
    if (d == 3) return {5, -1};
    return {(uint8_t)(4 + d), +1};
}

// Key circle phase, then one run through the column.
inline uint32_t lageAnimKeysMs(Lage from, Lage to) {
    return lageAnimPlan(from, to).steps * kLageAnimStepMs;
}
inline uint32_t lageAnimMs(Lage from, Lage to) {
    return lageAnimKeysMs(from, to) + kLageAnimColumnMs;
}

// ── Sensor ───────────────────────────────────────────────────────────────────
// LSM6DS3TR-C D6D_SRC (1Dh) position bits (ST DocID030071 Rev 3, Table 84).
// High: the axis points up (reads about +1 g), low: down.
static constexpr uint8_t kD6dXL = 1 << 0;
static constexpr uint8_t kD6dXH = 1 << 1;
static constexpr uint8_t kD6dYL = 1 << 2;
static constexpr uint8_t kD6dYH = 1 << 3;
static constexpr uint8_t kD6dZL = 1 << 4;
static constexpr uint8_t kD6dZH = 1 << 5;
static constexpr uint8_t kD6dPositionMask = 0x3F;

// The orientation for each 6D position. UNVERIFIED: check on the first
// boards (bring-up), one position at a time.
//
// Derived from the routed board (buzzerboard hardware/buzzerboard.kicad_pcb)
// and the datasheet. U5 sits on B.Cu at (47.403, 104.144), rotation 180; pad 1
// (SDO/SA0, +3V3) at (48.566, 103.394), pads 2-4 below it in +y, pad 14 (SDA)
// to its left at x 47.903 (board coordinates seen from the front, x right,
// y down toward the bottom edge, USB-C at y 0). Seen from the back, that is
// pin 1 top left, pins 1-4 down the left side, pin 14 right of pin 1: the
// datasheet's top view (Figure 1), in which +X points right, +Y up and +Z
// out of the package top. So, as seen from the front in hochkant:
//   +X = toward the left edge (the LED column), +Y = toward USB-C,
//   +Z = out of the back.
struct D6dLage {
    uint8_t bit;
    Lage lage;
};
static constexpr D6dLage kD6dLage[6] = {
    {kD6dYH, Lage::Hochkant},   // USB-C up
    {kD6dXH, Lage::Quer},       // LED column up
    {kD6dXL, Lage::QuerLinks},  // LED column down
    {kD6dZL, Lage::Liegend},    // front up
    {kD6dYL, Lage::Unknown},    // upside down: no orientation
    {kD6dZH, Lage::Unknown},    // front down: no orientation
};

// The orientation of a D6D_SRC value: exactly one position bit, else Unknown.
inline Lage lageFromD6d(uint8_t src) {
    uint8_t pos = src & kD6dPositionMask;
    for (const auto& e : kD6dLage)
        if (pos == e.bit) return e.lage;
    return Lage::Unknown;
}

// Accepts a new orientation once the sensor reported it for kLageSettleMs
// without a change. Unknown (upside down, front down, no position) is no
// orientation: the accepted one stays, a pending one is dropped.
class LageFilter {
   public:
    void onRaw(Lage raw, uint32_t now) {
        if (raw == Lage::Unknown || raw == _accepted) {
            _candidate = Lage::Unknown;
            return;
        }
        if (raw != _candidate) {
            _candidate = raw;
            _since = now;
        }
    }

    // True once when the pending orientation is accepted.
    bool tick(uint32_t now) {
        if (_candidate == Lage::Unknown || now - _since < kLageSettleMs) return false;
        _accepted = _candidate;
        _candidate = Lage::Unknown;
        return true;
    }

    Lage accepted() const { return _accepted; }

   private:
    Lage _accepted = Lage::Unknown;
    Lage _candidate = Lage::Unknown;
    uint32_t _since = 0;
};

}  // namespace game
