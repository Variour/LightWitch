#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FleetConfig.h"
#include "FleetTypes.h"

// Radio frames of the fleet package (fleet protocol v0): status from every
// device, commands from the bridge. Packed, no padding,
// little-endian like the ESP32 and the host. The MsgType values are mirrored
// in src/mesh/MeshTypes.h, which static_asserts that both agree.
namespace fleet {

static constexpr uint8_t kMsgFleetStatus = 44;
static constexpr uint8_t kMsgFleetCommand = 45;
static constexpr uint8_t kProto = 1;  // `proto` in both frames; receivers drop other values

// Device -> broadcast, every kStatusPeriodMs and at once on a change.
struct __attribute__((packed)) FleetStatusMsg {
    uint8_t type;         // kMsgFleetStatus
    uint8_t proto;        // kProto
    uint16_t number;      // device number, 0 = none
    uint8_t state;        // State
    uint8_t battery;      // percent, kNoBattery = unknown
    uint16_t vbatMv;      // 0 = unknown
    uint8_t charge;       // Charge
    uint8_t target;       // Target
    uint8_t flags;        // kFlagUsb | kFlagWifi
    uint32_t fw;          // firmware build: first 4 bytes of the app image's SHA-256
    uint8_t update;       // Update
    uint8_t updatePct;    // 0-100
    uint8_t updateError;  // UpdateError of the last fleet update
    uint16_t errors;      // kErr* bits
};
static_assert(sizeof(FleetStatusMsg) == 20, "FleetStatusMsg layout");

enum class Op : uint8_t {
    Update = 1,  // arg: Source; pr: PR number for Source::Pr
    Charge = 2,  // arg: Target
    Off = 3,     // devices on battery enter ship mode; devices on USB ignore it
    Ap = 4,      // arg: minutes of the maintenance AP
};
enum class Source : uint8_t { Release = 0, Pr = 1 };

// Bridge -> broadcast, sent kCommandRepeats times with the same nonce.
// targetCount 0 addresses every device, otherwise the listed MACs.
struct __attribute__((packed)) FleetCommandMsg {
    uint8_t type;    // kMsgFleetCommand
    uint8_t proto;   // kProto
    uint32_t nonce;  // the same on every repeat; receivers act once per nonce
    uint8_t op;      // Op
    uint8_t arg;
    uint16_t pr;
    uint8_t targetCount;
    uint8_t targets[kMaxTargets][6];
};
static constexpr size_t kCommandHeadLen = offsetof(FleetCommandMsg, targets);
static_assert(kCommandHeadLen == 11, "FleetCommandMsg layout");
static_assert(sizeof(FleetCommandMsg) <= 250, "ESP-NOW v1 payload limit");

inline bool isFleetType(uint8_t type) {
    return type == kMsgFleetStatus || type == kMsgFleetCommand;
}

// Strict framing: FleetStatus needs its exact size, FleetCommand its head
// plus exactly targetCount MACs, targetCount <= kMaxTargets.
inline bool fleetFrameLenOk(const uint8_t* d, size_t len) {
    if (len < 1) return false;
    switch (d[0]) {
        case kMsgFleetStatus:
            return len == sizeof(FleetStatusMsg);
        case kMsgFleetCommand: {
            if (len < kCommandHeadLen) return false;
            uint8_t count = d[offsetof(FleetCommandMsg, targetCount)];
            return count <= kMaxTargets && len == kCommandHeadLen + (size_t)count * 6;
        }
        default:
            return false;
    }
}

inline size_t commandFrameLen(const FleetCommandMsg& m) {
    return kCommandHeadLen + (size_t)m.targetCount * 6;
}

// Copies a validated frame of kind `type` into out (the unused MAC slots of a
// FleetCommandMsg stay zero). False for another kind, bad framing or proto.
template <typename T>
inline bool decodeFleetFrame(const uint8_t* d, size_t len, uint8_t type, T& out) {
    if (len < 2 || d[0] != type || !fleetFrameLenOk(d, len) || len > sizeof(T)) return false;
    if (d[1] != kProto) return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, d, len);
    return true;
}

}  // namespace fleet
