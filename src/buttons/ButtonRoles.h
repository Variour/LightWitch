#pragma once
#include <string.h>

// Stable key roles (ButtonHardwareConfig::role). Other modules find a key by
// its role instead of by its slot index or GPIO, so a key can move to another
// pin or slot without breaking its consumers. An empty role means the button
// has no fixed function. Each non-empty role belongs to at most one button.
namespace ButtonRole {
constexpr char BUZZ[] = "buzz";
constexpr char UP[] = "up";
constexpr char DOWN[] = "down";
constexpr char LEFT[] = "left";
constexpr char RIGHT[] = "right";
constexpr char ENTER[] = "enter";
constexpr char L1[] = "l1";
constexpr char L2[] = "l2";
constexpr char R1[] = "r1";
constexpr char R2[] = "r2";
constexpr char POWER[] = "power";

constexpr const char* ALL[] = {BUZZ, UP, DOWN, LEFT, RIGHT, ENTER, L1, L2, R1, R2, POWER};

// True for "" (no role) and for every role in ALL.
inline bool isValid(const char* role) {
    if (role == nullptr || role[0] == '\0') return true;
    for (const char* r : ALL)
        if (strcmp(r, role) == 0) return true;
    return false;
}
}  // namespace ButtonRole
