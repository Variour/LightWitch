#pragma once
#include <Arduino.h>
#include <functional>
#include "../logging/Logger.h"

struct PeerInfo {
    uint8_t  mac[6]            = {};
    char     name[32]          = {};
    uint8_t  groupId           = 0;
    uint32_t lastSeen          = 0;
    bool     active            = false;
    int8_t   rssi              = -90;
    bool     sceneSyncEnabled  = true;

    bool online() const { return active && (millis() - lastSeen < 15000); }

    String macStr() const {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }
};

class PeerRegistry {
public:
    static constexpr uint8_t MAX_PEERS = 16;

    using ChangeCb = std::function<void()>;
    void setOnChange(ChangeCb cb) { _onChange = cb; }

    // Returns true if the peer was newly seen (not previously active)
    bool update(const uint8_t* mac, const char* name, uint8_t groupId, bool sceneSyncEnabled = true) {
        PeerInfo* p = _find(mac);
        bool isNew = (p == nullptr || !p->active);
        if (!p) p = _slot();
        if (!p) return false;
        bool nameChanged  = !isNew && strncmp(p->name, name, 32) != 0;
        bool groupChanged = !isNew && p->groupId != groupId;
        memcpy(p->mac, mac, 6);
        strlcpy(p->name, name, sizeof(p->name));
        p->groupId          = groupId;
        p->sceneSyncEnabled = sceneSyncEnabled;
        p->lastSeen         = millis();
        p->active           = true;
        if (isNew)
            Logger::i("[mesh] peer online: %s (group %u)", name, groupId);
        else if (nameChanged)
            Logger::i("[mesh] peer renamed: %s", name);
        else if (groupChanged)
            Logger::i("[mesh] peer %s moved to group %u", name, groupId);
        bool changed = isNew || nameChanged || groupChanged;
        if (changed && _onChange) _onChange();
        return isNew;
    }

    void updateRssi(const uint8_t* mac, int8_t rssi) {
        if (auto* p = _find(mac)) p->rssi = rssi;
    }

    void updateGroup(const uint8_t* mac, uint8_t groupId) {
        if (auto* p = _find(mac)) {
            if (p->groupId != groupId) {
                Logger::i("[mesh] peer %s group → %u", p->name, groupId);
                p->groupId = groupId;
                if (_onChange) _onChange();
            }
        }
    }

    void tick() {
        bool any = false;
        for (auto& p : _peers) {
            if (p.active && !p.online()) {
                Logger::i("[mesh] peer offline: %s", p.name);
                p.active = false;
                any = true;
            }
        }
        if (any && _onChange) _onChange();
    }

    PeerInfo* begin() { return _peers; }
    PeerInfo* end()   { return _peers + MAX_PEERS; }

private:
    PeerInfo _peers[MAX_PEERS];
    ChangeCb _onChange;

    PeerInfo* _find(const uint8_t* mac) {
        for (auto& p : _peers)
            if (p.active && memcmp(p.mac, mac, 6) == 0) return &p;
        return nullptr;
    }
    PeerInfo* _slot() {
        for (auto& p : _peers) if (!p.active) return &p;
        return nullptr;
    }
};
