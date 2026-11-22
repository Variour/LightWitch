#pragma once
#include <Arduino.h>

#include <functional>

#include "../config/Config.h"
#include "../logging/Logger.h"
#include "TorchSpread.h"

// LARP torches (docs/torch.md): every light with LightHardwareConfig::torch
// set is lit or out. An out torch renders dark; a lit one shows its group's
// look. The state lives in RAM only and every torch starts out at boot.
// Lit torches broadcast their groups; an out torch of the same group catches
// fire when one of them stays close (see TorchSpread.h).
class TorchManager {
   public:
    static_assert(MAX_GROUPS <= torch::MAX_FLAME_GROUPS, "TorchFlameMsg holds one bit per group");

    // Re-applies a light's effective config after its torch state changed.
    using ChangeFn = std::function<void(uint8_t lightIndex)>;
    // Broadcasts a TorchFlameMsg carrying the lit groups (bit per group id).
    using BeaconFn = std::function<void(uint8_t groupMask)>;

    void setChangeFn(ChangeFn fn) { _onChange = fn; }
    void setBeaconFn(BeaconFn fn) { _beacon = fn; }

    // True when the light is a torch that is out, i.e. renders dark.
    bool isOut(uint8_t lightIndex) const {
        if (lightIndex >= MAX_LIGHTS) return false;
        const auto& l = Config::get().lights[lightIndex];
        return l.exists && l.torch && !_lit[lightIndex];
    }

    // Lights (true) or puts out (false) a torch light. Ignored for a light
    // that is not a torch.
    void set(uint8_t lightIndex, bool lit) {
        if (lightIndex >= MAX_LIGHTS) return;
        const auto& l = Config::get().lights[lightIndex];
        if (!l.exists || !l.torch) {
            Logger::w("[torch] light %u is not a torch — ignored", lightIndex);
            return;
        }
        if (_lit[lightIndex] == lit) return;
        _lit[lightIndex] = lit;
        if (!lit) _outSinceMs[lightIndex] = millis();
        Logger::i("[torch] light %u %s", lightIndex, lit ? "lit" : "out");
        if (_onChange) _onChange(lightIndex);
    }

    // Called after a light's torch flag changed: a new torch starts out.
    void onTorchFlagChanged(uint8_t lightIndex) {
        if (lightIndex >= MAX_LIGHTS) return;
        _lit[lightIndex] = false;
        _outSinceMs[lightIndex] = millis();
        if (_onChange) _onChange(lightIndex);
    }

    // From MeshManager's TorchFlameMsg callback (WiFi task).
    void onFlame(const uint8_t* mac, uint8_t groupMask, int8_t rssi) {
        portENTER_CRITICAL(&_mux);
        _spread.onBeacon(mac, groupMask, rssi, millis());
        portEXIT_CRITICAL(&_mux);
    }

    void tick() {
        uint32_t now = millis();
        uint8_t litGroups = 0;
        Config::forEachLight([&](uint8_t i, LightHardwareConfig& l) {
            if (!l.torch) return;
            if (!_lit[i] && _shouldIgnite(l.groupId, _outSinceMs[i], now)) {
                Logger::i("[torch] light %u caught fire", i);
                set(i, true);
            }
            if (_lit[i] && l.groupId < torch::MAX_FLAME_GROUPS)
                litGroups |= (uint8_t)(1u << l.groupId);
        });
        if (litGroups && now - _lastBeaconMs >= torch::BEACON_INTERVAL_MS) {
            _lastBeaconMs = now;
            if (_beacon) _beacon(litGroups);
        }
    }

   private:
    bool _shouldIgnite(uint8_t groupId, uint32_t outSinceMs, uint32_t now) {
        portENTER_CRITICAL(&_mux);
        bool ignite = _spread.shouldIgnite(groupId, outSinceMs, now);
        portEXIT_CRITICAL(&_mux);
        return ignite;
    }

    bool _lit[MAX_LIGHTS] = {};
    uint32_t _outSinceMs[MAX_LIGHTS] = {};
    uint32_t _lastBeaconMs = 0;
    torch::Spread _spread;
    // onFlame() runs on the WiFi task, tick() on the loop task.
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    ChangeFn _onChange;
    BeaconFn _beacon;
};
