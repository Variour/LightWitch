#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "../config/BoardProfile.h"
#include "../config/Config.h"
#include "../logging/Logger.h"

// The device's own softAP, in one place. Most boards open a fallback AP
// whenever no WiFi network is configured or reachable, as before. A board
// without one (boardHasFallbackAp(): the Buzzerboard) runs WiFi as a station
// only and opens a maintenance AP on request for a limited time (src/fleet/:
// L1 + R1 held, USB `config ap`, fleet `ap`).
class SoftAp {
   public:
    static bool fallbackAllowed() { return boardHasFallbackAp(); }

    // Every place where WiFi gives up on a station connection.
    static void startFallback() {
        if (!fallbackAllowed()) return;
        auto& c = Config::get();
        WiFi.softAP(c.deviceName, c.apPassword, 1);
        WiFi.setTxPower(WIFI_TX_POWER);
    }

    // A station connection is up: the fallback AP goes. A maintenance AP
    // stays until its time is up.
    static void stopFallback() {
        if (fallbackAllowed()) WiFi.softAPdisconnect(false);
    }

    // The AP password changed: a running AP restarts with it.
    static void passwordChanged() {
        if (fallbackAllowed()) {
            if (!WiFi.isConnected()) {
                auto& c = Config::get();
                WiFi.softAP(c.deviceName, c.apPassword, 1);
                Logger::i("[wifi] AP password updated live");
            }
            return;
        }
        if (_maintenanceOpen) {
            _startMaintenance();
            Logger::i("[wifi] maintenance AP restarted with the new password");
        }
    }

    // Opens (or extends) the maintenance AP for `minutes`, on the current
    // radio channel so ESP-NOW stays where it is. Boards with a fallback AP
    // have none: false.
    static bool openMaintenance(uint8_t minutes, uint32_t now) {
        if (fallbackAllowed()) return false;
        _until = now + (uint32_t)minutes * 60000UL;
        if (!_maintenanceOpen) {
            _maintenanceOpen = true;
            _startMaintenance();
        }
        Logger::i("[wifi] maintenance AP \"%s\" open for %u min", Config::get().deviceName,
                  minutes);
        return true;
    }

    static bool maintenanceOpen() { return _maintenanceOpen; }

    // Closes the maintenance AP once its time is up.
    static void tick(uint32_t now) {
        if (!_maintenanceOpen || (int32_t)(now - _until) < 0) return;
        Logger::i("[wifi] maintenance AP closed");
        off();
    }

    // Every AP off now (game session, charging mode, standby).
    static void off() {
        _maintenanceOpen = false;
        if (WiFi.getMode() & WIFI_MODE_AP) WiFi.softAPdisconnect(true);
    }

   private:
    static inline bool _maintenanceOpen = false;
    static inline uint32_t _until = 0;

    static void _startMaintenance() {
        auto& c = Config::get();
        uint8_t ch = 0;
        wifi_second_chan_t second;
        if (esp_wifi_get_channel(&ch, &second) != ESP_OK || ch == 0) ch = 1;
        WiFi.softAP(c.deviceName, c.apPassword, ch);
        WiFi.setTxPower(WIFI_TX_POWER);
    }
};
