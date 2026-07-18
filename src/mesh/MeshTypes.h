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
    AudioGroupSync = 23,
    SetPlaylistSync = 24,
    PlaylistManifest = 25,
    PlaylistRequest = 26,
    PlaylistChunk = 27,
    PlaylistForceSet = 28,
    PlaylistEditPush = 29,
    RequestPlaylistManifest = 30,
    PlayAudio = 31,
    StopAudio = 32,
    SetSoundGroup = 33,
    SetVolume = 34,
};

enum class FwState : uint8_t { Idle = 0, Checking = 1, Downloading = 2, Error = 3, Done = 4 };

// Reset to 1 before first real deployment.
static constexpr uint8_t PRESENCE_MSG_VERSION = 3;

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
    // Sound output advertisement — mirrors lightGroupIds/lightNames but for the
    // one sound output a device may have (see MAX_SOUNDS), so peers can render
    // a "Connected Speakers" dashboard table and offer cross-device audio-group
    // assignment/volume control the same way they do for lights (see
    // SetSoundGroupMsg/SetVolumeMsg below). soundAudioGroupId/soundVolume are
    // only meaningful when hasSound is set.
    uint8_t hasSound;
    uint8_t soundAudioGroupId;
    uint8_t soundVolume;
    char soundName[20];
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

// Broadcast to move a specific peer's sound output to a different audio group
// — mirrors SetGroupMsg for lights (no light index equivalent since MAX_SOUNDS
// is 1). Every receiving device applies it if targeted and updates its own
// peer-registry cache of the target's group either way, same pattern as
// SetGroupMsg (see MeshManager::setOnSetSoundGroup).
struct SetSoundGroupMsg {
    MsgType type = MsgType::SetSoundGroup;
    uint8_t targetMac[6];
    uint8_t audioGroupId;
};

// Broadcast to change a specific peer's sound output volume from any device's
// dashboard — cross-device control, unlike SoundHardwareConfig::volume being
// merely a local default. Mirrors SetSoundGroupMsg.
struct SetVolumeMsg {
    MsgType type = MsgType::SetVolume;
    uint8_t targetMac[6];
    uint8_t volume;
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

// ── Audio group sync ──────────────────────────────────────────────────────────
// Mirrors GroupSyncMsg for AudioGroupConfig: sent when an audio group is
// created, renamed, or deleted; replayed to newly-seen peers and periodically
// re-advertised (see MeshManager::tick) so an already-known peer that missed
// an update self-heals. exists=false acts as a tombstone (delete). Unlike
// GroupSyncMsg there is no per-group payload beyond identity — see
// AudioGroupConfig.
struct AudioGroupSyncMsg {
    MsgType type = MsgType::AudioGroupSync;
    AudioGroupConfig group;
};

// ── Playlist sync messages ────────────────────────────────────────────────────
// Playlists are small named metadata (an ordered list of SD-card filenames + a
// loop flag) synced over mesh the same way scenes are — see SceneManifestMsg/
// SceneRequestMsg/SceneChunkMsg/SceneForceSetMsg/SceneEditPushMsg/
// RequestManifestMsg/SetSceneSyncMsg for the mirrored pattern. Unlike scenes,
// only this metadata travels over the mesh — the audio files a playlist
// references are never distributed this way (manual per-device upload only,
// see docs/audio-playback.md); a device missing a referenced file simply
// doesn't participate when that playlist is played.

// Playlist id buffer size, incl. null terminator — same convention as SCENE_ID_LEN.
static constexpr uint8_t PLAYLIST_ID_LEN = 33;

struct PlaylistManifestEntry {
    char id[PLAYLIST_ID_LEN];
    uint32_t hash;
};

static constexpr uint8_t PLAYLIST_MANIFEST_ENTRIES_PER_MSG = 6;

struct PlaylistManifestMsg {
    MsgType type = MsgType::PlaylistManifest;
    uint8_t page;
    uint8_t totalPages;
    uint8_t count;
    PlaylistManifestEntry entries[PLAYLIST_MANIFEST_ENTRIES_PER_MSG];
};

struct PlaylistRequestMsg {
    MsgType type = MsgType::PlaylistRequest;
    char id[PLAYLIST_ID_LEN];
};

static constexpr uint16_t PLAYLIST_CHUNK_DATA_SIZE = 208;

struct PlaylistChunkMsg {
    MsgType type = MsgType::PlaylistChunk;
    uint8_t _pad = 0;
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint16_t dataLen;
    char id[PLAYLIST_ID_LEN];
    uint8_t data[PLAYLIST_CHUNK_DATA_SIZE];
};

struct PlaylistForceSetMsg {
    MsgType type = MsgType::PlaylistForceSet;
    char id[PLAYLIST_ID_LEN];
    uint32_t hash;
};

// Broadcast before chunk stream on any playlist save (create or edit).
// prevHash=0 means new playlist; prevHash=crc32 of playlist before save otherwise.
struct PlaylistEditPushMsg {
    MsgType type = MsgType::PlaylistEditPush;
    char id[PLAYLIST_ID_LEN];
    uint32_t prevHash;
};

// Broadcast to ask all playlist-sync-enabled peers to send their manifest.
struct RequestPlaylistManifestMsg {
    MsgType type = MsgType::RequestPlaylistManifest;
};

struct SetPlaylistSyncMsg {
    MsgType type = MsgType::SetPlaylistSync;
    uint8_t targetMac[6];
    uint8_t enabled;
};

// ── Playback triggers ─────────────────────────────────────────────────────────
// Broadcast once to (re)start or stop synchronized playback across an audio
// group — a one-shot event, not replicated/reconciled state (see
// docs/audio-playback.md: "no continuous resync" and "no cross-device
// playback state" design decisions). Every device whose
// SoundHardwareConfig::audioGroupId == audioGroupId independently decides
// whether to participate: it must locally have a file (isPlaylist=0,
// `filename` on its SD card) or a playlist (isPlaylist=1, `id`, whose entries
// it must all locally have) — no readiness handshake, no distribution; a
// device missing what it needs silently doesn't participate.
//
// Participating devices open/buffer the content immediately on receipt, then
// start output exactly startDelayMs after their own local receipt time — not
// a shared wall clock, since ESP-NOW's broadcast is single-hop with low,
// roughly-symmetric latency, so devices' receipt times differ by only a few
// ms. A device that can't finish preparing before its scheduled start skips
// the trigger rather than starting late (fail closed, not fail late). There
// is no periodic re-sync afterward — long loops are expected to drift, see
// docs/audio-playback.md.
static constexpr uint8_t AUDIO_FILENAME_LEN = 64;

struct PlayAudioMsg {
    MsgType type = MsgType::PlayAudio;
    uint8_t audioGroupId;
    uint8_t isPlaylist;  // 0 = single file (filename below), 1 = playlist (id below)
    uint8_t loop;
    uint16_t startDelayMs;
    char id[PLAYLIST_ID_LEN];           // playlist id, meaningful when isPlaylist
    char filename[AUDIO_FILENAME_LEN];  // SD filename, meaningful when !isPlaylist
};

// Broadcast once to stop playback on every device in the target audio group,
// regardless of what each is currently doing (idempotent no-op if not playing).
struct StopAudioMsg {
    MsgType type = MsgType::StopAudio;
    uint8_t audioGroupId;
};
