#pragma once
#include <stdint.h>

#include "../config/Config.h"

// Mesh wire protocol policy:
// - On-wire payloads are the native C++ struct layouts declared in this header.
// - Compatibility is only guaranteed between peers running the same firmware.
// - Mixed-firmware compatibility is intentionally unsupported.
// - PresenceMsg.version is the only explicit schema gate today.

enum class MsgType : uint8_t {
    Presence = 1,
    LightConfig = 2,
    SetGroup = 3,
    GroupSync = 4,
    PhaseSync = 5,
    ProximityPing = 6,
    SceneManifest = 7,
    SceneRequest = 8,
    SceneChunk = 9,
    SceneForceSet = 10,
    SetSceneSync = 11,
    ConfigChunk = 12,
    TriggerUpdate = 13,
    CheckUpdate = 14,
    SceneEditPush = 15,
    RequestManifest = 16,
    TimeSync = 17,
    KeyExchangeInit = 18,
    KeyExchangeResp = 19,
    MeshPolicy = 20,
    WifiRetry = 21,
    MeshSearch = 22,
    GenericEvent = 23,
};

enum class FwState : uint8_t { Idle = 0, Checking = 1, Downloading = 2, Error = 3, Done = 4 };

// Reset to 1 before first real deployment.
static constexpr uint8_t PRESENCE_MSG_VERSION = 2;

// lightGroupIds: groupId for each light slot; 0xFF means that slot is empty.
// Receivers require an exact sizeof(PresenceMsg) frame for this schema.
struct PresenceMsg {
    MsgType type = MsgType::Presence;
    uint8_t version = PRESENCE_MSG_VERSION;
    char name[32];
    uint8_t sceneSyncEnabled;
    uint8_t wifiConnected;
    char fwVersion[16];
    uint8_t fwState;
    uint8_t lightCount;
    uint8_t lightGroupIds[MAX_LIGHTS];
    char lightNames[MAX_LIGHTS][20];
    // Whether this device has ≥1 WiFi network configured, i.e. is a candidate to
    // be the mesh's single WiFi client (see WifiElection.h).
    uint8_t hasWifiNetworks;
    // Whether this device is right now mid-attempt to join a WiFi network
    // (WifiConnectAttempt in flight, either its own election turn or a
    // temporary OTA connect).
    uint8_t wifiConnecting;
    // Battery monitoring (see BatteryMonitor.h) — batteryPercent/batteryCharging
    // are only meaningful when batteryPresent is set.
    uint8_t batteryPresent;
    uint8_t batteryPercent;
    uint8_t batteryCharging;
};

struct LightConfigMsg {
    MsgType type;
    uint8_t groupId;
    LightConfig config;
};

// lightIndex: which light slot on the target device to reassign.
struct SetGroupMsg {
    MsgType type;
    uint8_t targetMac[6];
    uint8_t lightIndex;
    uint8_t groupId;
};

// Sent when a group is created, renamed, deleted, syncEnabled toggled, or its
// light changed; replayed to newly-seen peers and periodically re-advertised
// (see MeshManager::tick) so an already-known peer that missed an update
// self-heals. exists=false acts as a tombstone (delete). The whole group —
// name/exists/syncEnabled/light together — is reconciled as one unit by
// GroupConfig::revision+originMac (see Config::applyGroupSync).
struct GroupSyncMsg {
    MsgType type;
    GroupConfig group;
};

// Periodic phase broadcast from the sync master of a group.
struct PhaseSyncMsg {
    MsgType type = MsgType::PhaseSync;
    uint8_t groupId;
    float phase;  // 0.0 – 1.0
};

// Frequent ping sent by devices whose group is in Proximity mode.
struct ProximityPingMsg {
    MsgType type = MsgType::ProximityPing;
    uint8_t groupId;
};

// ── Scene sync messages ───────────────────────────────────────────────────────

struct SceneManifestEntry {
    char id[SCENE_ID_LEN];
    uint32_t hash;
};

static constexpr uint8_t MANIFEST_ENTRIES_PER_MSG = 6;

struct SceneManifestMsg {
    MsgType type = MsgType::SceneManifest;
    uint8_t page;
    uint8_t totalPages;
    uint8_t count;
    SceneManifestEntry entries[MANIFEST_ENTRIES_PER_MSG];
};

struct SceneRequestMsg {
    MsgType type = MsgType::SceneRequest;
    char id[SCENE_ID_LEN];
};

static constexpr uint16_t CHUNK_DATA_SIZE = 208;

struct SceneChunkMsg {
    MsgType type = MsgType::SceneChunk;
    uint8_t _pad = 0;
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    char id[SCENE_ID_LEN];
    uint8_t data[CHUNK_DATA_SIZE];
};

struct SceneForceSetMsg {
    MsgType type = MsgType::SceneForceSet;
    char id[SCENE_ID_LEN];
    uint32_t hash;
};

// Broadcast before chunk stream on any scene save (create or edit).
// prevHash=0 means new scene; prevHash=crc32 of scene before save otherwise.
struct SceneEditPushMsg {
    MsgType type = MsgType::SceneEditPush;
    char id[SCENE_ID_LEN];
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
    MsgType type = MsgType::TimeSync;
    uint32_t epoch;  // unix time (UTC seconds) at the sender
};

// ── Config push messages ──────────────────────────────────────────────────────

static constexpr uint16_t CONFIG_CHUNK_DATA_SIZE = 230;

struct ConfigChunkMsg {
    MsgType type = MsgType::ConfigChunk;
    uint8_t targetMac[6];
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    uint8_t
        data[CONFIG_CHUNK_DATA_SIZE];  // AES-256-GCM ciphertext once reassembled, see MeshCrypto.h
};

// ── Config push encryption handshake (issue #252) ─────────────────────────────
// Ephemeral X25519 ECDH per push: the two devices involved exchange one-time
// public keys to derive a forward-secret AES key for that single push, so no
// pre-shared key or pairing step is needed and secrets never cross the air in
// the clear. Sent over the same broadcast-with-targetMac-filter transport as
// SetGroupMsg/TriggerUpdateMsg above.

// mbedtls's ECDH public-key export format for Curve25519 is a 1-byte length
// prefix (always 0x20) followed by the 32-byte point (RFC 8422 §5.4).
static constexpr uint8_t ECDH_PUBKEY_LEN = 33;

struct KeyExchangeInitMsg {
    MsgType type = MsgType::KeyExchangeInit;
    uint8_t targetMac[6];
    uint32_t sessionId;
    uint8_t pubKey[ECDH_PUBKEY_LEN];
};

struct KeyExchangeRespMsg {
    MsgType type = MsgType::KeyExchangeResp;
    uint8_t targetMac[6];  // = initiator's MAC
    uint32_t sessionId;
    uint8_t pubKey[ECDH_PUBKEY_LEN];
};

// ── Mesh-wide WiFi policy ──────────────────────────────────────────────────────
// Broadcast whenever a device changes the "single WiFi client" mesh setting via
// its web UI, so the choice applies to the whole mesh rather than just the
// device it was changed on. See WifiElection.h for the election this enables.
struct MeshPolicyMsg {
    MsgType type = MsgType::MeshPolicy;
    uint8_t wifiSingleClientMode;
    uint32_t revision;
    uint8_t originMac[6];
};

// Broadcast by a "Retry WiFi" UI button so every mesh device — including any
// stuck in WifiElection::State::GaveUp — takes a fresh, immediate shot at
// connecting instead of waiting for the mesh-wide policy to be toggled off
// and back on. No payload: every recipient just re-evaluates its own state.
struct WifiRetryMsg {
    MsgType type = MsgType::WifiRetry;
};

// Broadcast by a "Search devices" UI click so every mesh device re-searches
// together, not just the one whose web UI the click happened on — clicking
// on a single device only ever moves that one device, which doesn't reconcile
// a mesh that's already split into islands (#321). No payload: every
// recipient re-searches starting from its own current channel, see
// ChannelManager::beginSearch.
struct MeshSearchMsg {
    MsgType type = MsgType::MeshSearch;
};

// Broadcast primitive for "event X happened on this device" — no meaning is
// baked into the mesh layer, the meaning is entirely defined by whichever
// feature uses it (e.g. buzzer press/reset events). Broadcast only, no
// ACK/sequence number, consistent with WifiRetryMsg/MeshSearchMsg above.
// Sender identity is not embedded in the payload: consumers resolve it via
// the mac passed to the recv callback plus the existing PeerRegistry/
// PresenceMsg name.
static constexpr uint8_t EVENT_TYPE_LEN = 33;  // same convention as SCENE_ID_LEN

struct GenericEventMsg {
    MsgType type = MsgType::GenericEvent;
    char eventType[EVENT_TYPE_LEN];  // e.g. "buzz.press", "buzz.reset" — opaque to the mesh layer
    uint16_t payload;
};
