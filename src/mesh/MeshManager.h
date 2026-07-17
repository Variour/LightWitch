#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <functional>
#include <new>

#include "../battery/BatteryMonitor.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../update/Updater.h"
#include "../version.h"
#include "MeshCrypto.h"
#include "MeshTypes.h"
#include "PeerRegistry.h"

class MeshManager {
   public:
    PeerRegistry peers;

    struct MeshPolicyState {
        bool singleClientMode = false;
        uint32_t revision = 0;
        uint8_t originMac[6] = {};
    };

    using LightConfigCb = std::function<void(uint8_t groupId, const LightConfig&)>;
    using PresenceCb = std::function<void(const uint8_t* mac, const char* name, bool isNew)>;
    using SetGroupCb =
        std::function<void(const uint8_t* targetMac, uint8_t lightIndex, uint8_t groupId)>;
    using GroupSyncCb = std::function<void(const GroupConfig&)>;
    using PhaseSyncCb = std::function<void(uint8_t groupId, float phase)>;
    // Returns the current animation phase for the given light index.
    using GetPhaseCb = std::function<float(uint8_t lightIndex)>;
    using SceneManifestCb = std::function<void(const uint8_t* mac, const SceneManifestMsg*)>;
    using SceneRequestCb = std::function<void(const uint8_t* mac, const char* id)>;
    using SceneChunkCb = std::function<void(const SceneChunkMsg*)>;
    using SceneForceSetCb = std::function<void(const char* id, uint32_t hash)>;
    using SceneEditPushCb =
        std::function<void(const uint8_t* mac, const char* id, uint32_t prevHash)>;
    using RequestManifestCb = std::function<void()>;
    using SetSceneSyncCb = std::function<void(bool enabled)>;
    using ConfigChunkCb = std::function<void(const uint8_t* srcMac, const ConfigChunkMsg*)>;
    using TriggerUpdateCb = std::function<void()>;
    // Called when this device is told to check for a firmware update (no auto-install)
    using CheckUpdateCb = std::function<void()>;
    using PeerHeardCb = std::function<void(const uint8_t* mac)>;
    using TimeSyncCb = std::function<void(uint32_t epoch)>;
    // Called when a newer mesh-wide single-WiFi-client policy state is seen.
    // Receiver should persist and apply the full state locally.
    using MeshPolicyCb = std::function<void(const MeshPolicyState& state)>;
    // Polled once per heartbeat to fill PresenceMsg.wifiConnecting — whether
    // this device is right now mid-attempt to join a WiFi network.
    using WifiAttemptingCb = std::function<bool()>;
    // Polled once per heartbeat to fill PresenceMsg.wifiConnected — whether
    // this device should be advertised as the mesh's actual WiFi client
    // (see WifiElection::isAdvertisableConnected). Falls back to the raw
    // WiFi.status() check if unset.
    using WifiConnectedCb = std::function<bool()>;
    // Called when a peer (or this device, echoed back) broadcasts a manual
    // "retry WiFi now" request.
    using WifiRetryCb = std::function<void()>;
    // Called when a peer broadcasts a manual "search devices" request, so
    // this device re-searches too (see ChannelManager::beginSearch).
    using MeshSearchCb = std::function<void()>;
    // Polled once per heartbeat to fill PresenceMsg's battery fields.
    using BatteryStatusCb = std::function<BatteryMonitor::Status()>;

    void setOnPeerHeard(PeerHeardCb cb) { _onPeerHeard = cb; }
    void setOnLightConfig(LightConfigCb cb) { _onLightConfig = cb; }
    void setOnPresence(PresenceCb cb) { _onPresence = cb; }
    void setOnSetGroup(SetGroupCb cb) { _onSetGroup = cb; }
    void setOnGroupSync(GroupSyncCb cb) { _onGroupSync = cb; }
    void setOnPhaseSync(PhaseSyncCb cb) { _onPhaseSync = cb; }
    void setGetPhase(GetPhaseCb cb) { _getPhase = cb; }
    void setOnSceneManifest(SceneManifestCb cb) { _onSceneManifest = cb; }
    void setOnSceneRequest(SceneRequestCb cb) { _onSceneRequest = cb; }
    void setOnSceneChunk(SceneChunkCb cb) { _onSceneChunk = cb; }
    void setOnSceneForceSet(SceneForceSetCb cb) { _onSceneForceSet = cb; }
    void setOnSceneEditPush(SceneEditPushCb cb) { _onSceneEditPush = cb; }
    void setOnRequestManifest(RequestManifestCb cb) { _onRequestManifest = cb; }
    void setOnSetSceneSync(SetSceneSyncCb cb) { _onSetSceneSync = cb; }
    void setOnConfigChunk(ConfigChunkCb cb) { _onConfigChunk = cb; }
    void setOnTriggerUpdate(TriggerUpdateCb cb) { _onTriggerUpdate = cb; }
    void setOnCheckUpdate(CheckUpdateCb cb) { _onCheckUpdate = cb; }
    void setOnTimeSync(TimeSyncCb cb) { _onTimeSync = cb; }
    void setOnMeshPolicy(MeshPolicyCb cb) { _onMeshPolicy = cb; }
    void setWifiAttemptingProvider(WifiAttemptingCb cb) { _wifiAttemptingProvider = cb; }
    void setWifiConnectedProvider(WifiConnectedCb cb) { _wifiConnectedProvider = cb; }
    void setOnWifiRetry(WifiRetryCb cb) { _onWifiRetry = cb; }
    void setOnMeshSearch(MeshSearchCb cb) { _onMeshSearch = cb; }
    void setBatteryStatusProvider(BatteryStatusCb cb) { _batteryStatusProvider = cb; }

    void begin() {
        _instance = this;
        if (esp_now_init() != ESP_OK) {
            Logger::e("[mesh] esp_now_init failed");
            return;
        }
        _addBroadcastPeer();
        esp_now_register_recv_cb(_onRecv);
        esp_now_register_send_cb(_onSent);
        // Enable sniffer if any existing light is in proximity mode
        _setSnifferEnabled(_anyProximity());
        Logger::i("[mesh] ready, MAC: %s", WiFi.macAddress().c_str());
        _ready = true;
    }

    void tick() {
        if (!_ready) return;
        peers.tick();
        uint32_t now = millis();

        if (_outPush.active && now - _outPush.startedAt >= HANDSHAKE_TIMEOUT_MS) {
            Logger::w("[mesh] config push handshake timed out");
            mbedtls_ecdh_free(&_outPush.ctx);
            _outPush.active = false;
            _advancePushQueue();
        }

        if (now - _lastHeartbeat >= 5000) {
            _lastHeartbeat = now;
            _sendPresence();
        }
        if (now - _lastMeshPolicySync >= 15000) {
            _lastMeshPolicySync = now;
            broadcastMeshPolicy(_currentMeshPolicyState());
        }
        // Periodically re-advertise every existing group so an already-known
        // peer that missed a GroupSync (dropped packet) self-heals without
        // waiting for an unrelated group mutation.
        if (now - _lastGroupSync >= 15000) {
            _lastGroupSync = now;
            broadcastAllGroups();
        }

        // Proximity: enable sniffer + ping if any light is in proximity mode
        bool inProximity = _anyProximity();
        _setSnifferEnabled(inProximity);
        if (inProximity && now - _lastProximityPing >= 500) {
            _lastProximityPing = now;
            Config::forEachLight([&](uint8_t, LightHardwareConfig& l) {
                GroupConfig* g = Config::group(l.groupId);
                if (!g || g->light.mode != GroupMode::Proximity) return;
                ProximityPingMsg msg;
                msg.groupId = l.groupId;
                _send(&msg, sizeof(msg));
            });
        }

        // Phase sync: broadcast for each local light group where this device is master
        if (now - _lastPhaseBroadcast >= 2000 && _getPhase) {
            _lastPhaseBroadcast = now;
            Config::forEachLight([&](uint8_t i, LightHardwareConfig& l) {
                if (!_isSyncMaster(l.groupId)) return;
                GroupConfig* g = Config::group(l.groupId);
                if (!g || !g->syncEnabled) return;
                broadcastPhaseSync(l.groupId, _getPhase(i));
            });
        }
    }

    void broadcastLightConfig(uint8_t groupId, const LightConfig& cfg) {
        if (!_ready) return;
        LightConfigMsg msg;
        msg.type = MsgType::LightConfig;
        msg.groupId = groupId;
        msg.config = cfg;
        _lastSentGroup = groupId;
        _lastSentSeq = cfg.seq;
        _send(&msg, sizeof(msg));
    }

    void broadcastSetGroup(const uint8_t* targetMac, uint8_t lightIndex, uint8_t groupId) {
        if (!_ready) return;
        SetGroupMsg msg;
        msg.type = MsgType::SetGroup;
        msg.lightIndex = lightIndex;
        msg.groupId = groupId;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    void broadcastGroupSync(const GroupConfig& g) {
        if (!_ready) return;
        GroupSyncMsg msg;
        msg.type = MsgType::GroupSync;
        msg.group = g;
        _send(&msg, sizeof(msg));
    }

    void broadcastPhaseSync(uint8_t groupId, float phase) {
        if (!_ready) return;
        PhaseSyncMsg msg;
        msg.type = MsgType::PhaseSync;
        msg.groupId = groupId;
        msg.phase = phase;
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneManifest(const SceneManifestMsg& msg) {
        if (!_ready) return;
        size_t msgSize =
            offsetof(SceneManifestMsg, entries) + msg.count * sizeof(SceneManifestEntry);
        _send(&msg, msgSize);
    }

    void broadcastSceneRequest(const char* id) {
        if (!_ready) return;
        SceneRequestMsg msg;
        msg.type = MsgType::SceneRequest;
        strlcpy(msg.id, id, SCENE_ID_LEN);
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneChunk(const SceneChunkMsg& msg) {
        if (!_ready) return;
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneEditPush(const char* id, uint32_t prevHash) {
        if (!_ready) return;
        SceneEditPushMsg msg;
        strlcpy(msg.id, id, SCENE_ID_LEN);
        msg.prevHash = prevHash;
        _send(&msg, sizeof(msg));
    }

    void broadcastRequestManifest() {
        if (!_ready) return;
        RequestManifestMsg msg;
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneForceSet(const char* id, uint32_t hash) {
        if (!_ready) return;
        SceneForceSetMsg msg;
        msg.type = MsgType::SceneForceSet;
        strlcpy(msg.id, id, SCENE_ID_LEN);
        msg.hash = hash;
        _send(&msg, sizeof(msg));
    }

    // Encrypts `json` under a fresh ECDH-derived key and pushes it to a single
    // peer (issue #252). Fire-and-forget, like the rest of the mesh API: the
    // HTTP request that triggered this has already returned by the time the
    // handshake and send happen.
    void pushConfigSecure(const uint8_t* targetMac, const char* json) {
        if (!_ready) return;
        if (_outPush.active) {
            Logger::w("[mesh] config push already in progress, dropping request");
            return;
        }
        memcpy(_outPush.targetMac, targetMac, 6);
        _outPush.json = json;

        KeyExchangeInitMsg msg;
        memcpy(msg.targetMac, targetMac, 6);
        if (!MeshCrypto::beginExchange(_outPush.ctx, msg.pubKey)) {
            Logger::e("[mesh] ecdh keygen failed");
            _advancePushQueue();
            return;
        }
        _outPush.sessionId = esp_random();
        msg.sessionId = _outPush.sessionId;
        _outPush.active = true;
        _outPush.startedAt = millis();
        _send(&msg, sizeof(msg));
    }

    // Fans out an encrypted push to every currently-online peer, one at a time
    // (issue #252): a single pairwise ECDH key can't be shared across multiple
    // recipients, so "push to all" becomes a separate handshake + separately
    // keyed payload per peer instead of one plaintext broadcast.
    void pushConfigSecureToAll(const char* json) {
        if (!_ready) return;
        _pushQueueCount = 0;
        for (auto& p : peers) {
            if (p.online() && _pushQueueCount < PeerRegistry::MAX_PEERS)
                memcpy(_pushQueueMacs[_pushQueueCount++], p.mac, 6);
        }
        _pushQueueJson = json;
        _pushQueueIdx = 0;
        _advancePushQueue();
    }

    // Decrypts a fully-reassembled config push received from `srcMac`, using the
    // session key established during that push's ECDH handshake (issue #252).
    // Returns false if there's no valid/unexpired session (no handshake happened,
    // it expired, or the data is corrupt/tampered) — caller should drop the message.
    // `outPlain` must be at least `len` bytes.
    bool decryptConfigFromPeer(const uint8_t* srcMac, const uint8_t* data, size_t len,
                               uint8_t* outPlain, size_t& outLen) {
        for (auto& s : _inSessions) {
            if (!s.valid || memcmp(s.mac, srcMac, 6) != 0) continue;
            bool expired = millis() - s.establishedAt >= SESSION_KEY_TTL_MS;
            bool ok = !expired && MeshCrypto::decrypt(s.key, data, len, outPlain, outLen);
            s.valid = false;  // one-shot: never reuse a session key across pushes
            return ok;
        }
        return false;
    }

    void broadcastSetSceneSync(const uint8_t* targetMac, bool enabled) {
        if (!_ready) return;
        SetSceneSyncMsg msg;
        msg.type = MsgType::SetSceneSync;
        msg.enabled = enabled ? 1 : 0;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    void broadcastTriggerUpdate(const uint8_t* targetMac) {
        if (!_ready) return;
        TriggerUpdateMsg msg;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    void broadcastCheckUpdate(const uint8_t* targetMac) {
        if (!_ready) return;
        CheckUpdateMsg msg;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    void broadcastTimeSync(uint32_t epoch) {
        if (!_ready) return;
        TimeSyncMsg msg;
        msg.epoch = epoch;
        _send(&msg, sizeof(msg));
    }

    // Broadcasts the current mesh-wide single-WiFi-client policy state so peers
    // can converge even after missed packets, rejoins, or near-simultaneous edits.
    void broadcastMeshPolicy(const MeshPolicyState& state) {
        if (!_ready) return;
        MeshPolicyMsg msg;
        msg.wifiSingleClientMode = state.singleClientMode ? 1 : 0;
        msg.revision = state.revision;
        memcpy(msg.originMac, state.originMac, sizeof(msg.originMac));
        _send(&msg, sizeof(msg));
    }

    // Broadcasts a manual "retry WiFi now" request (see WifiElection::retryNow).
    void broadcastWifiRetry() {
        if (!_ready) return;
        WifiRetryMsg msg;
        _send(&msg, sizeof(msg));
    }

    // Broadcasts a manual "search devices" request (see ChannelManager::beginSearch).
    void broadcastMeshSearch() {
        if (!_ready) return;
        MeshSearchMsg msg;
        _send(&msg, sizeof(msg));
    }

    void broadcastAllGroups() {
        for (uint8_t i = 0; i < MAX_GROUPS; i++)
            if (Config::get().groups[i].exists) broadcastGroupSync(Config::get().groups[i]);
    }

    bool isReady() const { return _ready; }

   private:
    bool _ready = false;
    bool _snifferRunning = false;
    uint32_t _lastHeartbeat = 0;
    uint32_t _lastMeshPolicySync = 0;
    uint32_t _lastGroupSync = 0;
    uint32_t _lastPhaseBroadcast = 0;
    uint32_t _lastProximityPing = 0;
    uint8_t _lastSentGroup = 0xFF;
    uint32_t _lastSentSeq = UINT32_MAX;

    PeerHeardCb _onPeerHeard;
    LightConfigCb _onLightConfig;
    PresenceCb _onPresence;
    SetGroupCb _onSetGroup;
    GroupSyncCb _onGroupSync;
    PhaseSyncCb _onPhaseSync;
    GetPhaseCb _getPhase;
    SceneManifestCb _onSceneManifest;
    SceneRequestCb _onSceneRequest;
    SceneChunkCb _onSceneChunk;
    SceneForceSetCb _onSceneForceSet;
    SceneEditPushCb _onSceneEditPush;
    RequestManifestCb _onRequestManifest;
    SetSceneSyncCb _onSetSceneSync;
    ConfigChunkCb _onConfigChunk;
    TriggerUpdateCb _onTriggerUpdate;
    CheckUpdateCb _onCheckUpdate;
    TimeSyncCb _onTimeSync;
    MeshPolicyCb _onMeshPolicy;
    WifiAttemptingCb _wifiAttemptingProvider;
    WifiConnectedCb _wifiConnectedProvider;
    BatteryStatusCb _batteryStatusProvider;
    WifiRetryCb _onWifiRetry;
    MeshSearchCb _onMeshSearch;

    // ── Config push encryption (issue #252) ───────────────────────────────────
    static constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 3000;
    static constexpr uint32_t SESSION_KEY_TTL_MS = 10000;
    static constexpr uint8_t MAX_INBOUND_SESSIONS = 4;

    // State for the config push we initiated and are waiting on a KeyExchangeResp
    // for. Only one at a time; pushConfigSecureToAll() fans out sequentially.
    struct OutboundPush {
        bool active = false;
        uint8_t targetMac[6] = {};
        uint32_t sessionId = 0;
        mbedtls_ecdh_context ctx;
        uint32_t startedAt = 0;
        String json;
    };
    // A session key derived by responding to a peer's KeyExchangeInit, held until
    // the matching ConfigChunk train arrives (or it expires unused).
    struct InboundSession {
        bool valid = false;
        uint8_t mac[6] = {};
        uint8_t key[MeshCrypto::AES_KEY_LEN] = {};
        uint32_t establishedAt = 0;
    };

    OutboundPush _outPush;
    InboundSession _inSessions[MAX_INBOUND_SESSIONS];
    uint8_t _pushQueueMacs[PeerRegistry::MAX_PEERS][6];
    uint8_t _pushQueueCount = 0;
    uint8_t _pushQueueIdx = 0;
    String _pushQueueJson;

    static MeshManager* _instance;

    void _send(const void* data, size_t len) {
        static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        esp_now_send(bcast, (uint8_t*)data, len);
    }

    void _addBroadcastPeer() {
        esp_now_peer_info_t peer{};
        memset(peer.peer_addr, 0xFF, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    void _sendConfigChunks(const uint8_t* targetMac, const uint8_t* data, size_t len) {
        uint16_t totalChunks =
            (uint16_t)((len + CONFIG_CHUNK_DATA_SIZE - 1) / CONFIG_CHUNK_DATA_SIZE);
        for (uint16_t i = 0; i < totalChunks; i++) {
            ConfigChunkMsg msg;
            memcpy(msg.targetMac, targetMac, 6);
            msg.chunkIndex = i;
            msg.totalChunks = totalChunks;
            size_t offset = (size_t)i * CONFIG_CHUNK_DATA_SIZE;
            size_t chunkLen = min((size_t)CONFIG_CHUNK_DATA_SIZE, len - offset);
            msg.dataLen = (uint16_t)chunkLen;
            memcpy(msg.data, data + offset, chunkLen);
            _send(&msg, sizeof(msg));
            if (i < totalChunks - 1) delay(20);
        }
        Logger::i("[mesh] config push: %u bytes in %u chunks", (unsigned)len, totalChunks);
    }

    // Advances pushConfigSecureToAll()'s fan-out queue by starting the handshake
    // with the next peer, if any remain.
    void _advancePushQueue() {
        if (_pushQueueIdx >= _pushQueueCount) {
            _pushQueueJson = "";
            return;
        }
        pushConfigSecure(_pushQueueMacs[_pushQueueIdx++], _pushQueueJson.c_str());
    }

    // Finds the inbound session slot for `mac`, reusing an existing entry for
    // that peer, an empty slot, or the oldest expired one, in that order.
    InboundSession* _findInboundSlot(const uint8_t* mac) {
        for (auto& s : _inSessions)
            if (s.valid && memcmp(s.mac, mac, 6) == 0) return &s;
        for (auto& s : _inSessions)
            if (!s.valid) return &s;
        InboundSession* oldest = &_inSessions[0];
        for (auto& s : _inSessions)
            if (s.establishedAt < oldest->establishedAt) oldest = &s;
        return oldest;
    }

    // A peer wants to push us config: derive the session key immediately (we
    // never need to wait, since we already have their public key) and reply
    // with ours so they can derive the same key on their end.
    void _onKeyExchangeInit(const uint8_t* srcMac, const KeyExchangeInitMsg* m) {
        mbedtls_ecdh_context ctx;
        uint8_t myPub[ECDH_PUBKEY_LEN];
        if (!MeshCrypto::beginExchange(ctx, myPub)) {
            Logger::e("[mesh] ecdh keygen failed (responder)");
            return;
        }
        uint8_t key[MeshCrypto::AES_KEY_LEN];
        if (!MeshCrypto::finishExchange(ctx, m->pubKey, key)) {
            Logger::e("[mesh] ecdh exchange failed (responder)");
            return;
        }
        InboundSession* slot = _findInboundSlot(srcMac);
        memcpy(slot->mac, srcMac, 6);
        memcpy(slot->key, key, sizeof(key));
        slot->establishedAt = millis();
        slot->valid = true;
        memset(key, 0, sizeof(key));

        KeyExchangeRespMsg resp;
        memcpy(resp.targetMac, srcMac, 6);
        resp.sessionId = m->sessionId;
        memcpy(resp.pubKey, myPub, ECDH_PUBKEY_LEN);
        _send(&resp, sizeof(resp));
    }

    // The peer we're pushing config to replied with their public key: derive
    // the shared key, encrypt the pending JSON payload, and send it as chunks.
    void _onKeyExchangeResp(const KeyExchangeRespMsg* m) {
        if (!_outPush.active || m->sessionId != _outPush.sessionId) return;
        uint8_t key[MeshCrypto::AES_KEY_LEN];
        bool ok = MeshCrypto::finishExchange(_outPush.ctx, m->pubKey, key);
        _outPush.active = false;  // ctx already freed by finishExchange
        if (!ok) {
            Logger::e("[mesh] ecdh exchange failed (initiator)");
            _advancePushQueue();
            return;
        }
        size_t plainLen = _outPush.json.length();
        size_t encCap = plainLen + MeshCrypto::NONCE_LEN + MeshCrypto::TAG_LEN;
        uint8_t* enc = new (std::nothrow) uint8_t[encCap];
        size_t encLen = 0;
        if (!enc) {
            Logger::e("[mesh] config push: out of memory encrypting %u bytes", (unsigned)plainLen);
        } else if (MeshCrypto::encrypt(key, (const uint8_t*)_outPush.json.c_str(), plainLen, enc,
                                       encLen)) {
            _sendConfigChunks(_outPush.targetMac, enc, encLen);
        } else {
            Logger::e("[mesh] config encrypt failed");
        }
        delete[] enc;
        memset(key, 0, sizeof(key));
        _advancePushQueue();
    }

    bool _anyProximity() {
        bool found = false;
        Config::forEachLightUntil([&](uint8_t, LightHardwareConfig& l) -> bool {
            GroupConfig* g = Config::group(l.groupId);
            if (g && g->light.mode == GroupMode::Proximity) {
                found = true;
                return false;
            }
            return true;
        });
        return found;
    }

    MeshPolicyState _currentMeshPolicyState() const {
        MeshPolicyState state;
        state.singleClientMode = Config::get().wifiSingleClientMode;
        state.revision = Config::get().wifiPolicyRevision;
        memcpy(state.originMac, Config::get().wifiPolicyOriginMac, sizeof(state.originMac));
        return state;
    }

    static MeshPolicyState _stateFromMsg(const MeshPolicyMsg& msg) {
        MeshPolicyState state;
        state.singleClientMode = msg.wifiSingleClientMode != 0;
        state.revision = msg.revision;
        memcpy(state.originMac, msg.originMac, sizeof(state.originMac));
        return state;
    }

    static int _compareMeshPolicyState(const MeshPolicyState& a, const MeshPolicyState& b) {
        if (a.revision != b.revision) return a.revision > b.revision ? 1 : -1;
        int macCmp = memcmp(a.originMac, b.originMac, sizeof(a.originMac));
        if (macCmp != 0) return macCmp > 0 ? 1 : -1;
        if (a.singleClientMode == b.singleClientMode) return 0;
        return a.singleClientMode ? 1 : -1;
    }

    void _reconcileMeshPolicy(const MeshPolicyState& remote) {
        MeshPolicyState local = _currentMeshPolicyState();
        int cmp = _compareMeshPolicyState(remote, local);
        if (cmp > 0) {
            if (_onMeshPolicy) _onMeshPolicy(remote);
        } else if (cmp < 0) {
            broadcastMeshPolicy(local);
        }
    }

    // Merges an incoming GroupSync via Config::applyGroupSync, then — if our
    // revision turned out to be ahead of the sender's — re-broadcasts our
    // (winning) local state so the sender/mesh self-heals immediately instead
    // of waiting for the next periodic re-advertisement.
    //
    // Skips the merge (and its Config::save()) entirely when remote has
    // nothing newer than what we already have — otherwise the 15s periodic
    // re-advertisement from every peer would trigger a flash write here on
    // every tick even when nothing actually changed.
    void _reconcileGroupSync(const GroupConfig& remote) {
        if (remote.id >= MAX_GROUPS || !_onGroupSync) return;
        const GroupConfig& local = Config::get().groups[remote.id];
        bool remoteAhead = !local.exists || Config::compareGroupRevision(remote, local) > 0;
        if (remoteAhead) _onGroupSync(remote);
        if (Config::compareGroupRevision(local, remote) > 0) broadcastGroupSync(local);
    }

    template <typename T>
    static bool _hasExactLen(int len) {
        return len == (int)sizeof(T);
    }

    static bool _hasVariableArrayLen(int len, size_t headerLen, size_t itemCount, size_t itemSize,
                                     size_t maxCount) {
        return itemCount <= maxCount && (size_t)len >= headerLen + itemCount * itemSize;
    }

    static bool _hasVariablePayloadLen(int len, size_t headerLen, size_t payloadLen,
                                       size_t payloadCap) {
        return payloadLen <= payloadCap && (size_t)len >= headerLen + payloadLen;
    }

    void _sendPresence() {
        PresenceMsg msg;
        msg.type = MsgType::Presence;
        msg.sceneSyncEnabled = Config::get().sceneSyncEnabled ? 1 : 0;
        strlcpy(msg.name, Config::get().deviceName, sizeof(msg.name));
        msg.wifiConnected =
            (_wifiConnectedProvider ? _wifiConnectedProvider() : (WiFi.status() == WL_CONNECTED))
                ? 1
                : 0;
        msg.hasWifiNetworks = (Config::wifiCount() > 0) ? 1 : 0;
        msg.wifiConnecting = (_wifiAttemptingProvider && _wifiAttemptingProvider()) ? 1 : 0;
        BatteryMonitor::Status bs =
            _batteryStatusProvider ? _batteryStatusProvider() : BatteryMonitor::Status{};
        msg.batteryPresent = bs.present ? 1 : 0;
        msg.batteryPercent = bs.percent;
        msg.batteryCharging = (bs.state == BatteryMonitor::State::Charging) ? 1 : 0;
        strlcpy(msg.fwVersion, FW_VERSION, sizeof(msg.fwVersion));
        const auto& us = Updater::status();
        msg.fwState = (uint8_t)(us.state == Updater::State::Checking      ? FwState::Checking
                                : us.state == Updater::State::Downloading ? FwState::Downloading
                                : us.state == Updater::State::Error       ? FwState::Error
                                : us.state == Updater::State::Done        ? FwState::Done
                                                                          : FwState::Idle);

        msg.lightCount = 0;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            msg.lightGroupIds[i] = 0xFF;
            msg.lightNames[i][0] = '\0';
        }
        Config::forEachLight([&](uint8_t, LightHardwareConfig& l) {
            if (msg.lightCount < MAX_LIGHTS) {
                uint8_t slot = msg.lightCount++;
                msg.lightGroupIds[slot] = l.groupId;
                strlcpy(msg.lightNames[slot], l.name, 20);
            }
        });
        _send(&msg, sizeof(msg));
    }

    // Returns true if this device has the lowest MAC among online peers that also
    // have a light in groupId. The lowest-MAC device acts as the phase-sync master.
    bool _isSyncMaster(uint8_t groupId) {
        uint8_t ownMac[6];
        WiFi.macAddress(ownMac);
        for (const auto& p : peers) {
            if (!p.online()) continue;
            bool inGroup = false;
            for (uint8_t i = 0; i < p.lightCount && i < MAX_LIGHTS; i++)
                if (p.lightGroupIds[i] == groupId) {
                    inGroup = true;
                    break;
                }
            if (!inGroup) continue;
            if (memcmp(p.mac, ownMac, 6) < 0) return false;
        }
        return true;
    }

    void _setSnifferEnabled(bool enable) {
        if (enable == _snifferRunning) return;
        if (enable) {
            wifi_promiscuous_filter_t f = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
            esp_wifi_set_promiscuous_filter(&f);
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(_promiscuousRecv);
            Logger::i("[mesh] proximity sniffer started");
        } else {
            esp_wifi_set_promiscuous(false);
            Logger::i("[mesh] proximity sniffer stopped");
        }
        _snifferRunning = enable;
    }

    static void _promiscuousRecv(void* buf, wifi_promiscuous_pkt_type_t type) {
        if (type != WIFI_PKT_MGMT || !_instance) return;
        const auto* pkt = (const wifi_promiscuous_pkt_t*)buf;
        if (pkt->rx_ctrl.sig_len < 16) return;
        if ((pkt->payload[0] & 0xFC) != 0xD0) return;
        _instance->peers.updateRssi(pkt->payload + 10, (int8_t)pkt->rx_ctrl.rssi);
    }

    static void _onRecv(const uint8_t* mac, const uint8_t* data, int len) {
        if (len < 1 || !_instance) return;
        MsgType type = (MsgType)data[0];

        switch (type) {
            case MsgType::Presence: {
                if (!_hasExactLen<PresenceMsg>(len)) return;
                auto* m = (PresenceMsg*)data;
                if (m->version != PRESENCE_MSG_VERSION) return;
                bool isNew = _instance->peers.update(
                    mac, m->name, m->lightCount, m->lightGroupIds, m->lightNames,
                    m->sceneSyncEnabled != 0, m->wifiConnected != 0, m->fwVersion,
                    (FwState)m->fwState, m->hasWifiNetworks != 0, m->wifiConnecting != 0,
                    m->batteryPresent != 0, m->batteryPercent, m->batteryCharging != 0);
                if (_instance->_onPeerHeard) _instance->_onPeerHeard(mac);
                if (_instance->_onPresence) _instance->_onPresence(mac, m->name, isNew);
                if (isNew) {
                    _instance->broadcastAllGroups();
                    _instance->broadcastMeshPolicy(_instance->_currentMeshPolicyState());
                }
                break;
            }
            case MsgType::LightConfig: {
                if (len < (int)sizeof(LightConfigMsg)) return;
                auto* m = (LightConfigMsg*)data;
                if (m->groupId == _instance->_lastSentGroup &&
                    m->config.seq == _instance->_lastSentSeq)
                    return;
                Logger::d("[mesh] light config rx group %u pattern %u seq %u", m->groupId,
                          (uint8_t)m->config.pattern, m->config.seq);
                if (_instance->_onLightConfig) _instance->_onLightConfig(m->groupId, m->config);
                break;
            }
            case MsgType::SetGroup: {
                if (len < (int)sizeof(SetGroupMsg)) return;
                auto* m = (SetGroupMsg*)data;
                Logger::i(
                    "[mesh] set-group rx: target %02x:%02x:%02x:%02x:%02x:%02x light %u → group %u",
                    m->targetMac[0], m->targetMac[1], m->targetMac[2], m->targetMac[3],
                    m->targetMac[4], m->targetMac[5], m->lightIndex, m->groupId);
                if (_instance->_onSetGroup)
                    _instance->_onSetGroup(m->targetMac, m->lightIndex, m->groupId);
                _instance->peers.updateLightGroup(mac, m->lightIndex, m->groupId);
                break;
            }
            case MsgType::GroupSync: {
                if (len < (int)sizeof(GroupSyncMsg)) return;
                auto* m = (GroupSyncMsg*)data;
                Logger::v("[mesh] group-sync rx: group %u \"%s\" exists=%d rev=%lu", m->group.id,
                          m->group.name, m->group.exists, (unsigned long)m->group.revision);
                _instance->_reconcileGroupSync(m->group);
                break;
            }
            case MsgType::PhaseSync: {
                if (len < (int)sizeof(PhaseSyncMsg)) return;
                auto* m = (PhaseSyncMsg*)data;
                if (_instance->_onPhaseSync) _instance->_onPhaseSync(m->groupId, m->phase);
                break;
            }
            case MsgType::ProximityPing:
                if (!_hasExactLen<ProximityPingMsg>(len)) return;
                break;
            case MsgType::SceneManifest: {
                if (len < (int)offsetof(SceneManifestMsg, entries)) return;
                auto* m = (SceneManifestMsg*)data;
                if (!_hasVariableArrayLen(len, offsetof(SceneManifestMsg, entries), m->count,
                                          sizeof(SceneManifestEntry), MANIFEST_ENTRIES_PER_MSG))
                    return;
                if (_instance->_onSceneManifest) _instance->_onSceneManifest(mac, m);
                break;
            }
            case MsgType::SceneRequest: {
                if (len < (int)sizeof(SceneRequestMsg)) return;
                auto* m = (SceneRequestMsg*)data;
                if (_instance->_onSceneRequest) _instance->_onSceneRequest(mac, m->id);
                break;
            }
            case MsgType::SceneChunk: {
                if (len < (int)offsetof(SceneChunkMsg, data)) return;
                auto* m = (SceneChunkMsg*)data;
                if (!_hasVariablePayloadLen(len, offsetof(SceneChunkMsg, data), m->dataLen,
                                            CHUNK_DATA_SIZE))
                    return;
                if (_instance->_onSceneChunk) _instance->_onSceneChunk(m);
                break;
            }
            case MsgType::SceneForceSet: {
                if (len < (int)sizeof(SceneForceSetMsg)) return;
                auto* m = (SceneForceSetMsg*)data;
                if (_instance->_onSceneForceSet) _instance->_onSceneForceSet(m->id, m->hash);
                break;
            }
            case MsgType::SceneEditPush: {
                if (len < (int)sizeof(SceneEditPushMsg)) return;
                auto* m = (SceneEditPushMsg*)data;
                if (_instance->_onSceneEditPush)
                    _instance->_onSceneEditPush(mac, m->id, m->prevHash);
                break;
            }
            case MsgType::RequestManifest: {
                if (!_hasExactLen<RequestManifestMsg>(len)) return;
                if (_instance->_onRequestManifest) _instance->_onRequestManifest();
                break;
            }
            case MsgType::SetSceneSync: {
                if (len < (int)sizeof(SetSceneSyncMsg)) return;
                auto* m = (SetSceneSyncMsg*)data;
                uint8_t own[6];
                WiFi.macAddress(own);
                if (memcmp(m->targetMac, own, 6) == 0) {
                    if (_instance->_onSetSceneSync) _instance->_onSetSceneSync(m->enabled != 0);
                }
                break;
            }
            case MsgType::ConfigChunk: {
                if (len < (int)offsetof(ConfigChunkMsg, data)) return;
                auto* m = (ConfigChunkMsg*)data;
                if (!_hasVariablePayloadLen(len, offsetof(ConfigChunkMsg, data), m->dataLen,
                                            CONFIG_CHUNK_DATA_SIZE))
                    return;
                if (_instance->_onConfigChunk) _instance->_onConfigChunk(mac, m);
                break;
            }
            case MsgType::TriggerUpdate: {
                if (len < (int)sizeof(TriggerUpdateMsg)) return;
                auto* m = (TriggerUpdateMsg*)data;
                uint8_t own[6];
                WiFi.macAddress(own);
                if (memcmp(m->targetMac, own, 6) == 0) {
                    Logger::i("[mesh] trigger-update rx");
                    if (_instance->_onTriggerUpdate) _instance->_onTriggerUpdate();
                }
                break;
            }
            case MsgType::CheckUpdate: {
                if (len < (int)sizeof(CheckUpdateMsg)) return;
                auto* m = (CheckUpdateMsg*)data;
                uint8_t own[6];
                WiFi.macAddress(own);
                if (memcmp(m->targetMac, own, 6) == 0) {
                    Logger::i("[mesh] check-update rx");
                    if (_instance->_onCheckUpdate) _instance->_onCheckUpdate();
                }
                break;
            }
            case MsgType::TimeSync: {
                if (len < (int)sizeof(TimeSyncMsg)) return;
                auto* m = (TimeSyncMsg*)data;
                if (_instance->_onTimeSync) _instance->_onTimeSync(m->epoch);
                break;
            }
            case MsgType::KeyExchangeInit: {
                if (len < (int)sizeof(KeyExchangeInitMsg)) return;
                auto* m = (KeyExchangeInitMsg*)data;
                uint8_t own[6];
                WiFi.macAddress(own);
                if (memcmp(m->targetMac, own, 6) == 0) _instance->_onKeyExchangeInit(mac, m);
                break;
            }
            case MsgType::KeyExchangeResp: {
                if (len < (int)sizeof(KeyExchangeRespMsg)) return;
                auto* m = (KeyExchangeRespMsg*)data;
                uint8_t own[6];
                WiFi.macAddress(own);
                if (memcmp(m->targetMac, own, 6) == 0) _instance->_onKeyExchangeResp(m);
                break;
            }
            case MsgType::MeshPolicy: {
                if (len < (int)sizeof(MeshPolicyMsg)) return;
                auto* m = (MeshPolicyMsg*)data;
                _instance->_reconcileMeshPolicy(_stateFromMsg(*m));
                break;
            }
            case MsgType::WifiRetry: {
                if (!_hasExactLen<WifiRetryMsg>(len)) return;
                if (_instance->_onWifiRetry) _instance->_onWifiRetry();
                break;
            }
            case MsgType::MeshSearch: {
                if (!_hasExactLen<MeshSearchMsg>(len)) return;
                if (_instance->_onMeshSearch) _instance->_onMeshSearch();
                break;
            }
            default:
                Logger::w("[mesh] unknown msg type %u len %d", (uint8_t)type, len);
                break;
        }
    }

    static void _onSent(const uint8_t*, esp_now_send_status_t status) {
        if (status != ESP_NOW_SEND_SUCCESS) Logger::d("[mesh] send failed");
    }
};

inline MeshManager* MeshManager::_instance = nullptr;
