#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Shared enums and small helpers of the game session module. Plain C++ with
// no Arduino dependency, so the protocol and state machines build on the
// host too.
namespace game {

// Key index = bit in QuizState.keyMask and slot in keyColor (fixed v0).
enum class Key : uint8_t {
    Buzz = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
    Enter = 5,
    L1 = 6,
    L2 = 7,
    R1 = 8,
    R2 = 9,
    Power = 10,  // never part of a round
    None = 0xFF,
};
static constexpr uint8_t kKeySlots = 10;  // keys with a keyMask bit

// Palette index in QuizState.keyColor (fixed v0).
enum class Color : uint8_t {
    Off = 0,
    White = 1,
    Red = 2,
    Green = 3,
    Yellow = 4,
    Blue = 5,
    Magenta = 6,
    Cyan = 7,
    Orange = 8,
};
static constexpr uint8_t kColorCount = 9;

enum class RoundType : uint8_t { Idle = 0, Race = 1, Scale = 2, Choice = 3 };
static constexpr uint8_t kRoundTypeCount = 4;
enum class Phase : uint8_t { Announced = 0, Open = 1, Closed = 2 };
static constexpr uint8_t kPhaseCount = 3;

// QuizStatus.flags bits (fixed v0).
static constexpr uint8_t kStatusCharging = 1 << 0;
static constexpr uint8_t kStatusJoin = 1 << 1;
static constexpr uint8_t kStatusPressed = 1 << 2;
static constexpr uint8_t kStatusRejoin = 1 << 3;

static constexpr uint8_t kNoValue = 0xFF;    // echo / report: no value
static constexpr uint8_t kNoBattery = 0xFF;  // battery percent unknown

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

// Firmware colour values for the palette names.
inline Rgb colorRgb(Color c) {
    switch (c) {
        case Color::White:
            return {255, 255, 255};
        case Color::Red:
            return {255, 0, 0};
        case Color::Green:
            return {0, 255, 0};
        case Color::Yellow:
            return {255, 170, 0};
        case Color::Blue:
            return {0, 0, 255};
        case Color::Magenta:
            return {255, 0, 255};
        case Color::Cyan:
            return {0, 255, 255};
        case Color::Orange:
            return {255, 70, 0};
        default:
            return {0, 0, 0};
    }
}

inline Rgb scaleRgb(Rgb c, uint8_t level) {
    return {(uint8_t)(c.r * level / 255), (uint8_t)(c.g * level / 255),
            (uint8_t)(c.b * level / 255)};
}

// ── Names used on the USB link and for key roles ─────────────────────────────
static constexpr const char* kKeyNames[] = {"buzz", "up", "down", "left", "right", "enter",
                                            "l1",   "l2", "r1",   "r2",   "power"};
static constexpr const char* kColorNames[] = {"off",  "white",   "red",  "green", "yellow",
                                              "blue", "magenta", "cyan", "orange"};
static constexpr const char* kRoundTypeNames[] = {"idle", "race", "scale", "choice"};
static constexpr const char* kPhaseNames[] = {"announced", "open", "closed"};

// Maps a key role name ("buzz", "up", ..., "power") to a Key; Key::None if unknown.
inline Key keyFromName(const char* s) {
    if (!s) return Key::None;
    for (uint8_t i = 0; i < sizeof(kKeyNames) / sizeof(kKeyNames[0]); i++)
        if (strcmp(s, kKeyNames[i]) == 0) return (Key)i;
    return Key::None;
}

inline int indexOfName(const char* s, const char* const* names, uint8_t count) {
    if (!s) return -1;
    for (uint8_t i = 0; i < count; i++)
        if (strcmp(s, names[i]) == 0) return i;
    return -1;
}

// Wrap-aware "a is newer than b" for 16-bit counters (stateSeq).
inline bool seq16Newer(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d >= 1 && d <= 0x7FFF;
}

// Wrap-aware "a is newer than b" for valueSeq: (a - b) mod 256 in 1..127 (fixed v0).
inline bool seq8Newer(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(a - b);
    return d >= 1 && d <= 127;
}

// "02:00:00:00:00:11", lowercase (fixed v0). out needs 18 bytes.
inline void formatMac(const uint8_t* mac, char* out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses "aa:bb:cc:dd:ee:ff" (either case). Returns false on any other shape.
inline bool parseMac(const char* s, uint8_t* out) {
    if (!s || strlen(s) != 17) return false;
    for (uint8_t i = 0; i < 6; i++) {
        int hi = hexNibble(s[i * 3]);
        int lo = hexNibble(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':') return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The `id` a link command carried: absent, an integer, or a string of up to 16 chars.
struct ReqId {
    enum class Kind : uint8_t { None, Num, Str };
    Kind kind = Kind::None;
    long long num = 0;
    char str[17] = "";
};

}  // namespace game
