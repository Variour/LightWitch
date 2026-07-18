#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../mesh/MeshTypes.h"
#include "PlaylistManager.h"

// Mesh sync for playlist metadata — mirrors SceneSyncManager (see
// src/scenes/SceneSyncManager.h) field-for-field, substituting Playlist for
// Scene. Deliberately not shared code: playlists and scenes are independent
// entities that happen to reuse the same proven sync protocol. Only playlist
// metadata (name/loop/file list) is ever synced this way — the audio files a
// playlist references are never distributed over mesh (see PlayAudioMsg).

static constexpr uint8_t PLAYLIST_SYNC_MAX_PEER_PLAYLISTS = 32;
static constexpr uint8_t PLAYLIST_SYNC_MAX_FETCH_QUEUE = 8;
static constexpr uint32_t PLAYLIST_SYNC_MAX_PLAYLIST_BYTES = 4096;
static constexpr uint32_t PLAYLIST_SYNC_REQUEST_RETRY_MS = 6000;
static constexpr uint8_t PLAYLIST_SYNC_MAX_RETRIES = 3;
static constexpr uint32_t PLAYLIST_SYNC_CHUNK_SEND_INTERVAL_MS = 20;
static constexpr uint32_t PLAYLIST_SYNC_FOLLOWUP_MANIFEST_MS = 5000;
static constexpr uint32_t PLAYLIST_SYNC_MANIFEST_JITTER_MS = 2000;
static constexpr uint32_t PLAYLIST_SYNC_REQUEST_MANIFEST_JITTER_MS = 1000;
static constexpr uint8_t PLAYLIST_SYNC_MAX_RECV_SLOTS = 2;

class PlaylistSyncManager {
   public:
    void setOnPlaylistSaved(std::function<void(const char*)> cb) { _onPlaylistSaved = cb; }
    void setOnPlaylistListChanged(std::function<void()> cb) { _onPlaylistListChanged = cb; }

    void onManifest(const uint8_t* senderMac, const PlaylistManifestMsg* msg) {
        if (!Config::get().playlistSyncEnabled) return;
        _updatePeerCache(senderMac, msg);
        _processManifest(senderMac, msg);
    }

    void onRequest(const uint8_t* /*senderMac*/, const char* id) {
        if (!Config::get().playlistSyncEnabled) return;
        if (!LittleFS.exists(PlaylistManager::path(id))) return;
        _startChunkSend(id);
    }

    void onChunk(const PlaylistChunkMsg* msg) {
        if (!Config::get().playlistSyncEnabled) return;
        ChunkRecvState* slot = _findRecv(msg->id);
        if (slot) {
            _handleChunk(slot, msg);
            return;
        }
        if (!_isInFetchQueue(msg->id)) return;
        slot = _allocRecv(msg->id, msg->totalChunks);
        if (slot) _handleChunk(slot, msg);
    }

    void onPlaylistEditPush(const uint8_t* mac, const char* id, uint32_t prevHash) {
        if (!Config::get().playlistSyncEnabled) return;

        if (_findRecv(id)) {
            Logger::w("[plsync] PlaylistEditPush for %s already in-flight, treating as conflict",
                      id);
            uint32_t localHash = PlaylistManager::crc32(id);
            _registerConflict(id, localHash, mac, 0);
            return;
        }

        uint32_t localHash = PlaylistManager::crc32(id);
        bool tombstoned = PlaylistManager::isTombstone(id);
        bool localAbsent = (localHash == 0) && !tombstoned;

        if (localAbsent || tombstoned) {
            Logger::i("[plsync] PlaylistEditPush %s: absent/tombstoned, auto-importing", id);
            _allocRecv(id, 0);
        } else if (localHash == prevHash) {
            Logger::i("[plsync] PlaylistEditPush %s: prevHash match, auto-applying", id);
            _allocRecv(id, 0);
        } else {
            Logger::i(
                "[plsync] PlaylistEditPush %s: conflict (local=%08x prev=%08x), waiting for "
                "manifest",
                id, localHash, prevHash);
        }
    }

    void onForceSet(const char* id, uint32_t hash) {
        if (!Config::get().playlistSyncEnabled) return;
        uint32_t localHash = PlaylistManager::crc32(id);
        if (localHash == hash) return;
        if (hash == 0) {
            PlaylistManager::remove(id);
            if (_onPlaylistListChanged) _onPlaylistListChanged();
            return;
        }
        setForcedAccept(id);
        _enqueueRequest(id);
        Logger::i("[plsync] force-set for %s, requesting chunks", id);
    }

    void onSetPlaylistSync(bool enabled) {
        if (Config::get().playlistSyncEnabled == enabled) return;
        bool prev = Config::get().playlistSyncEnabled;
        Config::get().playlistSyncEnabled = enabled;
        Config::save();
        Logger::i("[plsync] playlistSyncEnabled set to %d", (int)enabled);
        if (enabled && !prev) onSyncEnabled();
    }

    void onNewPeer(const uint8_t* /*mac*/) {
        if (!Config::get().playlistSyncEnabled) return;
        _scheduleJitteredManifest(PLAYLIST_SYNC_MANIFEST_JITTER_MS);
    }

    void onSyncEnabled() {
        _scheduleJitteredManifest(PLAYLIST_SYNC_MANIFEST_JITTER_MS);
        if (_broadcastRequestManifest) _broadcastRequestManifest();
        Logger::i("[plsync] sync enabled, broadcasting manifest request");
    }

    void onRequestManifest() {
        if (!Config::get().playlistSyncEnabled) return;
        _scheduleJitteredManifest(PLAYLIST_SYNC_REQUEST_MANIFEST_JITTER_MS);
    }

    bool deletePlaylist(const char* id) {
        bool ok = PlaylistManager::remove(id);
        if (ok) {
            Logger::i("[plsync] playlist %s deleted, broadcasting manifest", id);
            _broadcastManifestNow = true;
        }
        return ok;
    }

    void onPlaylistChanged(const char* id, uint32_t prevHash) {
        if (!Config::get().playlistSyncEnabled) return;
        Logger::i("[plsync] playlist %s changed (prevHash=%08x), broadcasting edit push", id,
                  prevHash);
        if (_broadcastEditPush) _broadcastEditPush(id, prevHash);
        _startChunkSend(id);
        _followupManifestMs = millis();
        _followupManifestPending = true;
    }

    void setForcedAccept(const char* id) { strlcpy(_forcedAcceptId, id, PLAYLIST_ID_LEN); }

    void resolveWithLocal(const char* id) {
        uint32_t hash = PlaylistManager::crc32(id);
        if (hash == 0) {
            Logger::w("[plsync] resolveWithLocal: playlist %s not found", id);
            return;
        }
        Logger::i("[plsync] resolving conflict for %s with local copy (hash=%08x)", id, hash);
        if (_broadcastForceSet) _broadcastForceSet(id, hash);
        _startChunkSend(id);
    }

    void tick() {
        if (!Config::get().playlistSyncEnabled) return;
        uint32_t now = millis();

        if (_jitteredManifestDue && now >= _jitteredManifestAt) {
            _broadcastManifest();
            _jitteredManifestDue = false;
        }

        if (_broadcastManifestNow) {
            _broadcastManifest();
            _broadcastManifestNow = false;
        }

        if (_followupManifestPending &&
            now - _followupManifestMs >= PLAYLIST_SYNC_FOLLOWUP_MANIFEST_MS) {
            _broadcastManifest();
            _followupManifestPending = false;
        }

        for (uint8_t i = 0; i < _fetchQueueCount; i++) {
            FetchEntry& e = _fetchQueue[i];
            if (!e.active) continue;
            if (e.retries >= PLAYLIST_SYNC_MAX_RETRIES) {
                Logger::w("[plsync] giving up on playlist %s after %u retries", e.id, e.retries);
                e.active = false;
                continue;
            }
            if (now - e.lastRequestMs >= PLAYLIST_SYNC_REQUEST_RETRY_MS) {
                _broadcastRequest(e.id);
                e.lastRequestMs = now;
                e.retries++;
            }
        }
        _compactFetchQueue();

        if (_send.active && now - _send.lastSendMs >= PLAYLIST_SYNC_CHUNK_SEND_INTERVAL_MS) {
            _sendNextChunk();
        }

        for (uint8_t i = 0; i < PLAYLIST_SYNC_MAX_RECV_SLOTS; i++) {
            ChunkRecvState& s = _recv[i];
            if (!s.active) continue;
            if (now - s.lastChunkMs > 10000) {
                Logger::w("[plsync] receive timeout for %s, falling back to PlaylistRequest",
                          s.id);
                _enqueueRequest(s.id, s.autoApply);
                _resetRecvSlot(s);
            }
        }
    }

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
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", c.peerHashes[j].mac[0],
                         c.peerHashes[j].mac[1], c.peerHashes[j].mac[2], c.peerHashes[j].mac[3],
                         c.peerHashes[j].mac[4], c.peerHashes[j].mac[5]);
                p["mac"] = mac;
                p["hash"] = c.peerHashes[j].hash;
            }
        }
    }

    void buildPeerPlaylistsJson(JsonDocument& doc) {
        JsonArray arr = doc["peerPlaylists"].to<JsonArray>();
        for (uint8_t i = 0; i < PeerRegistry::MAX_PEERS; i++) {
            PeerManifest& pm = _peerManifests[i];
            if (!pm.active) continue;
            JsonObject o = arr.add<JsonObject>();
            char mac[18];
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", pm.mac[0], pm.mac[1],
                     pm.mac[2], pm.mac[3], pm.mac[4], pm.mac[5]);
            o["mac"] = mac;
            JsonArray playlists = o["playlists"].to<JsonArray>();
            for (uint8_t j = 0; j < pm.count; j++) {
                JsonObject s = playlists.add<JsonObject>();
                s["id"] = pm.entries[j].id;
                s["hash"] = pm.entries[j].hash;
            }
        }
    }

    using BroadcastFn = std::function<void(const char* id, uint32_t hash)>;
    using RequestFn = std::function<void(const char* id)>;
    using ChunkFn = std::function<void(const PlaylistChunkMsg& msg)>;
    using EditPushFn = std::function<void(const char* id, uint32_t prevHash)>;
    using RequestManFn = std::function<void()>;

    void setBroadcastFns(BroadcastFn forceSet, RequestFn request, ChunkFn chunk,
                         std::function<void(const PlaylistManifestMsg&)> manifest) {
        _broadcastForceSet = forceSet;
        _broadcastRequest = request;
        _broadcastChunk = chunk;
        _broadcastManifestMsg = manifest;
    }

    void setEditPushFn(EditPushFn fn) { _broadcastEditPush = fn; }
    void setRequestManifestFn(RequestManFn fn) { _broadcastRequestManifest = fn; }

   private:
    struct PeerManifestEntry {
        char id[PLAYLIST_ID_LEN];
        uint32_t hash;
    };

    struct PeerManifest {
        uint8_t mac[6];
        bool active = false;
        PeerManifestEntry entries[PLAYLIST_SYNC_MAX_PEER_PLAYLISTS];
        uint8_t count = 0;
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
                pm.count = 0;
                return &pm;
            }
        }
        return nullptr;
    }

    void _updatePeerCache(const uint8_t* mac, const PlaylistManifestMsg* msg) {
        PeerManifest* pm = _findOrCreatePeerManifest(mac);
        if (!pm) return;
        if (msg->page == 0) pm->count = 0;
        uint8_t safeCount = msg->count < PLAYLIST_MANIFEST_ENTRIES_PER_MSG
                                ? msg->count
                                : PLAYLIST_MANIFEST_ENTRIES_PER_MSG;
        for (uint8_t i = 0; i < safeCount && pm->count < PLAYLIST_SYNC_MAX_PEER_PLAYLISTS; i++) {
            const PlaylistManifestEntry& e = msg->entries[i];
            bool found = false;
            for (uint8_t j = 0; j < pm->count; j++) {
                if (strncmp(pm->entries[j].id, e.id, PLAYLIST_ID_LEN) == 0) {
                    pm->entries[j].hash = e.hash;
                    found = true;
                    break;
                }
            }
            if (!found) {
                strlcpy(pm->entries[pm->count].id, e.id, PLAYLIST_ID_LEN);
                pm->entries[pm->count].hash = e.hash;
                pm->count++;
            }
        }
    }

    struct PeerHash {
        uint8_t mac[6];
        uint32_t hash;
        bool active;
    };

    struct Conflict {
        char id[PLAYLIST_ID_LEN];
        uint32_t localHash;
        PeerHash peerHashes[PeerRegistry::MAX_PEERS];
    };

    static constexpr uint8_t MAX_CONFLICTS = 16;
    Conflict _conflicts[MAX_CONFLICTS] = {};
    uint8_t _conflictCount = 0;

    void _registerConflict(const char* id, uint32_t localHash, const uint8_t* peerMac,
                           uint32_t peerHash) {
        Conflict* c = nullptr;
        for (uint8_t i = 0; i < _conflictCount; i++) {
            if (strncmp(_conflicts[i].id, id, PLAYLIST_ID_LEN) == 0) {
                c = &_conflicts[i];
                break;
            }
        }
        if (!c) {
            if (_conflictCount >= MAX_CONFLICTS) {
                Logger::w("[plsync] conflict table full, dropping conflict for %s", id);
                return;
            }
            c = &_conflicts[_conflictCount++];
            strlcpy(c->id, id, PLAYLIST_ID_LEN);
            memset(c->peerHashes, 0, sizeof(c->peerHashes));
            c->localHash = localHash;
        }
        for (auto& p : c->peerHashes) {
            if (!p.active || memcmp(p.mac, peerMac, 6) == 0) {
                memcpy(p.mac, peerMac, 6);
                p.hash = peerHash;
                p.active = true;
                return;
            }
        }
    }

    void _clearConflict(const char* id) {
        for (uint8_t i = 0; i < _conflictCount; i++) {
            if (strncmp(_conflicts[i].id, id, PLAYLIST_ID_LEN) == 0) {
                _conflicts[i] = _conflicts[--_conflictCount];
                return;
            }
        }
    }

    void _processManifest(const uint8_t* senderMac, const PlaylistManifestMsg* msg) {
        uint8_t safeCount = msg->count < PLAYLIST_MANIFEST_ENTRIES_PER_MSG
                                ? msg->count
                                : PLAYLIST_MANIFEST_ENTRIES_PER_MSG;
        for (uint8_t i = 0; i < safeCount; i++) {
            const PlaylistManifestEntry& e = msg->entries[i];

            if (_findRecv(e.id)) continue;

            uint32_t localHash = PlaylistManager::crc32(e.id);
            bool localTombstone = (localHash == 0) && PlaylistManager::isTombstone(e.id);
            bool localAbsent = (localHash == 0) && !localTombstone;

            if (e.hash == 0) {
                if (localAbsent || localTombstone) {
                    // Both absent/deleted — nothing to do
                } else {
                    Logger::d("[plsync] conflict: peer deleted %s, we have hash=%08x", e.id,
                              localHash);
                    _registerConflict(e.id, localHash, senderMac, 0);
                }
            } else if (localAbsent) {
                Logger::i("[plsync] new playlist %s from peer, fetching", e.id);
                _enqueueRequest(e.id, false);
            } else if (localHash == e.hash) {
                // Identical — no action
            } else {
                Logger::d("[plsync] conflict: playlist %s local=%08x peer=%08x", e.id, localHash,
                          e.hash);
                _registerConflict(e.id, localHash, senderMac, e.hash);
            }
        }
    }

    struct FetchEntry {
        char id[PLAYLIST_ID_LEN];
        bool active;
        bool autoAccept;
        uint8_t retries;
        uint32_t lastRequestMs;
    };

    FetchEntry _fetchQueue[PLAYLIST_SYNC_MAX_FETCH_QUEUE] = {};
    uint8_t _fetchQueueCount = 0;

    void _enqueueRequest(const char* id, bool autoAccept = false) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++) {
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, PLAYLIST_ID_LEN) == 0) {
                if (autoAccept) _fetchQueue[i].autoAccept = true;
                return;
            }
        }
        if (_fetchQueueCount >= PLAYLIST_SYNC_MAX_FETCH_QUEUE) {
            Logger::w("[plsync] fetch queue full, dropping request for %s", id);
            return;
        }
        FetchEntry& e = _fetchQueue[_fetchQueueCount++];
        strlcpy(e.id, id, PLAYLIST_ID_LEN);
        e.active = true;
        e.autoAccept = autoAccept;
        e.retries = 0;
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
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, PLAYLIST_ID_LEN) == 0)
                _fetchQueue[i].active = false;
    }

    bool _isInFetchQueue(const char* id) {
        for (uint8_t i = 0; i < _fetchQueueCount; i++)
            if (_fetchQueue[i].active && strncmp(_fetchQueue[i].id, id, PLAYLIST_ID_LEN) == 0)
                return true;
        return false;
    }

    struct ChunkSendState {
        char id[PLAYLIST_ID_LEN];
        File file;
        uint16_t totalChunks;
        uint16_t nextChunk;
        uint32_t lastSendMs;
        bool active;
    };

    ChunkSendState _send = {};

    void _startChunkSend(const char* id) {
        if (_send.active) {
            if (strncmp(_send.id, id, PLAYLIST_ID_LEN) == 0) return;
            _send.file.close();
        }
        String p = PlaylistManager::path(id);
        File f = LittleFS.open(p, "r");
        if (!f) {
            Logger::e("[plsync] chunk send: file not found %s", id);
            return;
        }
        uint32_t size = f.size();
        if (size == 0) {
            f.close();
            return;
        }
        strlcpy(_send.id, id, PLAYLIST_ID_LEN);
        _send.file = f;
        _send.totalChunks = (uint16_t)((size + PLAYLIST_CHUNK_DATA_SIZE - 1) / PLAYLIST_CHUNK_DATA_SIZE);
        _send.nextChunk = 0;
        _send.lastSendMs = 0;
        _send.active = true;
        Logger::i("[plsync] starting chunk send for %s (%u bytes, %u chunks)", id, size,
                  _send.totalChunks);
    }

    void _sendNextChunk() {
        if (!_send.active) return;
        PlaylistChunkMsg msg;
        msg.type = MsgType::PlaylistChunk;
        strlcpy(msg.id, _send.id, PLAYLIST_ID_LEN);
        msg.chunkIndex = _send.nextChunk;
        msg.totalChunks = _send.totalChunks;
        msg.dataLen = (uint16_t)_send.file.read(msg.data, PLAYLIST_CHUNK_DATA_SIZE);
        if (msg.dataLen == 0) {
            Logger::w("[plsync] chunk send EOF early for %s at chunk %u", _send.id,
                      _send.nextChunk);
            _send.file.close();
            _send.active = false;
            return;
        }
        if (_broadcastChunk) _broadcastChunk(msg);
        _send.lastSendMs = millis();
        _send.nextChunk++;
        if (_send.nextChunk >= _send.totalChunks) {
            Logger::i("[plsync] chunk send complete for %s (%u chunks)", _send.id,
                      _send.totalChunks);
            _send.file.close();
            _send.active = false;
        }
    }

    struct ChunkRecvState {
        char id[PLAYLIST_ID_LEN];
        uint8_t* buffer;
        bool* received;
        uint16_t totalChunks;
        uint16_t gotChunks;
        uint32_t bufferSize;
        uint32_t lastChunkMs;
        bool active;
        bool autoApply;
    };

    ChunkRecvState _recv[PLAYLIST_SYNC_MAX_RECV_SLOTS] = {};

    ChunkRecvState* _findRecv(const char* id) {
        for (auto& s : _recv)
            if (s.active && strncmp(s.id, id, PLAYLIST_ID_LEN) == 0) return &s;
        return nullptr;
    }

    ChunkRecvState* _allocRecv(const char* id, uint16_t totalChunks) {
        for (auto& s : _recv) {
            if (!s.active) {
                memset(&s, 0, sizeof(s));
                strlcpy(s.id, id, PLAYLIST_ID_LEN);
                s.active = true;
                s.autoApply = true;
                s.lastChunkMs = millis();
                if (totalChunks > 0) {
                    uint32_t est = (uint32_t)totalChunks * PLAYLIST_CHUNK_DATA_SIZE;
                    if (est > PLAYLIST_SYNC_MAX_PLAYLIST_BYTES) {
                        Logger::w("[plsync] playlist %s too large (%u bytes), skipping", id, est);
                        s.active = false;
                        return nullptr;
                    }
                    s.buffer = (uint8_t*)malloc(est);
                    s.received = (bool*)calloc(totalChunks, sizeof(bool));
                    if (!s.buffer || !s.received) {
                        Logger::e("[plsync] OOM allocating recv slot for %s", id);
                        free(s.buffer);
                        free(s.received);
                        s.buffer = nullptr;
                        s.received = nullptr;
                        s.active = false;
                        return nullptr;
                    }
                    s.totalChunks = totalChunks;
                    s.bufferSize = est;
                }
                Logger::i("[plsync] recv slot allocated for %s", id);
                return &s;
            }
        }
        Logger::w("[plsync] no recv slot available for %s", id);
        return nullptr;
    }

    void _handleChunk(ChunkRecvState* s, const PlaylistChunkMsg* msg) {
        if (!s->buffer) {
            uint32_t est = (uint32_t)msg->totalChunks * PLAYLIST_CHUNK_DATA_SIZE;
            if (est > PLAYLIST_SYNC_MAX_PLAYLIST_BYTES) {
                Logger::w("[plsync] playlist %s too large (%u bytes), skipping", s->id, est);
                _resetRecvSlot(*s);
                return;
            }
            s->buffer = (uint8_t*)malloc(est);
            s->received = (bool*)calloc(msg->totalChunks, sizeof(bool));
            if (!s->buffer || !s->received) {
                Logger::e("[plsync] OOM for %s", s->id);
                _resetRecvSlot(*s);
                return;
            }
            s->totalChunks = msg->totalChunks;
            s->bufferSize = est;
        }

        if (msg->chunkIndex >= s->totalChunks) return;
        if (s->received[msg->chunkIndex]) return;
        uint32_t offset = (uint32_t)msg->chunkIndex * PLAYLIST_CHUNK_DATA_SIZE;
        if (offset + msg->dataLen > s->bufferSize) return;
        memcpy(s->buffer + offset, msg->data, msg->dataLen);
        s->received[msg->chunkIndex] = true;
        s->gotChunks++;
        s->lastChunkMs = millis();
        if (s->gotChunks == s->totalChunks) {
            _finaliseReceive(s, msg);
        }
    }

    void _finaliseReceive(ChunkRecvState* s, const PlaylistChunkMsg* lastMsg) {
        char id[PLAYLIST_ID_LEN];
        strlcpy(id, s->id, PLAYLIST_ID_LEN);

        uint32_t totalSize = (uint32_t)(s->totalChunks - 1) * PLAYLIST_CHUNK_DATA_SIZE + lastMsg->dataLen;
        uint32_t incomingHash = PlaylistManager::crc32OfData(s->buffer, totalSize);
        uint32_t localHash = PlaylistManager::crc32(id);
        bool forced = strncmp(id, _forcedAcceptId, PLAYLIST_ID_LEN) == 0 && _forcedAcceptId[0] != 0;
        bool autoAccept = s->autoApply || forced;

        if (localHash == incomingHash) {
            Logger::i("[plsync] received playlist %s already matches local, discarding", id);
            _resetRecvSlot(*s);
            _removeFetchEntry(id);
            return;
        }

        if (localHash != 0 && !autoAccept) {
            Logger::i("[plsync] received playlist %s conflicts with local copy, not saving", id);
            _resetRecvSlot(*s);
            return;
        }

        if (!forced) {
            size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
            if (freeBytes < 20480) {
                Logger::w("[plsync] low storage (%u B free), skipping playlist %s", freeBytes, id);
                _resetRecvSlot(*s);
                _removeFetchEntry(id);
                return;
            }
        }

        bool ok = PlaylistManager::saveRaw(id, s->buffer, totalSize);
        Logger::i("[plsync] playlist %s saved (%u bytes, hash=%08x) ok=%d", id, totalSize,
                  incomingHash, ok);

        if (ok && _onPlaylistSaved) _onPlaylistSaved(id);
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

    void _resetRecvSlot(ChunkRecvState& s) {
        if (s.buffer) {
            free(s.buffer);
            s.buffer = nullptr;
        }
        if (s.received) {
            free(s.received);
            s.received = nullptr;
        }
        s.active = false;
    }

    void _scheduleJitteredManifest(uint32_t maxJitterMs) {
        uint32_t jitter = (uint32_t)(esp_random() % (maxJitterMs + 1));
        uint32_t fireAt = millis() + jitter;
        if (!_jitteredManifestDue || fireAt < _jitteredManifestAt) {
            _jitteredManifestAt = fireAt;
            _jitteredManifestDue = true;
        }
    }

    void _broadcastManifest() {
        PlaylistManager::ManifestEntry allEntries[64];
        uint8_t total = PlaylistManager::buildManifestEntries(allEntries, 64);
        uint8_t totalPages = (total + PLAYLIST_MANIFEST_ENTRIES_PER_MSG - 1) / PLAYLIST_MANIFEST_ENTRIES_PER_MSG;
        if (totalPages == 0) totalPages = 1;

        uint8_t idx = 0;
        for (uint8_t page = 0; page < totalPages; page++) {
            PlaylistManifestMsg msg;
            msg.type = MsgType::PlaylistManifest;
            msg.page = page;
            msg.totalPages = totalPages;
            msg.count = 0;
            while (msg.count < PLAYLIST_MANIFEST_ENTRIES_PER_MSG && idx < total) {
                strlcpy(msg.entries[msg.count].id, allEntries[idx].id, PLAYLIST_ID_LEN);
                msg.entries[msg.count].hash = allEntries[idx].hash;
                msg.count++;
                idx++;
            }
            if (_broadcastManifestMsg) _broadcastManifestMsg(msg);
        }
    }

    bool _broadcastManifestNow = false;
    bool _jitteredManifestDue = false;
    uint32_t _jitteredManifestAt = 0;
    bool _followupManifestPending = false;
    uint32_t _followupManifestMs = 0;
    char _forcedAcceptId[PLAYLIST_ID_LEN] = {};

    BroadcastFn _broadcastForceSet;
    RequestFn _broadcastRequest;
    ChunkFn _broadcastChunk;
    std::function<void(const PlaylistManifestMsg&)> _broadcastManifestMsg;
    EditPushFn _broadcastEditPush;
    RequestManFn _broadcastRequestManifest;
    std::function<void(const char*)> _onPlaylistSaved;
    std::function<void()> _onPlaylistListChanged;
};
