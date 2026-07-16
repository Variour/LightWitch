#pragma once
#include "../config/Config.h"
#include "../mesh/PeerRegistry.h"
#include "Pattern.h"

// Color indicates signal strength to the nearest same-group peer:
//   red (far) → yellow (mid) → green (close), off when no peer is visible.
// RSSI floor (-90 dBm) → 0, ceiling (-30 dBm) → 1.0 at scale=1.0.
// Higher proximityScale reaches green from farther away.
class Proximity : public Pattern {
   public:
    void setPeers(PeerRegistry* peers) { _peers = peers; }
    void setGroupId(uint8_t groupId) { _groupId = groupId; }

    float getPeriod() const override { return 1000.0f; }

    void tick(uint32_t) override {
        if (!_peers) {
            _led->setColor(0, 0, 0);
            return;
        }

        uint8_t myGroup = _groupId;
        int8_t bestRssi = -127;
        bool found = false;

        for (const auto& p : *_peers) {
            if (!p.online()) continue;
            bool inGroup = false;
            for (uint8_t i = 0; i < p.lightCount && i < MAX_LIGHTS; i++)
                if (p.lightGroupIds[i] == myGroup) {
                    inGroup = true;
                    break;
                }
            if (!inGroup) continue;
            if (!found || p.rssi > bestRssi) {
                bestRssi = p.rssi;
                found = true;
            }
        }

        if (!found) {
            _led->setColor(0, 0, 0);
            return;
        }

        float norm = ((float)(bestRssi + 90)) / 60.0f;  // 0..1 over -90..-30 dBm
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        float t = norm * _cfg.proximityScale;
        if (t > 1.0f) t = 1.0f;

        // Red → Yellow → Green
        uint8_t r, g;
        if (t <= 0.5f) {
            r = 255;
            g = (uint8_t)(t * 2.0f * 255.0f);
        } else {
            r = (uint8_t)((1.0f - t) * 2.0f * 255.0f);
            g = 255;
        }

        uint8_t br = _cfg.brightness;
        _led->setColor((uint16_t)r * br / 255, (uint16_t)g * br / 255, 0);
    }

   private:
    PeerRegistry* _peers = nullptr;
    uint8_t _groupId = 0;
};
