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

class MeshManager {
public:
    PeerRegistry peers;

    using LightConfigCb    = std::function<void(uint8_t groupId, const LightConfig&)>;
    using PresenceCb       = std::function<void(const uint8_t* mac, const char* name, uint8_t groupId, bool isNew)>;
    using SetGroupCb       = std::function<void(const uint8_t* targetMac, uint8_t groupId)>;
    using GroupSyncCb      = std::function<void(const GroupConfig&)>;
    using PhaseSyncCb      = std::function<void(uint8_t groupId, float phase)>;
    using GetPhaseCb       = std::function<float()>;
    // Scene sync callbacks
    using SceneManifestCb  = std::function<void(const uint8_t* mac, const SceneManifestMsg*)>;
    using SceneRequestCb   = std::function<void(const uint8_t* mac, const char* id)>;
    using SceneChunkCb     = std::function<void(const SceneChunkMsg*)>;
    using SceneForceSetCb  = std::function<void(const char* id, uint32_t hash)>;
    using SetSceneSyncCb   = std::function<void(bool enabled)>;

    void setOnLightConfig(LightConfigCb cb)     { _onLightConfig    = cb; }
    void setOnPresence(PresenceCb cb)            { _onPresence       = cb; }
    void setOnSetGroup(SetGroupCb cb)            { _onSetGroup       = cb; }
    void setOnGroupSync(GroupSyncCb cb)          { _onGroupSync      = cb; }
    void setOnPhaseSync(PhaseSyncCb cb)          { _onPhaseSync      = cb; }
    void setGetPhase(GetPhaseCb cb)              { _getPhase         = cb; }
    void setOnSceneManifest(SceneManifestCb cb)  { _onSceneManifest  = cb; }
    void setOnSceneRequest(SceneRequestCb cb)    { _onSceneRequest   = cb; }
    void setOnSceneChunk(SceneChunkCb cb)        { _onSceneChunk     = cb; }
    void setOnSceneForceSet(SceneForceSetCb cb)  { _onSceneForceSet  = cb; }
    void setOnSetSceneSync(SetSceneSyncCb cb)    { _onSetSceneSync   = cb; }

    void begin() {
        _instance = this;
        if (esp_now_init() != ESP_OK) {
            Logger::e("[mesh] esp_now_init failed");
            return;
        }
        _addBroadcastPeer();
        esp_now_register_recv_cb(_onRecv);
        esp_now_register_send_cb(_onSent);
        // Promiscuous sniffer captures RSSI from raw 802.11 frames.
        // ESP-NOW action frames (subtype 0xD) carry source MAC at byte 10.
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(_promiscuousRecv);
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

        // Sync master broadcasts phase every 2 s (only for animated patterns in groups with sync on)
        bool isMaster = _isSyncMaster();
        if (isMaster != _wasSyncMaster) {
            _wasSyncMaster = isMaster;
            if (isMaster)
                Logger::i("[sync] became sync master for group %u", Config::get().groupId);
            else
                Logger::i("[sync] lost sync master for group %u", Config::get().groupId);
        }
        if (now - _lastProximityPing >= 500) {
            GroupConfig* g = Config::group(Config::get().groupId);
            if (g && g->light.mode == GroupMode::Proximity) {
                _lastProximityPing = now;
                ProximityPingMsg msg;
                msg.groupId = Config::get().groupId;
                _send(&msg, sizeof(msg));
            }
        }

        if (now - _lastPhaseBroadcast >= 2000 && isMaster) {
            _lastPhaseBroadcast = now;
            GroupConfig* g = Config::group(Config::get().groupId);
            if (g && g->syncEnabled && _getPhase) {
                broadcastPhaseSync(Config::get().groupId, _getPhase());
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

    void broadcastSetGroup(const uint8_t* targetMac, uint8_t groupId) {
        if (!_ready) return;
        SetGroupMsg msg;
        msg.type    = MsgType::SetGroup;
        msg.groupId = groupId;
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

    void broadcastSetSceneSync(const uint8_t* targetMac, bool enabled) {
        if (!_ready) return;
        SetSceneSyncMsg msg;
        msg.type    = MsgType::SetSceneSync;
        msg.enabled = enabled ? 1 : 0;
        memcpy(msg.targetMac, targetMac, 6);
        _send(&msg, sizeof(msg));
    }

    // Re-broadcast all known groups (called when a new peer is seen)
    void broadcastAllGroups() {
        for (uint8_t i = 0; i < MAX_GROUPS; i++)
            if (Config::get().groups[i].exists)
                broadcastGroupSync(Config::get().groups[i]);
    }

    bool isReady() const { return _ready; }

private:
    bool     _ready                = false;
    uint32_t _lastHeartbeat        = 0;
    uint32_t _lastPhaseBroadcast   = 0;
    uint32_t _lastProximityPing    = 0;
    uint8_t  _lastSentGroup      = 0xFF;
    uint32_t _lastSentSeq        = UINT32_MAX;
    bool     _wasSyncMaster      = false;

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

    static MeshManager* _instance;

    void _send(const void* data, size_t len) {
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        esp_now_send(bcast, (uint8_t*)data, len);
    }

    void _addBroadcastPeer() {
        esp_now_peer_info_t peer{};
        memset(peer.peer_addr, 0xFF, 6);
        peer.channel = WiFi.channel();
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    void _sendPresence() {
        PresenceMsg msg;
        msg.type              = MsgType::Presence;
        msg.groupId           = Config::get().groupId;
        msg.sceneSyncEnabled  = Config::get().sceneSyncEnabled ? 1 : 0;
        strlcpy(msg.name, Config::get().deviceName, sizeof(msg.name));
        _send(&msg, sizeof(msg));
    }

    // Returns true if this device has the lowest MAC among online peers in the same group.
    // The lowest-MAC device acts as the phase-sync master for its group.
    bool _isSyncMaster() {
        uint8_t ownMac[6];
        WiFi.macAddress(ownMac);
        uint8_t myGroup = Config::get().groupId;
        for (const auto& p : peers) {
            if (!p.online() || p.groupId != myGroup) continue;
            if (memcmp(p.mac, ownMac, 6) < 0) return false;
        }
        return true;
    }

    // Promiscuous callback fires for every received 802.11 frame.
    // Filter to action frames (ESP-NOW) and update the sender's RSSI in the registry.
    static void _promiscuousRecv(void* buf, wifi_promiscuous_pkt_type_t type) {
        if (type != WIFI_PKT_MGMT || !_instance) return;
        const auto* pkt = (const wifi_promiscuous_pkt_t*)buf;
        if (pkt->rx_ctrl.sig_len < 16) return;
        if ((pkt->payload[0] & 0xFC) != 0xD0) return;  // not an action frame
        _instance->peers.updateRssi(pkt->payload + 10, (int8_t)pkt->rx_ctrl.rssi);
    }

    static void _onRecv(const uint8_t* mac, const uint8_t* data, int len) {
        if (len < 1 || !_instance) return;
        MsgType type = (MsgType)data[0];

        switch (type) {
            case MsgType::Presence: {
                if (len < (int)sizeof(PresenceMsg)) return;
                auto* m = (PresenceMsg*)data;
                bool sceneSyncEnabled = m->sceneSyncEnabled != 0;
                bool isNew = _instance->peers.update(mac, m->name, m->groupId, sceneSyncEnabled);
                if (_instance->_onPresence) _instance->_onPresence(mac, m->name, m->groupId, isNew);
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
                Logger::i("[mesh] set-group rx: target %02x:%02x:%02x:%02x:%02x:%02x → group %u",
                          m->targetMac[0], m->targetMac[1], m->targetMac[2],
                          m->targetMac[3], m->targetMac[4], m->targetMac[5], m->groupId);
                if (_instance->_onSetGroup) _instance->_onSetGroup(m->targetMac, m->groupId);
                _instance->peers.updateGroup(mac, m->groupId);
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
                // RSSI already captured by the promiscuous sniffer; nothing else to do.
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
