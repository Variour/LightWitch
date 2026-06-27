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
#include "mesh/ChannelManager.h"
#include "web/WebServer.h"
#include "mqtt/MqttManager.h"
#include "scenes/SceneSyncManager.h"

static Ws2812bDriver    _ws2812bPool[MAX_LIGHTS];
static Ws2801Driver     _ws2801Pool[MAX_LIGHTS];
static LedDriver*       _leds[MAX_LIGHTS]    = {};
static PatternRunner    _runners[MAX_LIGHTS];
static MeshManager      mesh;
static ChannelManager   channelMgr;
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

// Apply each light's group config to its runner
static void applyAllLights() {
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
        auto& l = Config::get().lights[i];
        if (!l.exists || !_leds[i]) continue;
        auto* g = Config::group(l.groupId);
        if (g) _runners[i].applyConfig(g->light);
    }
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
static void setupWifi() {
    auto& c = Config::get();

    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);

    Config::loadWifi();
    uint8_t count = Config::wifiCount();

    if (count == 0) {
        WiFi.softAP(c.deviceName, c.apPassword, 1);
        WiFi.setTxPower(WIFI_TX_POWER);
        Logger::i("[wifi] No networks configured, AP: %s  IP: %s",
                  c.deviceName, WiFi.softAPIP().toString().c_str());
        return;
    }

    uint8_t last = Config::wifiLast();
    uint8_t tryOrder[MAX_WIFI_NETWORKS];
    uint8_t idx = 0;
    tryOrder[idx++] = last;
    for (uint8_t i = 0; i < count; i++) {
        if (i != last) tryOrder[idx++] = i;
    }

    for (uint8_t t = 0; t < count; t++) {
        uint8_t ni = tryOrder[t];
        const char* ssid = Config::wifiNetworks()[ni].ssid;
        const char* pass = Config::wifiNetworks()[ni].password;
        for (uint8_t attempt = 0; attempt < 3; attempt++) {
            WiFi.disconnect(false);
            delay(attempt == 0 ? 100 : 2000);
            Logger::i("[wifi] Trying %s (attempt %u/3)...", ssid, attempt + 1);
            WiFi.begin(ssid, pass);
            WiFi.setTxPower(WIFI_TX_POWER);
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(250);
            if (WiFi.status() == WL_CONNECTED) {
                WiFi.softAPdisconnect(false);
                Logger::i("[wifi] Connected to %s, IP: %s  (AP off)", ssid, WiFi.localIP().toString().c_str());
                if (ni != last) {
                    Config::setWifiLast(ni);
                    Config::saveWifi();
                }
                return;
            }
        }
    }

    WiFi.disconnect(false);
    delay(100);
    Logger::w("[wifi] All networks failed, falling back to AP");
    WiFi.softAP(c.deviceName, c.apPassword, 1);
    WiFi.setTxPower(WIFI_TX_POWER);
    Logger::i("[wifi] AP: %s  IP: %s", c.deviceName, WiFi.softAPIP().toString().c_str());
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
    Logger::i("[sys] firmware %s  device: %s", FW_VERSION, Config::get().deviceName);

    setupWifi();
    channelMgr.begin();

    // Initialise one driver + runner per configured light
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
        auto& l = Config::get().lights[i];
        if (!l.exists) continue;
        uint16_t numLeds = (uint16_t)l.width * l.height;
        if (numLeds == 0) numLeds = 1;
        LedDriver* drv;
        if (l.ledType == LedType::WS2801) {
            _ws2801Pool[i].setup(l.dataPin, l.clockPin, numLeds);
            _ws2801Pool[i].begin();
            drv = &_ws2801Pool[i];
            Logger::i("[led] light %u: WS2801 data=GPIO%d clock=GPIO%d leds=%u group=%u",
                      i, l.dataPin, l.clockPin, numLeds, l.groupId);
        } else {
            _ws2812bPool[i].setup(l.dataPin, numLeds);
            _ws2812bPool[i].begin();
            drv = &_ws2812bPool[i];
            Logger::i("[led] light %u: WS2812B data=GPIO%d leds=%u group=%u",
                      i, l.dataPin, numLeds, l.groupId);
        }
        _leds[i] = drv;
        _runners[i].begin(*drv);
        _runners[i].setDimensions(l.width, l.height);
        _runners[i].setPeerRegistry(&mesh.peers);
        _runners[i].setGroupId(l.groupId);
        auto* g = Config::group(l.groupId);
        if (g) _runners[i].applyConfig(g->light);
    }

    // Wire MQTT to the first active light's group for now
    mqtt.setOnCommand([](const LightConfig& cfg) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (!l.exists || !_leds[i]) continue;
            GroupConfig* g = Config::group(l.groupId);
            if (!g) continue;
            g->light = cfg;
            Config::save();
            _runners[i].applyConfig(cfg);
            mesh.broadcastLightConfig(l.groupId, cfg);
            mesh.broadcastGroupSync(*g);
            mqtt.publishState(cfg);
            break;
        }
    });
    mqtt.begin(Config::get());

    if (MDNS.begin(Config::get().deviceName))
        Logger::i("[mdns] http://%s.local", Config::get().deviceName);

    if (Config::get().otaEnabled) setupOta();
    mesh.begin();
    mesh.setOnPeerHeard([](){ channelMgr.onPeerHeard(); });

    // Wire SceneSyncManager → MeshManager
    sceneSync.setBroadcastFns(
        [](const char* id, uint32_t hash) { mesh.broadcastSceneForceSet(id, hash); },
        [](const char* id)                { mesh.broadcastSceneRequest(id); },
        [](const SceneChunkMsg& msg)      { mesh.broadcastSceneChunk(msg); },
        [](const SceneManifestMsg& msg)   { mesh.broadcastSceneManifest(msg); }
    );

    // ── Mesh callbacks ────────────────────────────────────────────────────────

    mesh.setOnLightConfig([](uint8_t groupId, const LightConfig& cfg) {
        GroupConfig* g = Config::group(groupId);
        if (!g) { Logger::w("[mesh] light config for unknown group %u — ignored", groupId); return; }
        g->light = cfg;
        Config::save();
        // Apply to all runners in this group
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (l.exists && l.groupId == groupId && _leds[i]) {
                _runners[i].applyConfig(cfg);
                mqtt.publishState(cfg);
            }
        }
    });

    mesh.setOnPresence([](const uint8_t* mac, const char*, bool isNew) {
        webServer.pushPeers();
        if (isNew) sceneSync.onNewPeer(mac);
    });

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

    mesh.setOnTriggerUpdate([]() { Updater::triggerAsync(); });
    mesh.setOnCheckUpdate([]() { Updater::checkAsync(); });

    mesh.setOnConfigChunk([](const uint8_t* srcMac, const ConfigChunkMsg* msg) {
        uint8_t own[6];
        WiFi.macAddress(own);
        if (memcmp(srcMac, own, 6) == 0) return;
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

    // Another device told this device (or a peer) to change a light's group
    mesh.setOnSetGroup([](const uint8_t* targetMac, uint8_t lightIndex, uint8_t groupId) {
        uint8_t own[6];
        WiFi.macAddress(own);
        if (memcmp(targetMac, own, 6) == 0) {
            if (lightIndex < MAX_LIGHTS && Config::group(groupId)) {
                Config::get().lights[lightIndex].groupId = groupId;
                Config::save();
                if (_leds[lightIndex]) {
                    _runners[lightIndex].setGroupId(groupId);
                    auto* g = Config::group(groupId);
                    if (g) _runners[lightIndex].applyConfig(g->light);
                }
                Logger::i("[mesh] light %u moved to group %u", lightIndex, groupId);
            }
        }
        mesh.peers.updateLightGroup(targetMac, lightIndex, groupId);
        webServer.pushPeers();
    });

    mesh.setOnGroupSync([](const GroupConfig& g) {
        bool lightUpdated = Config::applyGroupSync(g);
        Config::save();
        if (!g.exists) {
            // Group deleted — move any lights in it to Default
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                auto& l = Config::get().lights[i];
                if (l.exists && l.groupId == g.id) l.groupId = 0;
            }
            Config::save();
            applyAllLights();
        } else if (lightUpdated && g.exists) {
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                auto& l = Config::get().lights[i];
                if (l.exists && l.groupId == g.id && _leds[i])
                    _runners[i].applyConfig(g.light);
            }
        }
        webServer.pushGroups();
    });

    mesh.setOnPhaseSync([](uint8_t groupId, float phase) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (!l.exists || l.groupId != groupId) continue;
            GroupConfig* g = Config::group(l.groupId);
            if (!g || !g->syncEnabled) continue;
            _runners[i].snapPhase(phase);
        }
    });

    mesh.setGetPhase([](uint8_t lightIndex) -> float {
        return _runners[lightIndex].getPhase();
    });

    // ── Web server callbacks ──────────────────────────────────────────────────

    webServer.begin(
        // onGroupChange: re-apply all lights when group assignment changed via web
        []() { applyAllLights(); },

        [](uint8_t groupId, const LightConfig& cfg) {
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                auto& l = Config::get().lights[i];
                if (l.exists && l.groupId == groupId && _leds[i]) {
                    _runners[i].applyConfig(cfg);
                    mqtt.publishState(cfg);
                }
            }
            mesh.broadcastLightConfig(groupId, cfg);
            if (auto* g = Config::group(groupId)) mesh.broadcastGroupSync(*g);
        },

        [](const GroupConfig& g) { mesh.broadcastGroupSync(g); },

        [](const uint8_t* mac, uint8_t lightIndex, uint8_t groupId) {
            mesh.broadcastSetGroup(mac, lightIndex, groupId);
        },

        &mesh.peers,
        &sceneSync,

        [](const uint8_t* mac, bool enabled) { mesh.broadcastSetSceneSync(mac, enabled); },

        [](const char* id, const uint8_t* sourceMac) {
            if (sourceMac == nullptr) {
                sceneSync.resolveWithLocal(id);
            } else {
                sceneSync.setForcedAccept(id);
                mesh.broadcastSceneRequest(id);
            }
        },

        [](const uint8_t* targetMac, const char* json, size_t len) {
            mesh.sendConfigChunks(targetMac, json, len);
        },

        [](const uint8_t* mac) { mesh.broadcastTriggerUpdate(mac); },

        []() { channelMgr.beginSearch(); },

        [](const uint8_t* mac) { mesh.broadcastCheckUpdate(mac); }
    );

    auto notifySceneUpdated = [](const char* id) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++)
            if (_leds[i]) _runners[i].notifySceneUpdated(id);
    };
    webServer.setOnSceneSaved(notifySceneUpdated);
    sceneSync.setOnSceneSaved(notifySceneUpdated);

    Logger::i("[sys] ready");
    if (Config::get().checkUpdateOnStartup && WiFi.status() == WL_CONNECTED) Updater::checkAsync();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    if (Config::get().otaEnabled) ArduinoOTA.handle();
    webServer.loop();
    if (!_otaActive) {
        channelMgr.tick();
        mesh.tick();
        mqtt.loop();
        for (uint8_t i = 0; i < MAX_LIGHTS; i++)
            if (_leds[i]) _runners[i].tick();
        sceneSync.tick();
    }
}
