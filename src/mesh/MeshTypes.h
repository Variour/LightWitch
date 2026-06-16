#pragma once
#include <stdint.h>
#include "../config/Config.h"

enum class MsgType : uint8_t {
    Presence      = 1,
    LightConfig   = 2,
    SetGroup      = 3,
    GroupSync     = 4,
    PhaseSync     = 5,
    ProximityPing = 6,
};

struct PresenceMsg {
    MsgType type    = MsgType::Presence;
    char    name[32];
    uint8_t groupId;
};

struct LightConfigMsg {
    MsgType     type;
    uint8_t     groupId;
    LightConfig config;
};

struct SetGroupMsg {
    MsgType type;
    uint8_t targetMac[6];
    uint8_t groupId;
};

// Sent when a group is created, renamed, deleted, or syncEnabled toggled.
// exists=false acts as a tombstone (delete).
struct GroupSyncMsg {
    MsgType     type;
    GroupConfig group;
};

// Periodic phase broadcast from the sync master of a group.
struct PhaseSyncMsg {
    MsgType type    = MsgType::PhaseSync;
    uint8_t groupId;
    float   phase;  // 0.0 – 1.0
};

// Frequent ping sent by devices whose group is in Proximity mode.
// Content is irrelevant — the receiver's promiscuous sniffer reads RSSI from the raw frame.
struct ProximityPingMsg {
    MsgType type    = MsgType::ProximityPing;
    uint8_t groupId;
};
