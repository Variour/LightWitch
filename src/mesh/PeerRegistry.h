#pragma once
#include <Arduino.h>

#include <functional>

#include "../logging/Logger.h"
#include "MeshTypes.h"

struct PeerInfo {
    uint8_t mac[6] = {};
    char name[32] = {};
    uint8_t lightCount = 0;
    uint8_t lightGroupIds[MAX_LIGHTS] = {0xFF, 0xFF, 0xFF, 0xFF};
    char lightNames[MAX_LIGHTS][20] = {};
    bool hasSound = false;
    uint8_t soundAudioGroupId = 0;
    uint8_t soundVolume = 0;
    char soundName[20] = {};
    uint32_t lastSeen = 0;
    bool active = false;
    int8_t rssi = -90;
    bool sceneSyncEnabled = true;
    bool wifiConnected = false;
    bool hasWifiNetworks = false;
    bool wifiConnecting = false;
    char fwVersion[16] = {};
    FwState fwState = FwState::Idle;
    bool batteryPresent = false;
    uint8_t batteryPercent = 0;
    bool batteryCharging = false;

    bool online() const { return active && (millis() - lastSeen < 15000); }

    String macStr() const {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        return String(buf);
    }
};

class PeerRegistry {
   public:
    static constexpr uint8_t MAX_PEERS = 16;

    using ChangeCb = std::function<void()>;
    void setOnChange(ChangeCb cb) { _onChange = cb; }

    bool update(const uint8_t* mac, const char* name, uint8_t lightCount,
                const uint8_t lightGroupIds[MAX_LIGHTS],
                const char lightNames_[MAX_LIGHTS][20] = nullptr, bool sceneSyncEnabled = true,
                bool wifiConnected = false, const char* fwVersion = "",
                FwState fwState = FwState::Idle, bool hasWifiNetworks = false,
                bool wifiConnecting = false, bool batteryPresent = false,
                uint8_t batteryPercent = 0, bool batteryCharging = false, bool hasSound = false,
                uint8_t soundAudioGroupId = 0, uint8_t soundVolume = 0,
                const char* soundName = "") {
        PeerInfo* p = _find(mac);
        bool isNew = (p == nullptr || !p->active);
        if (!p) p = _slot();
        if (!p) return false;
        bool nameChanged = !isNew && strncmp(p->name, name, 32) != 0;
        bool lightsChanged = false;
        if (!isNew) {
            if (p->lightCount != lightCount) {
                lightsChanged = true;
            } else {
                for (uint8_t i = 0; i < lightCount && i < MAX_LIGHTS; i++)
                    if (p->lightGroupIds[i] != lightGroupIds[i]) {
                        lightsChanged = true;
                        break;
                    }
            }
        }
        memcpy(p->mac, mac, 6);
        strlcpy(p->name, name, sizeof(p->name));
        p->lightCount = lightCount < MAX_LIGHTS ? lightCount : MAX_LIGHTS;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            p->lightGroupIds[i] = (i < lightCount) ? lightGroupIds[i] : 0xFF;
            if (lightNames_ && i < lightCount)
                strlcpy(p->lightNames[i], lightNames_[i], 20);
            else if (i >= lightCount)
                p->lightNames[i][0] = '\0';
        }
        p->sceneSyncEnabled = sceneSyncEnabled;
        p->wifiConnected = wifiConnected;
        p->hasWifiNetworks = hasWifiNetworks;
        p->wifiConnecting = wifiConnecting;
        strlcpy(p->fwVersion, fwVersion, sizeof(p->fwVersion));
        p->fwState = fwState;
        p->batteryPresent = batteryPresent;
        p->batteryPercent = batteryPercent;
        p->batteryCharging = batteryCharging;
        p->hasSound = hasSound;
        p->soundAudioGroupId = soundAudioGroupId;
        p->soundVolume = soundVolume;
        strlcpy(p->soundName, soundName, sizeof(p->soundName));
        p->lastSeen = millis();
        p->active = true;
        if (isNew)
            Logger::i("[mesh] peer online: %s (%u light(s))", name, lightCount);
        else if (nameChanged)
            Logger::i("[mesh] peer renamed: %s", name);
        else if (lightsChanged)
            Logger::i("[mesh] peer %s lights updated", name);
        bool changed = isNew || nameChanged || lightsChanged;
        if (changed && _onChange) _onChange();
        return isNew;
    }

    void updateRssi(const uint8_t* mac, int8_t rssi) {
        if (auto* p = _find(mac)) p->rssi = rssi;
    }

    void updateLightGroup(const uint8_t* mac, uint8_t lightIndex, uint8_t groupId) {
        if (lightIndex >= MAX_LIGHTS) return;
        if (auto* p = _find(mac)) {
            if (p->lightGroupIds[lightIndex] != groupId) {
                Logger::i("[mesh] peer %s light %u → group %u", p->name, lightIndex, groupId);
                p->lightGroupIds[lightIndex] = groupId;
                if (_onChange) _onChange();
            }
        }
    }

    void updateSoundGroup(const uint8_t* mac, uint8_t audioGroupId) {
        if (auto* p = _find(mac)) {
            if (p->soundAudioGroupId != audioGroupId) {
                Logger::i("[mesh] peer %s sound → audio group %u", p->name, audioGroupId);
                p->soundAudioGroupId = audioGroupId;
                if (_onChange) _onChange();
            }
        }
    }

    void updateSoundVolume(const uint8_t* mac, uint8_t volume) {
        if (auto* p = _find(mac)) {
            if (p->soundVolume != volume) {
                p->soundVolume = volume;
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
    PeerInfo* end() { return _peers + MAX_PEERS; }

   private:
    PeerInfo _peers[MAX_PEERS];
    ChangeCb _onChange;

    PeerInfo* _find(const uint8_t* mac) {
        for (auto& p : _peers)
            if (p.active && memcmp(p.mac, mac, 6) == 0) return &p;
        return nullptr;
    }
    PeerInfo* _slot() {
        for (auto& p : _peers)
            if (!p.active) return &p;
        return nullptr;
    }
};
