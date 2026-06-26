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
    SceneManifest = 7,
    SceneRequest  = 8,
    SceneChunk    = 9,
    SceneForceSet = 10,
    SetSceneSync  = 11,
};

struct PresenceMsg {
    MsgType type    = MsgType::Presence;
    char    name[32];
    uint8_t groupId;
    uint8_t sceneSyncEnabled;  // 1 = enabled, 0 = disabled  (v1 ends here — 35 bytes)
    uint8_t wifiConnected;     // v2: 1 = connected to WiFi, 0 = not
    char    fwVersion[16];     // v2: null-terminated firmware version string
};
static constexpr size_t PRESENCE_MSG_V1_SIZE = 35;

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

// ── Scene sync messages ───────────────────────────────────────────────────────

// One entry in a SceneManifest: scene ID + CRC32 of file content.
// hash == 0 is the deletion sentinel (tombstone).
struct SceneManifestEntry {
    char     id[33];
    uint32_t hash;
};

// Fits 6 entries per 250-byte ESP-NOW packet (4 header + 6×37 = 226 bytes).
static constexpr uint8_t  MANIFEST_ENTRIES_PER_MSG = 6;

struct SceneManifestMsg {
    MsgType            type       = MsgType::SceneManifest;
    uint8_t            page;        // 0-based page index
    uint8_t            totalPages;
    uint8_t            count;       // entries in this packet (≤ MANIFEST_ENTRIES_PER_MSG)
    SceneManifestEntry entries[MANIFEST_ENTRIES_PER_MSG];
};
// 4 + 6*37 = 226 bytes ✓

struct SceneRequestMsg {
    MsgType type = MsgType::SceneRequest;
    char    id[33];
};
// 34 bytes ✓

// Reordered to avoid compiler padding: numeric fields before char array.
// 1(type) + 1(pad) + 2 + 2 + 2 + 33(id) + 208(data) = 249 bytes.
static constexpr uint16_t CHUNK_DATA_SIZE = 208;

struct SceneChunkMsg {
    MsgType  type = MsgType::SceneChunk;
    uint8_t  _pad        = 0;   // explicit pad so uint16_t fields are 2-byte aligned
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    char     id[33];
    uint8_t  data[CHUNK_DATA_SIZE];
};
// 249 bytes, no implicit padding ✓

// Resolves a conflict: all devices must adopt this scene unconditionally.
// The sender immediately begins broadcasting SceneChunk packets.
struct SceneForceSetMsg {
    MsgType  type = MsgType::SceneForceSet;
    char     id[33];
    uint32_t hash;  // expected hash of the canonical content
};
// 38 bytes ✓

// Remotely toggle sceneSyncEnabled on a specific device (targeted by MAC).
struct SetSceneSyncMsg {
    MsgType type = MsgType::SetSceneSync;
    uint8_t targetMac[6];
    uint8_t enabled;  // 1 = enable, 0 = disable
};
// 8 bytes ✓
