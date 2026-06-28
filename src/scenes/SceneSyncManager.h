#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "../mesh/MeshTypes.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "SceneManager.h"

// Maximum scenes tracked per peer in the manifest cache
static constexpr uint8_t  SYNC_MAX_PEER_SCENES  = 32;
// Maximum concurrent scene fetch requests
static constexpr uint8_t  SYNC_MAX_FETCH_QUEUE  = 8;
// Maximum scene file size accepted over the mesh
static constexpr uint32_t SYNC_MAX_SCENE_BYTES  = 16384;
// How long to wait before retrying a scene request (ms)
static constexpr uint32_t SYNC_REQUEST_RETRY_MS  = 6000;
// Max retries before giving up on a fetch
static constexpr uint8_t  SYNC_MAX_RETRIES       = 3;
// Interval between chunk sends (ms) — avoid flooding ESP-NOW
static constexpr uint32_t SYNC_CHUNK_SEND_INTERVAL_MS = 20;
// How long after a SceneEditPush before broadcasting a follow-up manifest backstop
static constexpr uint32_t SYNC_FOLLOWUP_MANIFEST_MS = 5000;
// Maximum jitter window for manifest broadcasts (ms)
static constexpr uint32_t SYNC_MANIFEST_JITTER_MS = 2000;
// Jitter window for RequestManifest responses (ms)
static constexpr uint32_t SYNC_REQUEST_MANIFEST_JITTER_MS = 1000;
// Concurrent chunk-receive slots
static constexpr uint8_t  SYNC_MAX_RECV_SLOTS = 2;

class SceneSyncManager {
public:
    void setOnSceneSaved(std::function<void(const char*)> cb) { _onSceneSaved = cb; }

    // Called from MeshManager when a SceneManifest packet arrives
    void onManifest(const uint8_t* senderMac, const SceneManifestMsg* msg) {
        if (!Config::get().sceneSyncEnabled) return;
        _updatePeerCache(senderMac, msg);
        _processManifest(senderMac, msg);
    }

    // Called when a SceneRequest arrives — someone wants a scene we might have
    void onRequest(const uint8_t* /*senderMac*/, const char* id) {
        if (!Config::get().sceneSyncEnabled) return;
        if (!LittleFS.exists(SceneManager::path(id))) return;
        _startChunkSend(id);
    }

    // Called when a SceneChunk arrives
    void onChunk(const SceneChunkMsg* msg) {
        if (!Config::get().sceneSyncEnabled) return;
        // Find matching active recv slot
        ChunkRecvState* slot = _findRecv(msg->id);
        if (slot) {
            _handleChunk(slot, msg);
            return;
        }
        // No active slot — start one only if queued via SceneRequest fallback
        if (!_isInFetchQueue(msg->id)) return;
        slot = _allocRecv(msg->id, msg->totalChunks);
        if (slot) _handleChunk(slot, msg);
    }

    // Called when a SceneEditPush arrives — sender saved a scene and is broadcasting chunks
    void onSceneEditPush(const uint8_t* mac, const char* id, uint32_t prevHash) {
        if (!Config::get().sceneSyncEnabled) return;

        // If already receiving this scene, a second push while in-flight is a conflict
        if (_findRecv(id)) {
            Logger::w("[sync] SceneEditPush for %s already in-flight, treating as conflict", id);
            uint32_t localHash = SceneManager::crc32(id);
            _registerConflict(id, localHash, mac, 0);
            return;
        }

        uint32_t localHash = SceneManager::crc32(id);
        bool tombstoned = SceneManager::isTombstone(id);
        bool localAbsent = (localHash == 0) && !tombstoned;

        if (localAbsent || tombstoned) {
            // Scene not in local storage (or tombstoned) → auto-import; edit can resurrect
            Logger::i("[sync] SceneEditPush %s: absent/tombstoned, auto-importing", id);
            _allocRecv(id, 0);  // totalChunks will be filled on first chunk
        } else if (localHash == prevHash) {
            // We were in sync before this edit → auto-apply
            Logger::i("[sync] SceneEditPush %s: prevHash match, auto-applying", id);
            _allocRecv(id, 0);
        } else {
            // Our copy diverged before this edit → conflict; wait for follow-up manifest
            Logger::i("[sync] SceneEditPush %s: conflict (local=%08x prev=%08x), waiting for manifest", id, localHash, prevHash);
        }
    }

    // Called when a SceneForceSet arrives — unconditional push from conflict resolution
    void onForceSet(const char* id, uint32_t hash) {
        if (!Config::get().sceneSyncEnabled) return;
        uint32_t localHash = SceneManager::crc32(id);
        if (localHash == hash) return;  // already have it
        if (hash == 0) {
            SceneManager::remove(id);
            return;
        }
        setForcedAccept(id);
        _enqueueRequest(id);
        Logger::i("[sync] force-set for %s, requesting chunks", id);
    }

    // Called when SetSceneSync targets this device
    void onSetSceneSync(bool enabled) {
        if (Config::get().sceneSyncEnabled == enabled) return;
        bool prev = Config::get().sceneSyncEnabled;
        Config::get().sceneSyncEnabled = enabled;
        Config::save();
        Logger::i("[sync] sceneSyncEnabled set to %d", (int)enabled);
        if (enabled && !prev) onSyncEnabled();
    }

    // Called when a new peer is detected — schedule jittered manifest broadcast
    void onNewPeer(const uint8_t* /*mac*/) {
        if (!Config::get().sceneSyncEnabled) return;
        _scheduleJitteredManifest(SYNC_MANIFEST_JITTER_MS);
    }

    // Called when sceneSyncEnabled is toggled true — synthetic join
    void onSyncEnabled() {
        _scheduleJitteredManifest(SYNC_MANIFEST_JITTER_MS);
        if (_broadcastRequestManifest) _broadcastRequestManifest();
        Logger::i("[sync] sync enabled, broadcasting manifest request");
    }

    // Called when a RequestManifest broadcast arrives — respond with jittered manifest
    void onRequestManifest() {
        if (!Config::get().sceneSyncEnabled) return;
        _scheduleJitteredManifest(SYNC_REQUEST_MANIFEST_JITTER_MS);
    }

    // Scene deleted via web UI — add tombstone and broadcast manifest immediately
    bool deleteScene(const char* id) {
        bool ok = SceneManager::remove(id);
        if (ok) {
            Logger::i("[sync] scene %s deleted, broadcasting manifest", id);
            _broadcastManifestNow = true;
        }
        return ok;
    }

    // Scene created or saved via web UI — broadcast SceneEditPush + start chunk send
    void onSceneChanged(const char* id, uint32_t prevHash) {
        if (!Config::get().sceneSyncEnabled) return;
        Logger::i("[sync] scene %s changed (prevHash=%08x), broadcasting edit push", id, prevHash);
        if (_broadcastEditPush) _broadcastEditPush(id, prevHash);
        _startChunkSend(id);
        // Arm follow-up manifest backstop (~5 s after push)
        _followupManifestMs = millis();
        _followupManifestPending = true;
    }

    // Mark a scene as "accept unconditionally on next receive" — used when the user
    // picks a remote device's copy as the winner during conflict resolution.
    void setForcedAccept(const char* id) {
        strlcpy(_forcedAcceptId, id, 33);
    }

    // Trigger a conflict-resolution force-push for scene `id` using this device's local copy.
    void resolveWithLocal(const char* id) {
        uint32_t hash = SceneManager::crc32(id);
        if (hash == 0) {
            Logger::w("[sync] resolveWithLocal: scene %s not found", id);
            return;
        }
        Logger::i("[sync] resolving conflict for %s with local copy (hash=%08x)", id, hash);
        if (_broadcastForceSet) _broadcastForceSet(id, hash);
        _startChunkSend(id);
    }

    // Periodic tick — call from loop()
    void tick() {
        if (!Config::get().sceneSyncEnabled) return;
        uint32_t now = millis();

        // Jittered manifest broadcast
        if (_jitteredManifestDue && now >= _jitteredManifestAt) {
            _broadcastManifest();
            _jitteredManifestDue = false;
        }

        // Immediate manifest (scene delete etc.)
        if (_broadcastManifestNow) {
            _broadcastManifest();
            _broadcastManifestNow = false;
        }

        // Follow-up backstop manifest 5 s after a SceneEditPush
        if (_followupManifestPending && now - _followupManifestMs >= SYNC_FOLLOWUP_MANIFEST_MS) {
            _broadcastManifest();
            _followupManifestPending = false;
        }

        // Retry pending scene requests
        for (uint8_t i = 0; i < _fetchQueueCount; i++) {
            FetchEntry& e = _fetchQueue[i];
            if (!e.active) continue;
            if (e.retries >= SYNC_MAX_RETRIES) {
                Logger::w("[sync] giving up on scene %s after %u retries", e.id, e.retries);
                e.active = false;
                continue;
            }
            if (now - e.lastRequestMs >= SYNC_REQUEST_RETRY_MS) {
                _broadcastRequest(e.id);
                e.lastRequestMs = now;
                e.retries++;
            }
        }
        _compactFetchQueue();

        // Send next chunk
        if (_send.active && now - _send.lastSendMs >= SYNC_CHUNK_SEND_INTERVAL_MS) {
            _sendNextChunk();
        }

        // Receive timeouts: if a slot receives no chunks for 10 s, fall back to SceneRequest
        for (uint8_t i = 0; i < SYNC_MAX_RECV_SLOTS; i++) {
            ChunkRecvState& s = _recv[i];
            if (!s.active) continue;
            if (now - s.lastChunkMs > 10000) {
                Logger::w("[sync] receive timeout for %s, falling back to SceneRequest", s.id);
                _enqueueRequest(s.id, s.autoApply);
                _resetRecvSlot(s);
            }
        }
    }

    // Fill a JsonDocument with the conflict map for the web API
    void buildConflictsJson(JsonDocument& doc) {
        JsonArray arr = doc["conflicts"].to<JsonArray>();
        for (uint8_t i = 0; i < _conflictCount; i++) {
            Conflict& c = _conflicts[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"] = c.id;
            o["localHash"] = c.localHash;
            JsonArray peers = o["peers"].to<JsonArray>();
            for (uint8_t j = 0; j < PeerRegistry::MAX_PEERS; j++) {
                if (!c.peerHashes[j].active) continue;
                JsonObject p = peers.add<JsonObject>();
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         c.peerHashes[j].mac[0], c.peerHashes[j].mac[1],
                         c.peerHashes[j].mac[2], c.peerHashes[j].mac[3],
                         c.peerHashes[j].mac[4], c.peerHashes[j].mac[5]);
                p["mac"]  = mac;
                p["hash"] = c.peerHashes[j].hash;
            }
        }
    }

    // Build peer scene manifest for the web API (per-peer view)
    void buildPeerScenesJson(JsonDocument& doc) {
        JsonArray arr = doc["peerScenes"].to<JsonArray>();
        for (uint8_t i = 0; i < PeerRegistry::MAX_PEERS; i++) {
            PeerManifest& pm = _peerManifests[i];
            if (!pm.active) continue;
            JsonObject o = arr.add<JsonObject>();
            char mac[18];
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     pm.mac[0], pm.mac[1], pm.mac[2], pm.mac[3], pm.mac[4], pm.mac[5]);
            o["mac"] = mac;
            JsonArray scenes = o["scenes"].to<JsonArray>();
            for (uint8_t j = 0; j < pm.count; j++) {
                JsonObject s = scenes.add<JsonObject>();
                s["id"]   = pm.entries[j].id;
                s["hash"] = pm.entries[j].hash;
            }
        }
    }

    using BroadcastFn    = std::function<void(const char* id, uint32_t hash)>;
    using RequestFn      = std::function<void(const char* id)>;
    using ChunkFn        = std::function<void(const SceneChunkMsg& msg)>;
    using EditPushFn     = std::function<void(const char* id, uint32_t prevHash)>;
    using RequestManFn   = std::function<void()>;

    void setBroadcastFns(BroadcastFn forceSet, RequestFn request, ChunkFn chunk,
                         std::function<void(const SceneManifestMsg&)> manifest) {
        _broadcastForceSet    = forceSet;
        _broadcastRequest     = request;
        _broadcastChunk       = chunk;
        _broadcastManifestMsg = manifest;
    }

    void setEditPushFn(EditPushFn fn)         { _broadcastEditPush      = fn; }
    void setRequestManifestFn(RequestManFn fn) { _broadcastRequestManifest = fn; }

private:
    // ── Per-peer manifest cache ───────────────────────────────────────────────

    struct PeerManifestEntry { char id[33]; uint32_t hash; };

    struct PeerManifest {
        uint8_t           mac[6];
        bool              active = false;
        PeerManifestEntry entries[SYNC_MAX_PEER_SCENES];
        uint8_t           count  = 0;
    };

    PeerManifest _peerManifests[PeerRegistry::MAX_PEERS] = {};

    PeerManifest* _findOrCreatePeerManifest(const uint8_t* mac) {
        for (auto& pm : _peerManifests) {
            if (pm.active && memcmp(pm.mac, mac, 6) == 0) return &pm;
        }
        for (auto& pm : _peerManifests) {
            if (!pm.active) {
                memcpy(pm.mac, mac, 6);
                pm.active = true;
                pm.count  = 0;
                return &pm;
            }
        }
        return nullptr;
    }

    void _updatePeerCache(const uint8_t* mac, const SceneManifestMsg* msg) {
        PeerManifest* pm = _findOrCreatePeerManifest(mac);
        if (!pm) return;
        if (msg->page == 0) pm->count = 0;
        uint8_t safeCount = msg->count < MANIFEST_ENTRIES_PER_MSG ? msg->count : MANIFEST_ENTRIES_PER_MSG;
        for (uint8_t i = 0; i < safeCount && pm->count < SYNC_MAX_PEER_SCENES; i++) {
            const SceneManifestEntry& e = msg->entries[i];
            bool found = false;
            for (uint8_t j = 0; j < pm->count; j++) {
                if (strncmp(pm->entries[j].id, e.id, 33) == 0) {
                    pm->entries[j].hash = e.hash;
                    found = true;
                    break;
                }
            }
            if (!found) {
                strlcpy(pm->entries[pm->count].id, e.id, 33);
                pm->entries[pm->count].hash = e.hash;
                pm->count++;
            }
        }
    }

    // ── Conflict tracking ─────────────────────────────────────────────────────

    struct PeerHash { uint8_t mac[6]; uint32_t hash; bool active; };

    struct Conflict {
        char     id[33];
        uint32_t localHash;
        PeerHash peerHashes[PeerRegistry::MAX_PEERS];
    };

    static constexpr uint8_t MAX_CONFLICTS = 16;
    Conflict _conflicts[MAX_CONFLICTS] = {};
    uint8_t  _conflictCount = 0;

    void _registerConflict(const char* id, uint32_t localHash, const uint8_t* peerMac, uint32_t peerHash) {
        Conflict* c = nullptr;
        for (uint8_t i = 0; i < _conflictCount; i++) {
            if (strncmp(_conflicts[i].id, id, 33) == 0) { c = &_conflicts[i]; break; }
        }
        if (!c) {
            if (_conflictCount >= MAX_CONFLICTS) {
                Logger::w("[sync] conflict table full, dropping conflict for %s", id);
                return;
            }
            c = &_conflicts[_conflictCount++];
            strlcpy(c->id, id, 33);
            memset(c->peerHashes, 0, sizeof(c->peerHashes));
            c->localHash = localHash;
        }
        for (auto& p : c->peerHashes) {
            if (!p.active || memcmp(p.mac, peerMac, 6) == 0) {
                memcpy(p.mac, peerMac, 6);
                p.hash   = peerHash;
                p.active = true;
                return;
            }
        }
    }

    void _clearConflict(const char* id) {
        for (uint8_t i = 0; i < _conflictCount; i++) {
            if (strncmp(_conflicts[i].id, id, 33) == 0) {
                _conflicts[i] = _conflicts[--_conflictCount];
                return;
            }
        }
    }

    // ── Manifest processing ───────────────────────────────────────────────────

    void _processManifest(const uint8_t* senderMac, const SceneManifestMsg* msg) {
        uint8_t safeCount = msg->count < MANIFEST_ENTRIES_PER_MSG ? msg->count : MANIFEST_ENTRIES_PER_MSG;
        for (uint8_t i = 0; i < safeCount; i++) {
            const SceneManifestEntry& e = msg->entries[i];

            // Skip scenes whose chunk receive is currently in-flight
            if (_findRecv(e.id)) continue;

            uint32_t localHash    = SceneManager::crc32(e.id);
            bool     localTombstone = (localHash == 0) && SceneManager::isTombstone(e.id);
            bool     localAbsent    = (localHash == 0) && !localTombstone;

            if (e.hash == 0) {
                // Peer has a tombstone (deleted)
                if (localAbsent || localTombstone) {
                    // Both absent/deleted — nothing to do
                } else {
                    Logger::d("[sync] conflict: peer deleted %s, we have hash=%08x", e.id, localHash);
                    _registerConflict(e.id, localHash, senderMac, 0);
                }
            } else if (localAbsent) {
                // New scene from peer — fetch it
                Logger::i("[sync] new scene %s from peer, fetching", e.id);
                _enqueueRequest(e.id, false);
            } else if (localHash == e.hash) {
                // Identical — no action
            } else {
                // Hash mismatch — always a conflict (stateless; no auto-apply from manifest)
                Logger::d("[sync] conflict: scene %s local=%08x peer=%08x", e.id, localHash, e.hash);
                _registerConflict(e.id, localHash, senderMac, e.hash);
            }
        }
    }

    // ── Fetch queue ───────────────────────────────────────────────────────────

    struct FetchEntry {
        char     id[33];
        bool     active;
        bool     autoAccept;
        uint8_t  retries;
        uint32_t lastRequestMs;
    };

    FetchEntry _fetchQueue[SYNC_MAX_FETCH_QUEUE] = {};
    uint8_t    _fetchQueueCount = 0;

    void _enqueueRequest(const char* id, bool autoAccept = false) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++) {
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0) {
                if (autoAccept) _fetchQueue[i].autoAccept = true;
                return;
            }
        }
        if (_fetchQueueCount >= SYNC_MAX_FETCH_QUEUE) {
            Logger::w("[sync] fetch queue full, dropping request for %s", id);
            return;
        }
        FetchEntry& e = _fetchQueue[_fetchQueueCount++];
        strlcpy(e.id, id, 33);
        e.active        = true;
        e.autoAccept    = autoAccept;
        e.retries       = 0;
        e.lastRequestMs = 0;
    }

    void _compactFetchQueue() {
        uint8_t w = 0;
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active) _fetchQueue[w++] = _fetchQueue[i];
        _fetchQueueCount = w;
    }

    void _removeFetchEntry(const char* id) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0)
                _fetchQueue[i].active = false;
    }

    bool _isFetchAutoAccept(const char* id) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0)
                return _fetchQueue[i].autoAccept;
        return false;
    }

    bool _isInFetchQueue(const char* id) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0)
                return true;
        return false;
    }

    // ── Chunk send ────────────────────────────────────────────────────────────

    struct ChunkSendState {
        char     id[33];
        File     file;
        uint16_t totalChunks;
        uint16_t nextChunk;
        uint32_t lastSendMs;
        bool     active;
    };

    ChunkSendState _send = {};

    void _startChunkSend(const char* id) {
        if (_send.active) {
            if (strncmp(_send.id, id, 33) == 0) return;
            _send.file.close();
        }
        String p = SceneManager::path(id);
        File f = LittleFS.open(p, "r");
        if (!f) { Logger::e("[sync] chunk send: file not found %s", id); return; }
        uint32_t size = f.size();
        if (size == 0) { f.close(); return; }
        strlcpy(_send.id, id, 33);
        _send.file        = f;
        _send.totalChunks = (uint16_t)((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
        _send.nextChunk   = 0;
        _send.lastSendMs  = 0;
        _send.active      = true;
        Logger::i("[sync] starting chunk send for %s (%u bytes, %u chunks)", id, size, _send.totalChunks);
    }

    void _sendNextChunk() {
        if (!_send.active) return;
        SceneChunkMsg msg;
        msg.type        = MsgType::SceneChunk;
        strlcpy(msg.id, _send.id, 33);
        msg.chunkIndex  = _send.nextChunk;
        msg.totalChunks = _send.totalChunks;
        msg.dataLen     = (uint16_t)_send.file.read(msg.data, CHUNK_DATA_SIZE);
        if (msg.dataLen == 0) {
            Logger::w("[sync] chunk send EOF early for %s at chunk %u", _send.id, _send.nextChunk);
            _send.file.close();
            _send.active = false;
            return;
        }
        if (_broadcastChunk) _broadcastChunk(msg);
        _send.lastSendMs = millis();
        _send.nextChunk++;
        if (_send.nextChunk >= _send.totalChunks) {
            Logger::i("[sync] chunk send complete for %s (%u chunks)", _send.id, _send.totalChunks);
            _send.file.close();
            _send.active = false;
        }
    }

    // ── Chunk receive (pool of SYNC_MAX_RECV_SLOTS) ───────────────────────────

    struct ChunkRecvState {
        char      id[33];
        uint8_t*  buffer;
        bool*     received;
        uint16_t  totalChunks;
        uint16_t  gotChunks;
        uint32_t  bufferSize;
        uint32_t  lastChunkMs;
        bool      active;
        bool      autoApply;  // true = accept regardless of local copy
    };

    ChunkRecvState _recv[SYNC_MAX_RECV_SLOTS] = {};

    ChunkRecvState* _findRecv(const char* id) {
        for (auto& s : _recv)
            if (s.active && strncmp(s.id, id, 33) == 0) return &s;
        return nullptr;
    }

    // Allocate a new recv slot; totalChunks=0 means defer until first chunk
    ChunkRecvState* _allocRecv(const char* id, uint16_t totalChunks) {
        for (auto& s : _recv) {
            if (!s.active) {
                memset(&s, 0, sizeof(s));
                strlcpy(s.id, id, 33);
                s.active      = true;
                s.autoApply   = true;
                s.lastChunkMs = millis();
                if (totalChunks > 0) {
                    uint32_t est = (uint32_t)totalChunks * CHUNK_DATA_SIZE;
                    if (est > SYNC_MAX_SCENE_BYTES) {
                        Logger::w("[sync] scene %s too large (%u bytes), skipping", id, est);
                        s.active = false;
                        return nullptr;
                    }
                    s.buffer = (uint8_t*)malloc(est);
                    s.received = (bool*)calloc(totalChunks, sizeof(bool));
                    if (!s.buffer || !s.received) {
                        Logger::e("[sync] OOM allocating recv slot for %s", id);
                        free(s.buffer); free(s.received);
                        s.buffer = nullptr; s.received = nullptr;
                        s.active = false;
                        return nullptr;
                    }
                    s.totalChunks = totalChunks;
                    s.bufferSize  = est;
                }
                Logger::i("[sync] recv slot allocated for %s", id);
                return &s;
            }
        }
        Logger::w("[sync] no recv slot available for %s", id);
        return nullptr;
    }

    void _handleChunk(ChunkRecvState* s, const SceneChunkMsg* msg) {
        // Late-init: first chunk tells us totalChunks
        if (!s->buffer) {
            uint32_t est = (uint32_t)msg->totalChunks * CHUNK_DATA_SIZE;
            if (est > SYNC_MAX_SCENE_BYTES) {
                Logger::w("[sync] scene %s too large (%u bytes), skipping", s->id, est);
                _resetRecvSlot(*s);
                return;
            }
            s->buffer = (uint8_t*)malloc(est);
            s->received = (bool*)calloc(msg->totalChunks, sizeof(bool));
            if (!s->buffer || !s->received) {
                Logger::e("[sync] OOM for %s", s->id);
                _resetRecvSlot(*s);
                return;
            }
            s->totalChunks = msg->totalChunks;
            s->bufferSize  = est;
        }

        if (msg->chunkIndex >= s->totalChunks) return;
        if (s->received[msg->chunkIndex]) return;  // duplicate
        uint32_t offset = (uint32_t)msg->chunkIndex * CHUNK_DATA_SIZE;
        if (offset + msg->dataLen > s->bufferSize) return;
        memcpy(s->buffer + offset, msg->data, msg->dataLen);
        s->received[msg->chunkIndex] = true;
        s->gotChunks++;
        s->lastChunkMs = millis();
        if (s->gotChunks == s->totalChunks) {
            _finaliseReceive(s, msg);
        }
    }

    void _finaliseReceive(ChunkRecvState* s, const SceneChunkMsg* lastMsg) {
        char id[33];
        strlcpy(id, s->id, 33);

        uint32_t totalSize    = (uint32_t)(s->totalChunks - 1) * CHUNK_DATA_SIZE + lastMsg->dataLen;
        uint32_t incomingHash = SceneManager::crc32OfData(s->buffer, totalSize);
        uint32_t localHash    = SceneManager::crc32(id);
        bool forced           = strncmp(id, _forcedAcceptId, 33) == 0 && _forcedAcceptId[0] != 0;
        bool autoAccept       = s->autoApply || forced;

        if (localHash == incomingHash) {
            Logger::i("[sync] received scene %s already matches local, discarding", id);
            _resetRecvSlot(*s);
            _removeFetchEntry(id);
            return;
        }

        if (localHash != 0 && !autoAccept) {
            Logger::i("[sync] received scene %s conflicts with local copy, not saving", id);
            _resetRecvSlot(*s);
            return;
        }

        if (!forced) {
            size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
            if (freeBytes < 20480 && !_isReferencedByGroup(id)) {
                Logger::w("[sync] low storage (%u B free), skipping non-referenced scene %s", freeBytes, id);
                _resetRecvSlot(*s);
                _removeFetchEntry(id);
                return;
            }
        }

        bool ok = SceneManager::saveRaw(id, s->buffer, totalSize);
        Logger::i("[sync] scene %s saved (%u bytes, hash=%08x) ok=%d", id, totalSize, incomingHash, ok);

        if (ok && _onSceneSaved) _onSceneSaved(id);
        _clearConflict(id);

        if (ok && forced) {
            _forcedAcceptId[0] = 0;
            if (_broadcastForceSet) _broadcastForceSet(id, incomingHash);
            _resetRecvSlot(*s);
            _removeFetchEntry(id);
            _startChunkSend(id);
            return;
        }

        _resetRecvSlot(*s);
        _removeFetchEntry(id);
    }

    bool _isReferencedByGroup(const char* id) {
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            GroupConfig* g = Config::group(i);
            if (g && strncmp(g->light.sceneId, id, 33) == 0)
                return true;
        }
        return false;
    }

    void _resetRecvSlot(ChunkRecvState& s) {
        if (s.buffer)   { free(s.buffer);   s.buffer   = nullptr; }
        if (s.received) { free(s.received);  s.received = nullptr; }
        s.active = false;
    }

    // ── Manifest broadcast ────────────────────────────────────────────────────

    void _scheduleJitteredManifest(uint32_t maxJitterMs) {
        uint32_t jitter = (uint32_t)(esp_random() % (maxJitterMs + 1));
        uint32_t fireAt = millis() + jitter;
        if (!_jitteredManifestDue || fireAt < _jitteredManifestAt) {
            _jitteredManifestAt  = fireAt;
            _jitteredManifestDue = true;
        }
    }

    void _broadcastManifest() {
        SceneManager::ManifestEntry allEntries[64];
        uint8_t total = SceneManager::buildManifestEntries(allEntries, 64);
        uint8_t totalPages = (total + MANIFEST_ENTRIES_PER_MSG - 1) / MANIFEST_ENTRIES_PER_MSG;
        if (totalPages == 0) totalPages = 1;

        uint8_t idx = 0;
        for (uint8_t page = 0; page < totalPages; page++) {
            SceneManifestMsg msg;
            msg.type       = MsgType::SceneManifest;
            msg.page       = page;
            msg.totalPages = totalPages;
            msg.count      = 0;
            while (msg.count < MANIFEST_ENTRIES_PER_MSG && idx < total) {
                strlcpy(msg.entries[msg.count].id, allEntries[idx].id, 33);
                msg.entries[msg.count].hash = allEntries[idx].hash;
                msg.count++;
                idx++;
            }
            if (_broadcastManifestMsg) _broadcastManifestMsg(msg);
        }
    }

    // ── State ─────────────────────────────────────────────────────────────────

    bool     _broadcastManifestNow      = false;
    bool     _jitteredManifestDue       = false;
    uint32_t _jitteredManifestAt        = 0;
    bool     _followupManifestPending   = false;
    uint32_t _followupManifestMs        = 0;
    char     _forcedAcceptId[33]        = {};

    BroadcastFn                          _broadcastForceSet;
    RequestFn                            _broadcastRequest;
    ChunkFn                              _broadcastChunk;
    std::function<void(const SceneManifestMsg&)> _broadcastManifestMsg;
    EditPushFn                           _broadcastEditPush;
    RequestManFn                         _broadcastRequestManifest;
    std::function<void(const char*)>     _onSceneSaved;
};
