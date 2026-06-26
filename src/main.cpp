#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

#include "config/Config.h"
#include "version.h"
#include "logging/Logger.h"
#include "led/Ws2812bDriver.h"
#include "led/Ws2801Driver.h"
#include "patterns/PatternRunner.h"
#include "mesh/MeshManager.h"
#include "web/WebServer.h"
#include "mqtt/MqttManager.h"
#include "scenes/SceneSyncManager.h"

static Ws2812bDriver    _ws2812b;
static Ws2801Driver     _ws2801;
static LedDriver*       led       = nullptr;
static PatternRunner    runner;
static MeshManager      mesh;
static BatteryWebServer webServer;
static MqttManager      mqtt;
static SceneSyncManager sceneSync;
static bool             _otaActive = false;

// Reassembly buffer for incoming config push chunks
static String   _cfgSyncBuf;
static uint16_t _cfgSyncExpected = 0;

static void serialSink(LogLevel level, const char* msg) {
    const char* p = level == LogLevel::ERROR ? "[E]"
                  : level == LogLevel::WARN  ? "[W]"
                  : level == LogLevel::INFO  ? "[I]" : "[D]";
    Serial.printf("%s %s\n", p, msg);
}

// ── Light / group helpers ─────────────────────────────────────────────────────

// Apply this device's current group light config to the pattern runner
static void applyActiveLight() {
    runner.applyConfig(Config::light());
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
static void setupWifi() {
    auto& c = Config::get();

    // WIFI_AP_STA is required for stable OTA and ESP-NOW coexistence on ESP32.
    // When STA connects we stop the AP beacon via softAPdisconnect() rather than
    // switching mode, which would break OTA (triggers ASSOC_LEAVE mid-transfer).
    WiFi.mode(WIFI_AP_STA);
    WiFi.setTxPower(WIFI_TX_POWER);

    if (strlen(c.wifiSsid) == 0) {
        WiFi.softAP(c.deviceName, c.apPassword);
        Logger::i("[wifi] No SSID configured, AP: %s  IP: %s",
                  c.deviceName, WiFi.softAPIP().toString().c_str());
        return;
    }

    Logger::i("[wifi] Connecting to %s ...", c.wifiSsid);
    // Disconnect first to clear any stale PMK/connection state from previous boots.
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(c.wifiSsid, c.wifiPassword);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(250);

    if (WiFi.status() == WL_CONNECTED) {
        // Stop AP beacon — no SSID visible, no clients accepted.
        // The AP hardware stays up so WIFI_AP_STA mode is preserved.
        WiFi.softAPdisconnect(false);
        Logger::i("[wifi] Connected, IP: %s  (AP off)", WiFi.localIP().toString().c_str());
    } else {
        Logger::w("[wifi] Failed to connect, falling back to AP");
        WiFi.setAutoReconnect(false);
        WiFi.softAP(c.deviceName, c.apPassword);
        Logger::i("[wifi] AP: %s  IP: %s", c.deviceName, WiFi.softAPIP().toString().c_str());
    }
}

// ── OTA ──────────────────────────────────────────────────────────────────────
static void setupOta() {
    auto& c = Config::get();
    ArduinoOTA.setHostname(c.deviceName);
    ArduinoOTA.setPort(c.otaPort);
    ArduinoOTA.onStart([]() {
        _otaActive = true;
        const char* what = ArduinoOTA.getCommand() == U_SPIFFS ? "filesystem" : "firmware";
        Logger::i("[ota] %s update starting", what);
    });
    ArduinoOTA.onEnd([]()    { _otaActive = false; Logger::i("[ota] done");  });
    ArduinoOTA.onError([](ota_error_t e) { Logger::e("[ota] error %u: %s", e, Update.errorString()); });
    ArduinoOTA.begin();
    Logger::i("[ota] ready at %s:%u", c.deviceName, c.otaPort);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Logger::addSink(serialSink);

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Logger::e("[fs] mount failed");
    } else {
        File root = LittleFS.open("/");
        File f = root.openNextFile();
        while (f) { Logger::i("[fs] %s (%u bytes)", f.name(), f.size()); f = root.openNextFile(); }
    }
    Config::load();
    Logger::setLevel((LogLevel)Config::get().logLevel);
    Logger::i("[sys] firmware %s  device: %s  group: %u",
              FW_VERSION, Config::get().deviceName, Config::get().groupId);

    setupWifi();

    if (Config::get().ledType == LedType::WS2801) {
        _ws2801.begin();
        led = &_ws2801;
        Logger::i("[led] WS2801 data=GPIO%d clock=GPIO%d", Ws2801Driver::DATA_PIN, Ws2801Driver::CLOCK_PIN);
    } else {
        _ws2812b.begin();
        led = &_ws2812b;
        Logger::i("[led] WS2812B data=GPIO%d", Ws2812bDriver::DATA_PIN);
    }
    runner.begin(*led);
    applyActiveLight();

    mqtt.setOnCommand([](const LightConfig& cfg) {
        GroupConfig* g = Config::group(Config::get().groupId);
        if (!g) return;
        g->light = cfg;
        Config::save();
        runner.applyConfig(cfg);
        mesh.broadcastLightConfig(Config::get().groupId, cfg);
        if (g) mesh.broadcastGroupSync(*g);
        mqtt.publishState(cfg);
    });
    mqtt.begin(Config::get());

    if (MDNS.begin(Config::get().deviceName))
        Logger::i("[mdns] http://%s.local", Config::get().deviceName);

    if (Config::get().otaEnabled) setupOta();
    mesh.begin();
    runner.setPeerRegistry(&mesh.peers);

    // Wire SceneSyncManager → MeshManager
    sceneSync.setBroadcastFns(
        [](const char* id, uint32_t hash) { mesh.broadcastSceneForceSet(id, hash); },
        [](const char* id)                { mesh.broadcastSceneRequest(id); },
        [](const SceneChunkMsg& msg)      { mesh.broadcastSceneChunk(msg); },
        [](const SceneManifestMsg& msg)   { mesh.broadcastSceneManifest(msg); }
    );

    // ── Mesh callbacks ────────────────────────────────────────────────────────

    // Received light config for a group
    mesh.setOnLightConfig([](uint8_t groupId, const LightConfig& cfg) {
        GroupConfig* g = Config::group(groupId);
        if (!g) { Logger::w("[mesh] light config for unknown group %u — ignored", groupId); return; }
        g->light = cfg;
        Config::save();
        if (groupId == Config::get().groupId) {
            runner.applyConfig(cfg);
            mqtt.publishState(cfg);
        }
    });

    // Peer came online
    mesh.setOnPresence([](const uint8_t*, const char*, uint8_t, bool isNew) {
        webServer.pushPeers();
        if (isNew) sceneSync.onNewPeer();
    });

    // Scene sync mesh callbacks
    mesh.setOnSceneManifest([](const uint8_t* mac, const SceneManifestMsg* msg) {
        sceneSync.onManifest(mac, msg);
    });
    mesh.setOnSceneRequest([](const uint8_t* mac, const char* id) {
        sceneSync.onRequest(mac, id);
    });
    mesh.setOnSceneChunk([](const SceneChunkMsg* msg) {
        sceneSync.onChunk(msg);
    });
    mesh.setOnSceneForceSet([](const char* id, uint32_t hash) {
        sceneSync.onForceSet(id, hash);
    });
    mesh.setOnSetSceneSync([](bool enabled) {
        sceneSync.onSetSceneSync(enabled);
    });

    mesh.setOnConfigChunk([](const uint8_t* srcMac, const ConfigChunkMsg* msg) {
        uint8_t own[6];
        WiFi.macAddress(own);
        if (memcmp(srcMac, own, 6) == 0) return;  // ignore own broadcast
        static const uint8_t kAllZeros[6] = {};
        bool isAll = memcmp(msg->targetMac, kAllZeros, 6) == 0;
        if (!isAll && memcmp(msg->targetMac, own, 6) != 0) return;
        if (msg->chunkIndex == 0) {
            _cfgSyncBuf     = "";
            _cfgSyncExpected = msg->totalChunks;
        }
        if (msg->totalChunks != _cfgSyncExpected) return;
        _cfgSyncBuf.concat((const char*)msg->data, msg->dataLen);
        if (msg->chunkIndex == _cfgSyncExpected - 1) {
            Logger::i("[cfg] config sync received (%u bytes), applying", (unsigned)_cfgSyncBuf.length());
            Config::applyConfigSync(_cfgSyncBuf.c_str(), _cfgSyncBuf.length());
        }
    });

    // Another device told this device (or a peer) to change group
    mesh.setOnSetGroup([](const uint8_t* targetMac, uint8_t groupId) {
        uint8_t own[6];
        WiFi.macAddress(own);
        if (memcmp(targetMac, own, 6) == 0) {
            if (Config::group(groupId)) {
                Config::get().groupId = groupId;
                Config::save();
                applyActiveLight();
                Logger::i("[mesh] moved to group %u", groupId);
            }
        }
        // Update peer registry group field
        mesh.peers.updateGroup(targetMac, groupId);
        webServer.pushPeers();
    });

    // Group list changed (create / rename / delete from another device)
    mesh.setOnGroupSync([](const GroupConfig& g) {
        bool lightUpdated = Config::applyGroupSync(g);
        Config::save();
        if (!Config::group(Config::get().groupId)) {
            Logger::i("[mesh] active group deleted — falling back to Default");
            Config::get().groupId = 0;
            applyActiveLight();
        } else if (lightUpdated && g.id == Config::get().groupId && g.exists) {
            // Incoming light config has a higher seq than ours — apply it.
            // This covers rejoining devices receiving the group's current state,
            // while ignoring stale syncs from devices that were themselves offline.
            applyActiveLight();
        }
        webServer.pushGroups();
    });

    // Received phase sync from the group's sync master
    mesh.setOnPhaseSync([](uint8_t groupId, float phase) {
        if (groupId != Config::get().groupId) return;
        GroupConfig* g = Config::group(groupId);
        if (!g || !g->syncEnabled) {
            return;
        }
        runner.snapPhase(phase);
    });

    // Provide current phase for the periodic sync broadcast
    mesh.setGetPhase([](){ return runner.getPhase(); });

    // ── Web server callbacks ──────────────────────────────────────────────────

    webServer.begin(
        // onGroupChange: this device's group changed via web UI
        []() { applyActiveLight(); },

        // onGroupLight: a group's light config changed via web UI → broadcast
        [](uint8_t groupId, const LightConfig& cfg) {
            if (groupId == Config::get().groupId) {
                runner.applyConfig(cfg);
                mqtt.publishState(cfg);
            }
            mesh.broadcastLightConfig(groupId, cfg);
            if (auto* g = Config::group(groupId)) mesh.broadcastGroupSync(*g);
        },

        // onGroupSync: group created/renamed/deleted via web UI → broadcast
        [](const GroupConfig& g) { mesh.broadcastGroupSync(g); },

        // onSetRemote: move a remote peer's group via web UI
        [](const uint8_t* mac, uint8_t groupId) { mesh.broadcastSetGroup(mac, groupId); },

        &mesh.peers,
        &sceneSync,

        // onSetRemoteSync: toggle sceneSyncEnabled on a remote device
        [](const uint8_t* mac, bool enabled) { mesh.broadcastSetSceneSync(mac, enabled); },

        // onResolveConflict: user picked a winner for a conflicted scene
        [](const char* id, const uint8_t* sourceMac) {
            if (sourceMac == nullptr) {
                // Local copy wins — force-set and broadcast chunks
                sceneSync.resolveWithLocal(id);
            } else {
                // Remote copy wins — mark as forced accept, request chunks from the mesh.
                // On receive, SceneSyncManager will save unconditionally and re-broadcast.
                sceneSync.setForcedAccept(id);
                mesh.broadcastSceneRequest(id);
            }
        },

        // onPushConfig: push syncable settings to one or all peers via ESP-NOW
        [](const uint8_t* targetMac, const char* json, size_t len) {
            mesh.sendConfigChunks(targetMac, json, len);
        }
    );

    Logger::i("[sys] ready");
    if (WiFi.status() == WL_CONNECTED) Updater::checkAsync();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    if (Config::get().otaEnabled) ArduinoOTA.handle();
    webServer.loop();
    if (!_otaActive) {
        mesh.tick();
        mqtt.loop();
        runner.tick();
        sceneSync.tick();
    }
}
