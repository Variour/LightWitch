#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <functional>
#include "MeshTypes.h"
#include "PeerRegistry.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../update/Updater.h"
#include "../version.h"

class MeshManager {
public:
    PeerRegistry peers;

    using LightConfigCb    = std::function<void(uint8_t groupId, const LightConfig&)>;
    using PresenceCb       = std::function<void(const uint8_t* mac, const char* name, bool isNew)>;
    using SetGroupCb       = std::function<void(const uint8_t* targetMac, uint8_t lightIndex, uint8_t groupId)>;
    using GroupSyncCb      = std::function<void(const GroupConfig&)>;
    using PhaseSyncCb      = std::function<void(uint8_t groupId, float phase)>;
    // Returns the current animation phase for the given light index.
    using GetPhaseCb       = std::function<float(uint8_t lightIndex)>;
    using SceneManifestCb  = std::function<void(const uint8_t* mac, const SceneManifestMsg*)>;
    using SceneRequestCb   = std::function<void(const uint8_t* mac, const char* id)>;
    using SceneChunkCb     = std::function<void(const SceneChunkMsg*)>;
    using SceneForceSetCb  = std::function<void(const char* id, uint32_t hash)>;
    using SetSceneSyncCb   = std::function<void(bool enabled)>;
    using ConfigChunkCb    = std::function<void(const uint8_t* srcMac, const ConfigChunkMsg*)>;
    using TriggerUpdateCb  = std::function<void()>;
    // Called when this device is told to check for a firmware update (no auto-install)
    using CheckUpdateCb    = std::function<void()>;
    using PeerHeardCb      = std::function<void()>;

    void setOnPeerHeard(PeerHeardCb cb)           { _onPeerHeard      = cb; }
    void setOnLightConfig(LightConfigCb cb)       { _onLightConfig    = cb; }
    void setOnPresence(PresenceCb cb)             { _onPresence       = cb; }
    void setOnSetGroup(SetGroupCb cb)             { _onSetGroup       = cb; }
    void setOnGroupSync(GroupSyncCb cb)           { _onGroupSync      = cb; }
    void setOnPhaseSync(PhaseSyncCb cb)           { _onPhaseSync      = cb; }
    void setGetPhase(GetPhaseCb cb)               { _getPhase         = cb; }
    void setOnSceneManifest(SceneManifestCb cb)   { _onSceneManifest  = cb; }
    void setOnSceneRequest(SceneRequestCb cb)     { _onSceneRequest   = cb; }
    void setOnSceneChunk(SceneChunkCb cb)         { _onSceneChunk     = cb; }
    void setOnSceneForceSet(SceneForceSetCb cb)   { _onSceneForceSet  = cb; }
    void setOnSetSceneSync(SetSceneSyncCb cb)     { _onSetSceneSync   = cb; }
    void setOnConfigChunk(ConfigChunkCb cb)       { _onConfigChunk    = cb; }
    void setOnTriggerUpdate(TriggerUpdateCb cb)   { _onTriggerUpdate  = cb; }
    void setOnCheckUpdate(CheckUpdateCb cb)       { _onCheckUpdate    = cb; }

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

        if (now - _lastHeartbeat >= 5000) {
            _lastHeartbeat = now;
            _sendPresence();
        }

        // Proximity: enable sniffer + ping if any light is in proximity mode
        bool inProximity = _anyProximity();
        _setSnifferEnabled(inProximity);
        if (inProximity && now - _lastProximityPing >= 500) {
            _lastProximityPing = now;
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                auto& l = Config::get().lights[i];
                if (!l.exists) continue;
                GroupConfig* g = Config::group(l.groupId);
                if (!g || g->light.mode != GroupMode::Proximity) continue;
                ProximityPingMsg msg;
                msg.groupId = l.groupId;
                _send(&msg, sizeof(msg));
            }
        }

        // Phase sync: broadcast for each local light group where this device is master
        if (now - _lastPhaseBroadcast >= 2000 && _getPhase) {
            _lastPhaseBroadcast = now;
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                auto& l = Config::get().lights[i];
                if (!l.exists) continue;
                if (!_isSyncMaster(l.groupId)) continue;
                GroupConfig* g = Config::group(l.groupId);
                if (!g || !g->syncEnabled) continue;
                broadcastPhaseSync(l.groupId, _getPhase(i));
            }
        }
    }

    void broadcastLightConfig(uint8_t groupId, const LightConfig& cfg) {
        if (!_ready) return;
        LightConfigMsg msg;
        msg.type    = MsgType::LightConfig;
        msg.groupId = groupId;
        msg.config  = cfg;
        _lastSentGroup = groupId;
        _lastSentSeq   = cfg.seq;
        _send(&msg, sizeof(msg));
    }

    void broadcastSetGroup(const uint8_t* targetMac, uint8_t lightIndex, uint8_t groupId) {
        if (!_ready) return;
        SetGroupMsg msg;
        msg.type       = MsgType::SetGroup;
        msg.lightIndex = lightIndex;
        msg.groupId    = groupId;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    void broadcastGroupSync(const GroupConfig& g) {
        if (!_ready) return;
        GroupSyncMsg msg;
        msg.type  = MsgType::GroupSync;
        msg.group = g;
        _send(&msg, sizeof(msg));
    }

    void broadcastPhaseSync(uint8_t groupId, float phase) {
        if (!_ready) return;
        PhaseSyncMsg msg;
        msg.type    = MsgType::PhaseSync;
        msg.groupId = groupId;
        msg.phase   = phase;
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneManifest(const SceneManifestMsg& msg) {
        if (!_ready) return;
        size_t msgSize = offsetof(SceneManifestMsg, entries) + msg.count * sizeof(SceneManifestEntry);
        _send(&msg, msgSize);
    }

    void broadcastSceneRequest(const char* id) {
        if (!_ready) return;
        SceneRequestMsg msg;
        msg.type = MsgType::SceneRequest;
        strlcpy(msg.id, id, 33);
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneChunk(const SceneChunkMsg& msg) {
        if (!_ready) return;
        _send(&msg, sizeof(msg));
    }

    void broadcastSceneForceSet(const char* id, uint32_t hash) {
        if (!_ready) return;
        SceneForceSetMsg msg;
        msg.type = MsgType::SceneForceSet;
        strlcpy(msg.id, id, 33);
        msg.hash = hash;
        _send(&msg, sizeof(msg));
    }

    void sendConfigChunks(const uint8_t* targetMac, const char* json, size_t len) {
        if (!_ready) return;
        uint16_t totalChunks = (uint16_t)((len + CONFIG_CHUNK_DATA_SIZE - 1) / CONFIG_CHUNK_DATA_SIZE);
        for (uint16_t i = 0; i < totalChunks; i++) {
            ConfigChunkMsg msg;
            memcpy(msg.targetMac, targetMac, 6);
            msg.chunkIndex  = i;
            msg.totalChunks = totalChunks;
            size_t offset   = (size_t)i * CONFIG_CHUNK_DATA_SIZE;
            size_t chunkLen = min((size_t)CONFIG_CHUNK_DATA_SIZE, len - offset);
            msg.dataLen = (uint16_t)chunkLen;
            memcpy(msg.data, json + offset, chunkLen);
            _send(&msg, sizeof(msg));
            if (i < totalChunks - 1) delay(20);
        }
        Logger::i("[mesh] config push: %u bytes in %u chunks", (unsigned)len, totalChunks);
    }

    void broadcastSetSceneSync(const uint8_t* targetMac, bool enabled) {
        if (!_ready) return;
        SetSceneSyncMsg msg;
        msg.type    = MsgType::SetSceneSync;
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

    void broadcastAllGroups() {
        for (uint8_t i = 0; i < MAX_GROUPS; i++)
            if (Config::get().groups[i].exists)
                broadcastGroupSync(Config::get().groups[i]);
    }

    bool isReady() const { return _ready; }

private:
    bool     _ready                = false;
    bool     _snifferRunning       = false;
    uint32_t _lastHeartbeat        = 0;
    uint32_t _lastPhaseBroadcast   = 0;
    uint32_t _lastProximityPing    = 0;
    uint8_t  _lastSentGroup      = 0xFF;
    uint32_t _lastSentSeq        = UINT32_MAX;

    PeerHeardCb     _onPeerHeard;
    LightConfigCb   _onLightConfig;
    PresenceCb      _onPresence;
    SetGroupCb      _onSetGroup;
    GroupSyncCb     _onGroupSync;
    PhaseSyncCb     _onPhaseSync;
    GetPhaseCb      _getPhase;
    SceneManifestCb _onSceneManifest;
    SceneRequestCb  _onSceneRequest;
    SceneChunkCb    _onSceneChunk;
    SceneForceSetCb _onSceneForceSet;
    SetSceneSyncCb  _onSetSceneSync;
    ConfigChunkCb   _onConfigChunk;
    TriggerUpdateCb _onTriggerUpdate;
    CheckUpdateCb   _onCheckUpdate;

    static MeshManager* _instance;

    void _send(const void* data, size_t len) {
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        esp_now_send(bcast, (uint8_t*)data, len);
    }

    void _addBroadcastPeer() {
        esp_now_peer_info_t peer{};
        memset(peer.peer_addr, 0xFF, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    bool _anyProximity() {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (!l.exists) continue;
            GroupConfig* g = Config::group(l.groupId);
            if (g && g->light.mode == GroupMode::Proximity) return true;
        }
        return false;
    }

    void _sendPresence() {
        PresenceMsg msg;
        msg.type             = MsgType::Presence;
        msg.sceneSyncEnabled = Config::get().sceneSyncEnabled ? 1 : 0;
        strlcpy(msg.name, Config::get().deviceName, sizeof(msg.name));
        msg.wifiConnected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
        strlcpy(msg.fwVersion, FW_VERSION, sizeof(msg.fwVersion));
        const auto& us = Updater::status();
        msg.fwState = (uint8_t)(
            us.state == Updater::State::Checking    ? FwState::Checking    :
            us.state == Updater::State::Downloading ? FwState::Downloading :
            us.state == Updater::State::Error       ? FwState::Error       :
            us.state == Updater::State::Done        ? FwState::Done        : FwState::Idle);

        msg.lightCount = 0;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            msg.lightGroupIds[i] = 0xFF;
            msg.lightNames[i][0] = '\0';
        }
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (!l.exists) continue;
            if (msg.lightCount < MAX_LIGHTS) {
                uint8_t slot = msg.lightCount++;
                msg.lightGroupIds[slot] = l.groupId;
                strlcpy(msg.lightNames[slot], l.name, 20);
            }
        }
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
                if (p.lightGroupIds[i] == groupId) { inGroup = true; break; }
            if (!inGroup) continue;
            if (memcmp(p.mac, ownMac, 6) < 0) return false;
        }
        return true;
    }

    void _setSnifferEnabled(bool enable) {
        if (enable == _snifferRunning) return;
        if (enable) {
            wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
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
                if (len < (int)sizeof(PresenceMsg)) return;
                auto* m = (PresenceMsg*)data;
                if (m->version != PRESENCE_MSG_VERSION) return;
                bool isNew = _instance->peers.update(mac, m->name,
                    m->lightCount, m->lightGroupIds, m->lightNames,
                    m->sceneSyncEnabled != 0, m->wifiConnected != 0,
                    m->fwVersion, (FwState)m->fwState);
                if (_instance->_onPeerHeard) _instance->_onPeerHeard();
                if (_instance->_onPresence) _instance->_onPresence(mac, m->name, isNew);
                if (isNew) _instance->broadcastAllGroups();
                break;
            }
            case MsgType::LightConfig: {
                if (len < (int)sizeof(LightConfigMsg)) return;
                auto* m = (LightConfigMsg*)data;
                if (m->groupId == _instance->_lastSentGroup &&
                    m->config.seq == _instance->_lastSentSeq) return;
                Logger::d("[mesh] light config rx group %u pattern %u seq %u",
                          m->groupId, (uint8_t)m->config.pattern, m->config.seq);
                if (_instance->_onLightConfig) _instance->_onLightConfig(m->groupId, m->config);
                break;
            }
            case MsgType::SetGroup: {
                if (len < (int)sizeof(SetGroupMsg)) return;
                auto* m = (SetGroupMsg*)data;
                Logger::i("[mesh] set-group rx: target %02x:%02x:%02x:%02x:%02x:%02x light %u → group %u",
                          m->targetMac[0], m->targetMac[1], m->targetMac[2],
                          m->targetMac[3], m->targetMac[4], m->targetMac[5],
                          m->lightIndex, m->groupId);
                if (_instance->_onSetGroup) _instance->_onSetGroup(m->targetMac, m->lightIndex, m->groupId);
                _instance->peers.updateLightGroup(mac, m->lightIndex, m->groupId);
                break;
            }
            case MsgType::GroupSync: {
                if (len < (int)sizeof(GroupSyncMsg)) return;
                auto* m = (GroupSyncMsg*)data;
                Logger::i("[mesh] group-sync rx: group %u \"%s\" exists=%d",
                          m->group.id, m->group.name, m->group.exists);
                if (_instance->_onGroupSync) _instance->_onGroupSync(m->group);
                break;
            }
            case MsgType::PhaseSync: {
                if (len < (int)sizeof(PhaseSyncMsg)) return;
                auto* m = (PhaseSyncMsg*)data;
                if (_instance->_onPhaseSync) _instance->_onPhaseSync(m->groupId, m->phase);
                break;
            }
            case MsgType::ProximityPing:
                break;
            case MsgType::SceneManifest: {
                if (len < (int)(sizeof(SceneManifestMsg) - sizeof(SceneManifestEntry) * MANIFEST_ENTRIES_PER_MSG)) return;
                auto* m = (SceneManifestMsg*)data;
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
                if (len < (int)(sizeof(SceneChunkMsg) - CHUNK_DATA_SIZE)) return;
                auto* m = (SceneChunkMsg*)data;
                if (_instance->_onSceneChunk) _instance->_onSceneChunk(m);
                break;
            }
            case MsgType::SceneForceSet: {
                if (len < (int)sizeof(SceneForceSetMsg)) return;
                auto* m = (SceneForceSetMsg*)data;
                if (_instance->_onSceneForceSet) _instance->_onSceneForceSet(m->id, m->hash);
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
                if (len < (int)(sizeof(ConfigChunkMsg) - CONFIG_CHUNK_DATA_SIZE)) return;
                auto* m = (ConfigChunkMsg*)data;
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
            default:
                Logger::w("[mesh] unknown msg type %u len %d", (uint8_t)type, len);
                break;
        }
    }

    static void _onSent(const uint8_t*, esp_now_send_status_t status) {
        if (status != ESP_NOW_SEND_SUCCESS)
            Logger::d("[mesh] send failed");
    }
};

inline MeshManager* MeshManager::_instance = nullptr;
