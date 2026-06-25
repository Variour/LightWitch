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
// How often to broadcast our manifest (ms)
static constexpr uint32_t SYNC_MANIFEST_INTERVAL_MS = 60000;
// How long to wait before retrying a scene request (ms)
static constexpr uint32_t SYNC_REQUEST_RETRY_MS  = 6000;
// Max retries before giving up on a fetch
static constexpr uint8_t  SYNC_MAX_RETRIES       = 3;
// Interval between chunk sends (ms) — avoid flooding ESP-NOW
static constexpr uint32_t SYNC_CHUNK_SEND_INTERVAL_MS = 20;

class SceneSyncManager {
public:
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
        if (!_recv.active || strncmp(_recv.id, msg->id, 33) != 0) {
            // Not assembling this scene; start if we need it
            if (!_isNeeded(msg->id)) return;
            if (_recv.active) return;  // busy assembling something else
            _startReceive(msg);
            return;
        }
        _handleChunk(msg);
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
        _enqueueRequest(id);
        Logger::i("[sync] force-set for %s, requesting chunks", id);
    }

    // Called when SetSceneSync targets this device
    void onSetSceneSync(bool enabled) {
        if (Config::get().sceneSyncEnabled == enabled) return;
        Config::get().sceneSyncEnabled = enabled;
        Config::save();
        Logger::i("[sync] sceneSyncEnabled set to %d", (int)enabled);
    }

    // Called when a new peer is detected — send our manifest immediately
    void onNewPeer() {
        if (Config::get().sceneSyncEnabled)
            _lastManifestBroadcast = 0;  // force immediate broadcast on next tick
    }

    // Scene deleted via web UI — add tombstone and broadcast manifest
    bool deleteScene(const char* id) {
        bool ok = SceneManager::remove(id);
        if (ok) {
            Logger::i("[sync] scene %s deleted, broadcasting manifest", id);
            _broadcastManifestNow = true;
        }
        return ok;
    }

    // Scene created or saved via web UI — broadcast updated manifest
    void onSceneChanged() {
        _broadcastManifestNow = true;
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
        // Start broadcasting chunks immediately so peers can collect them
        _startChunkSend(id);
    }

    // Periodic tick — call from loop()
    void tick() {
        if (!Config::get().sceneSyncEnabled) return;
        uint32_t now = millis();

        // Manifest broadcast
        if (_broadcastManifestNow || now - _lastManifestBroadcast >= SYNC_MANIFEST_INTERVAL_MS) {
            _broadcastManifest();
            _lastManifestBroadcast  = now;
            _broadcastManifestNow   = false;
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

        // Receive timeout: abandon assembly if no chunk for 10 s
        if (_recv.active && now - _recv.lastChunkMs > 10000) {
            Logger::w("[sync] receive timeout for %s, abandoning", _recv.id);
            _resetReceive();
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

    using BroadcastFn = std::function<void(const char* id, uint32_t hash)>;
    using RequestFn   = std::function<void(const char* id)>;
    using ChunkFn     = std::function<void(const SceneChunkMsg& msg)>;

    void setBroadcastFns(BroadcastFn forceSet, RequestFn request, ChunkFn chunk,
                         std::function<void(const SceneManifestMsg&)> manifest) {
        _broadcastForceSet = forceSet;
        _broadcastRequest  = request;
        _broadcastChunk    = chunk;
        _broadcastManifestMsg = manifest;
    }

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
        // On page 0 reset the cache so stale entries from previous manifests are cleared
        if (msg->page == 0) pm->count = 0;
        uint8_t safeCount = msg->count < MANIFEST_ENTRIES_PER_MSG ? msg->count : MANIFEST_ENTRIES_PER_MSG;
        for (uint8_t i = 0; i < safeCount && pm->count < SYNC_MAX_PEER_SCENES; i++) {
            const SceneManifestEntry& e = msg->entries[i];
            // Update or append
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
        // Update peer entry
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
            uint32_t localHash = SceneManager::crc32(e.id);
            bool localTombstone = (localHash == 0) && SceneManager::isTombstone(e.id);
            bool localAbsent    = (localHash == 0) && !localTombstone;

            if (e.hash == 0) {
                // Peer has a tombstone (deleted)
                if (localAbsent || (localHash == 0 && localTombstone)) {
                    // Both absent/deleted — nothing to do
                } else {
                    // I have it, peer deleted it → conflict
                    Logger::d("[sync] conflict: peer deleted %s, we have hash=%08x", e.id, localHash);
                    _registerConflict(e.id, localHash, senderMac, 0);
                }
            } else if (localAbsent) {
                // New scene from peer — fetch it
                Logger::i("[sync] new scene %s from peer, fetching", e.id);
                _enqueueRequest(e.id);
            } else if (localHash == e.hash) {
                // Identical — clear any stale conflict entry for this peer
                // (no action needed)
            } else {
                // Hash mismatch — conflict
                Logger::d("[sync] conflict: scene %s local=%08x peer=%08x", e.id, localHash, e.hash);
                _registerConflict(e.id, localHash, senderMac, e.hash);
            }
        }
    }

    bool _isNeeded(const char* id) {
        // A scene is "needed" if it's in the fetch queue
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0)
                return true;
        return false;
    }

    // ── Fetch queue ───────────────────────────────────────────────────────────

    struct FetchEntry {
        char     id[33];
        bool     active;
        uint8_t  retries;
        uint32_t lastRequestMs;
    };

    FetchEntry _fetchQueue[SYNC_MAX_FETCH_QUEUE] = {};
    uint8_t    _fetchQueueCount = 0;

    void _enqueueRequest(const char* id) {
        // Avoid duplicates
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, 33) == 0) return;
        if (_fetchQueueCount >= SYNC_MAX_FETCH_QUEUE) {
            Logger::w("[sync] fetch queue full, dropping request for %s", id);
            return;
        }
        FetchEntry& e = _fetchQueue[_fetchQueueCount++];
        strlcpy(e.id, id, 33);
        e.active        = true;
        e.retries       = 0;
        e.lastRequestMs = 0;  // request immediately on next tick
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
            // Already sending; if same ID just let it continue; otherwise restart
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
            // Unexpected EOF
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

    // ── Chunk receive ─────────────────────────────────────────────────────────

    struct ChunkRecvState {
        char      id[33];
        uint8_t*  buffer;      // heap-allocated
        bool*     received;    // heap-allocated bitmask
        uint16_t  totalChunks;
        uint16_t  gotChunks;
        uint32_t  bufferSize;
        uint32_t  lastChunkMs;
        bool      active;
    };

    ChunkRecvState _recv = {};

    void _startReceive(const SceneChunkMsg* msg) {
        _resetReceive();
        uint32_t estimatedSize = (uint32_t)msg->totalChunks * CHUNK_DATA_SIZE;
        if (estimatedSize > SYNC_MAX_SCENE_BYTES) {
            Logger::w("[sync] scene %s too large (%u bytes), skipping", msg->id, estimatedSize);
            return;
        }
        _recv.buffer = (uint8_t*)malloc(estimatedSize);
        _recv.received = (bool*)calloc(msg->totalChunks, sizeof(bool));
        if (!_recv.buffer || !_recv.received) {
            Logger::e("[sync] OOM assembling scene %s", msg->id);
            free(_recv.buffer); free(_recv.received);
            _recv.buffer = nullptr; _recv.received = nullptr;
            return;
        }
        strlcpy(_recv.id, msg->id, 33);
        _recv.totalChunks = msg->totalChunks;
        _recv.gotChunks   = 0;
        _recv.bufferSize  = estimatedSize;
        _recv.lastChunkMs = millis();
        _recv.active      = true;
        Logger::i("[sync] receiving scene %s (%u chunks)", msg->id, msg->totalChunks);
        _handleChunk(msg);
    }

    void _handleChunk(const SceneChunkMsg* msg) {
        if (msg->chunkIndex >= _recv.totalChunks) return;
        if (_recv.received[msg->chunkIndex]) return;  // duplicate
        uint32_t offset = (uint32_t)msg->chunkIndex * CHUNK_DATA_SIZE;
        if (offset + msg->dataLen > _recv.bufferSize) return;
        memcpy(_recv.buffer + offset, msg->data, msg->dataLen);
        _recv.received[msg->chunkIndex] = true;
        _recv.gotChunks++;
        _recv.lastChunkMs = millis();
        if (_recv.gotChunks == _recv.totalChunks) {
            _finaliseReceive(msg);
        }
    }

    void _finaliseReceive(const SceneChunkMsg* lastMsg) {
        // Save id locally before any reset
        char id[33];
        strlcpy(id, _recv.id, 33);

        uint32_t totalSize = (uint32_t)(_recv.totalChunks - 1) * CHUNK_DATA_SIZE + lastMsg->dataLen;
        uint32_t incomingHash = SceneManager::crc32OfData(_recv.buffer, totalSize);
        uint32_t localHash    = SceneManager::crc32(id);
        bool forced = strncmp(id, _forcedAcceptId, 33) == 0 && _forcedAcceptId[0] != 0;

        if (localHash == incomingHash) {
            Logger::i("[sync] received scene %s already matches local, discarding", id);
            _resetReceive();
            _removeFetchEntry(id);
            return;
        }

        if (localHash != 0 && !forced) {
            // Content differs from local copy — conflict; don't overwrite.
            // Peer cache already records the incoming hash so conflicts API will surface it.
            Logger::i("[sync] received scene %s conflicts with local copy, not saving", id);
            _resetReceive();
            return;
        }

        // Save: either we don't have it locally, or it's a forced conflict resolution.
        bool ok = SceneManager::saveRaw(id, _recv.buffer, totalSize);
        Logger::i("[sync] scene %s saved (%u bytes, hash=%08x) ok=%d", id, totalSize, incomingHash, ok);

        if (ok && forced) {
            // We just accepted a remote winner — broadcast force-set + chunks so all other
            // devices also adopt this content.
            _forcedAcceptId[0] = 0;
            _clearConflict(id);
            if (_broadcastForceSet) _broadcastForceSet(id, incomingHash);
            _resetReceive();
            _removeFetchEntry(id);
            _startChunkSend(id);
            return;
        }

        _resetReceive();
        _removeFetchEntry(id);
    }

    void _resetReceive() {
        if (_recv.buffer)   { free(_recv.buffer);   _recv.buffer   = nullptr; }
        if (_recv.received) { free(_recv.received);  _recv.received = nullptr; }
        _recv.active = false;
    }

    // ── Manifest broadcast ────────────────────────────────────────────────────

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
        Logger::d("[sync] broadcast manifest: %u entries, %u pages", total, totalPages);
    }

    // ── State ─────────────────────────────────────────────────────────────────

    uint32_t _lastManifestBroadcast  = 0;
    bool     _broadcastManifestNow   = false;
    char     _forcedAcceptId[33]     = {};

    BroadcastFn                        _broadcastForceSet;
    RequestFn                          _broadcastRequest;
    ChunkFn                            _broadcastChunk;
    std::function<void(const SceneManifestMsg&)> _broadcastManifestMsg;
};
