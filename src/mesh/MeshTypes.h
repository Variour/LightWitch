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
    ConfigChunk    = 12,
    TriggerUpdate  = 13,
    CheckUpdate    = 14,
    SceneEditPush  = 15,
    RequestManifest = 16,
    TimeSync       = 17,
};

enum class FwState : uint8_t { Idle = 0, Checking = 1, Downloading = 2, Error = 3, Done = 4 };

static constexpr uint8_t PRESENCE_MSG_VERSION = 3;

// lightGroupIds: groupId for each light slot; 0xFF means that slot is empty.
struct PresenceMsg {
    MsgType type    = MsgType::Presence;
    uint8_t version = PRESENCE_MSG_VERSION;
    char    name[32];
    uint8_t sceneSyncEnabled;
    uint8_t wifiConnected;
    char    fwVersion[16];
    uint8_t fwState;
    uint8_t lightCount;
    uint8_t lightGroupIds[MAX_LIGHTS];
    char    lightNames[MAX_LIGHTS][20];
};

struct LightConfigMsg {
    MsgType     type;
    uint8_t     groupId;
    LightConfig config;
};

// lightIndex: which light slot on the target device to reassign.
struct SetGroupMsg {
    MsgType type;
    uint8_t targetMac[6];
    uint8_t lightIndex;
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
struct ProximityPingMsg {
    MsgType type    = MsgType::ProximityPing;
    uint8_t groupId;
};

// ── Scene sync messages ───────────────────────────────────────────────────────

struct SceneManifestEntry {
    char     id[SCENE_ID_LEN];
    uint32_t hash;
};

static constexpr uint8_t  MANIFEST_ENTRIES_PER_MSG = 6;

struct SceneManifestMsg {
    MsgType            type       = MsgType::SceneManifest;
    uint8_t            page;
    uint8_t            totalPages;
    uint8_t            count;
    SceneManifestEntry entries[MANIFEST_ENTRIES_PER_MSG];
};

struct SceneRequestMsg {
    MsgType type = MsgType::SceneRequest;
    char    id[SCENE_ID_LEN];
};

static constexpr uint16_t CHUNK_DATA_SIZE = 208;

struct SceneChunkMsg {
    MsgType  type = MsgType::SceneChunk;
    uint8_t  _pad        = 0;
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    char     id[SCENE_ID_LEN];
    uint8_t  data[CHUNK_DATA_SIZE];
};

struct SceneForceSetMsg {
    MsgType  type = MsgType::SceneForceSet;
    char     id[SCENE_ID_LEN];
    uint32_t hash;
};

// Broadcast before chunk stream on any scene save (create or edit).
// prevHash=0 means new scene; prevHash=crc32 of scene before save otherwise.
struct SceneEditPushMsg {
    MsgType  type = MsgType::SceneEditPush;
    char     id[SCENE_ID_LEN];
    uint32_t prevHash;
};

// Broadcast to ask all sync-enabled peers to send their manifest.
struct RequestManifestMsg {
    MsgType type = MsgType::RequestManifest;
};

struct SetSceneSyncMsg {
    MsgType type = MsgType::SetSceneSync;
    uint8_t targetMac[6];
    uint8_t enabled;
};

struct TriggerUpdateMsg {
    MsgType type = MsgType::TriggerUpdate;
    uint8_t targetMac[6];
};

// Tells a specific device to check for a firmware update (no auto-install).
struct CheckUpdateMsg {
    MsgType type = MsgType::CheckUpdate;
    uint8_t targetMac[6];
};
// 7 bytes ✓

// Broadcast periodically by a device with an NTP-synced clock, so peers with
// no internet access (but mesh connectivity) can adopt a wall-clock time.
struct TimeSyncMsg {
    MsgType  type  = MsgType::TimeSync;
    uint32_t epoch;  // unix time (UTC seconds) at the sender
};

// ── Config push messages ──────────────────────────────────────────────────────

static constexpr uint16_t CONFIG_CHUNK_DATA_SIZE = 230;

struct ConfigChunkMsg {
    MsgType  type = MsgType::ConfigChunk;
    uint8_t  targetMac[6];
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    uint8_t  data[CONFIG_CHUNK_DATA_SIZE];
};
