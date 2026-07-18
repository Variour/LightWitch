#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Wire.h>

#include <new>

#include "actions/ActionExecutor.h"
#include "battery/BatteryMonitor.h"
#include "buttons/ButtonManager.h"
#include "config/Config.h"
#include "led/Ws2801Driver.h"
#include "led/Ws2812bDriver.h"
#include "logging/Logger.h"
#include "mesh/ChannelManager.h"
#include "mesh/MeshManager.h"
#include "mesh/WifiElection.h"
#include "mqtt/MqttManager.h"
#include "patterns/PatternRunner.h"
#include "scenes/SceneSyncManager.h"
#include "sound/Es8311Driver.h"
#include "sound/PlaylistManager.h"
#include "sound/PlaylistSyncManager.h"
#include "storage/SdCardManager.h"
#include "timesync/TimeSync.h"
#include "version.h"
#include "web/WebServer.h"

static Ws2812bDriver _ws2812bPool[MAX_LIGHTS];
static Ws2801Driver _ws2801Pool[MAX_LIGHTS];
static LedDriver* _leds[MAX_LIGHTS] = {};
static PatternRunner _runners[MAX_LIGHTS];
static Es8311Driver _sound;
static SdCardManager _sdCard;
static MeshManager mesh;
static ChannelManager channelMgr;
static WifiElection wifiElection;
static BatteryWebServer webServer;
static MqttManager mqtt;
static SceneSyncManager sceneSync;
static PlaylistSyncManager playlistSync;
static ActionExecutor actionExecutor;
static ButtonManager buttonManager;
static BatteryMonitor battery;
static bool _otaActive = false;

// Last-published battery reading, so mqtt/mesh-telemetry pushes only fire on
// an actual change instead of every slow tick.
static bool _lastBatteryPresent = false;
static uint8_t _lastBatteryPercent = 0xFF;
static BatteryMonitor::State _lastBatteryState = BatteryMonitor::State::OnBattery;

// Caps how often patterns recompute and push to the LED driver. Well above what
// any strip or human eye needs, but far below the unthrottled main-loop rate —
// getPhase()/snapPhase() are time-based, so skipping ticks doesn't affect them.
static constexpr uint32_t PATTERN_TICK_INTERVAL_MS = 1000 / 60;
static uint32_t _lastPatternTickMs = 0;

// Caps how often the background subsystems below (OTA polling, web server
// housekeeping, mesh, wifi election, time sync, MQTT, buttons, scene sync)
// are ticked. Unthrottled, loop() spins as fast as the CPU allows and calls
// all of them — GPIO reads, socket polls, WiFi.status() — on every single
// pass. 5 ms (200 Hz) comfortably oversamples the tightest consumers: button
// debounce (30 ms, see ButtonManager::DEBOUNCE_MS) and scene-sync's chunk
// pacing (20 ms, see SYNC_CHUNK_SEND_INTERVAL_MS); every other subsystem's
// own internal timers run in the hundreds/thousands of ms. A live OTA
// transfer bypasses this for ArduinoOTA.handle() so flashing stays full-speed.
static constexpr uint32_t BACKGROUND_TICK_INTERVAL_MS = 5;
static uint32_t _lastBackgroundTickMs = 0;

// Second, slower tier for the subsystems whose own timers are all in the
// hundreds-of-ms-to-seconds range: mesh's tightest is a 500 ms proximity
// ping, wifi election's tightest is a 100 ms pre-connect delay, channel scan
// dwells 7-10 s, MQTT's keepalive is 30 s. 20 Hz oversamples all of them with
// room to spare. Buttons and scene sync stay on the faster tier above since
// their own timers (30 ms debounce, 20 ms chunk pacing) can't tolerate this.
static constexpr uint32_t SLOW_TICK_INTERVAL_MS = 50;
static uint32_t _lastSlowTickMs = 0;

// Tracks the last-rendered firmware-update status so the progress fill is
// only redrawn when it actually changes, not on every loop() iteration.
static Updater::State _lastUpdateState = Updater::State::Idle;
static int _lastUpdateProgress = -1;
static bool _startupUpdateCheckPending = false;

// Reassembly buffer for incoming config push chunks
static String _cfgSyncBuf;
static uint16_t _cfgSyncExpected = 0;

static void serialSink(LogLevel level, const char* msg) {
    const char* p = level == LogLevel::ERROR     ? "[E]"
                    : level == LogLevel::WARN    ? "[W]"
                    : level == LogLevel::INFO    ? "[I]"
                    : level == LogLevel::VERBOSE ? "[V]"
                                                 : "[D]";
    Serial.printf("%s %s\n", p, msg);
}

// ── Light / group helpers ─────────────────────────────────────────────────────

// Substitutes a light's own brightness override (local to this device, not
// mesh-synced) for its group's brightness, when enabled. Every path that
// applies a group's LightConfig to a light's runner must go through this so
// the override survives group brightness changes, mesh syncs, and reboots.
static LightConfig withBrightnessOverride(const LightConfig& cfg, const LightHardwareConfig& l) {
    if (!l.brightnessOverrideEnabled) return cfg;
    LightConfig c = cfg;
    c.brightness = l.brightnessOverride;
    return c;
}

// Apply each light's group config to its runner
static void applyAllLights() {
    Config::forEachLight([](uint8_t i, LightHardwareConfig& l) {
        if (!_leds[i]) return;
        auto* g = Config::group(l.groupId);
        if (g) _runners[i].applyConfig(withBrightnessOverride(g->light, l));
    });
}

// Pushes mesh peers over the dashboard WebSocket.
static void publishTelemetry() { webServer.pushPeers(); }

// Broadcasts a GroupConfig change over mesh and republishes its MQTT state —
// or, for a tombstone (g.exists == false, i.e. this group was just deleted),
// clears its retained MQTT topics instead of trying to publish content for
// a group Config no longer has.
static void publishGroupSync(const GroupConfig& g) {
    mesh.broadcastGroupSync(g);
    if (g.exists)
        mqtt.publishGroupState(g.id);
    else
        mqtt.clearGroupRetained(g.id);
}

// Re-applies a single light's effective config — its (possibly just
// reassigned) group's config, with its own brightness override layered on
// top if enabled — to its runner and pushes state to the dashboard. Used
// after a light's own brightnessOverride(Enabled) or groupId changes via
// MQTT; group config itself is unaffected, so this doesn't go through
// applyAndPropagateLightConfig/mesh broadcast.
static void applyLightBrightnessOverride(uint8_t lightIndex) {
    if (lightIndex >= MAX_LIGHTS || !_leds[lightIndex]) return;
    auto& l = Config::get().lights[lightIndex];
    GroupConfig* g = Config::group(l.groupId);
    if (!g) return;
    _runners[lightIndex].applyConfig(withBrightnessOverride(g->light, l));
    mqtt.publishLightOverride(lightIndex);
    publishTelemetry();
}

// Applies a new LightConfig to a group and propagates it everywhere: local
// runners, Config save, mesh broadcast, MQTT state. Shared by every trigger
// source (MQTT command, web API, inbound mesh sync) so the sequence — and the
// seq-based staleness check — stays consistent regardless of where the change
// originated.
//
// bumpRevision must be true for genuinely local-origin edits (MQTT/web/button)
// so the edit can win GroupConfig's mesh-wide revision merge (see
// Config::applyGroupSync) on other peers. It must be false for edits arriving
// via the mesh's own LightConfig relay (mesh.setOnLightConfig below) — those
// are just re-applying a peer's already-attributed edit, and bumping here
// would incorrectly re-attribute it to this device and inflate the revision
// on every relay hop.
static void applyAndPropagateLightConfig(uint8_t groupId, const LightConfig& cfg,
                                         bool bumpRevision) {
    GroupConfig* g = Config::group(groupId);
    if (!g) {
        Logger::w("[cfg] light config for unknown group %u — ignored", groupId);
        return;
    }
    if (cfg.seq < g->light.seq) return;  // stale relative to what we already have

    g->light = cfg;
    if (bumpRevision) Config::bumpGroupRevision(*g);
    Config::save();

    Config::forEachLight([&](uint8_t i, LightHardwareConfig& l) {
        if (l.groupId == groupId && _leds[i])
            _runners[i].applyConfig(withBrightnessOverride(cfg, l));
    });

    mesh.broadcastLightConfig(groupId, cfg);
    publishGroupSync(*g);
}

static bool hasWifiPolicyOrigin() {
    for (uint8_t b : Config::get().wifiPolicyOriginMac)
        if (b != 0) return true;
    return false;
}

static MeshManager::MeshPolicyState currentWifiPolicyState() {
    MeshManager::MeshPolicyState state;
    state.singleClientMode = Config::get().wifiSingleClientMode;
    state.revision = Config::get().wifiPolicyRevision;
    memcpy(state.originMac, Config::get().wifiPolicyOriginMac, sizeof(state.originMac));
    return state;
}

static void ensureWifiPolicyStateInitialized() {
    if (hasWifiPolicyOrigin()) return;
    WiFi.macAddress(Config::get().wifiPolicyOriginMac);
    Config::save();
}

static bool applyWifiPolicyState(const MeshManager::MeshPolicyState& state, const char* via) {
    auto& c = Config::get();
    bool policyChanged = c.wifiSingleClientMode != state.singleClientMode;
    bool metaChanged =
        c.wifiPolicyRevision != state.revision ||
        memcmp(c.wifiPolicyOriginMac, state.originMac, sizeof(c.wifiPolicyOriginMac)) != 0;
    if (!policyChanged && !metaChanged) return false;

    c.wifiSingleClientMode = state.singleClientMode;
    c.wifiPolicyRevision = state.revision;
    memcpy(c.wifiPolicyOriginMac, state.originMac, sizeof(c.wifiPolicyOriginMac));
    Config::save();

    Logger::i("[wifi] single-client policy synced via %s: enabled=%d rev=%lu", via,
              state.singleClientMode, (unsigned long)state.revision);
    if (policyChanged) {
        wifiElection.onPolicyChanged(state.singleClientMode);
        publishTelemetry();
    }
    return true;
}

static void setLocalWifiPolicy(bool enabled) {
    if (Config::get().wifiSingleClientMode == enabled) return;
    MeshManager::MeshPolicyState state = currentWifiPolicyState();
    state.singleClientMode = enabled;
    state.revision++;
    WiFi.macAddress(state.originMac);
    applyWifiPolicyState(state, "local");
    mesh.broadcastMeshPolicy(state);
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
static void setupWifi() {
    auto& c = Config::get();

    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);
    ensureWifiPolicyStateInitialized();

    Config::loadWifi();
    uint8_t count = Config::wifiCount();

    if (count == 0) {
        WiFi.softAP(c.deviceName, c.apPassword, 1);
        WiFi.setTxPower(WIFI_TX_POWER);
        Logger::i("[wifi] No networks configured, AP: %s  IP: %s", c.deviceName,
                  WiFi.softAPIP().toString().c_str());
        return;
    }

    // Single-client mode: don't block here trying to join a network — the
    // mesh isn't even up yet, so we have no way to know if a lower-MAC peer
    // should get first shot at the connection. Bring up the local AP for
    // reachability and let WifiElection (ticked once mesh.begin() has run)
    // decide non-blockingly whether/when this device should actually connect.
    if (c.wifiSingleClientMode) {
        WiFi.softAP(c.deviceName, c.apPassword, 1);
        WiFi.setTxPower(WIFI_TX_POWER);
        Logger::i(
            "[wifi] single-client mode: deferring connect decision to WifiElection, AP: %s  IP: %s",
            c.deviceName, WiFi.softAPIP().toString().c_str());
        return;
    }

    uint8_t last = Config::wifiLast();

    // Strict list order, first to last — no "last known good" stickiness.
    // A fleet configured with the same priority order is then more likely to
    // try the same network first and converge on the same channel (#323).
    for (uint8_t ni = 0; ni < count; ni++) {
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
                Logger::i("[wifi] Connected to %s, IP: %s  (AP off)", ssid,
                          WiFi.localIP().toString().c_str());
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
    ArduinoOTA.onEnd([]() {
        _otaActive = false;
        Logger::i("[ota] done");
    });
    ArduinoOTA.onError(
        [](ota_error_t e) { Logger::e("[ota] error %u: %s", e, Update.errorString()); });
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
        while (f) {
            Logger::i("[fs] %s (%u bytes)", f.name(), f.size());
            f = root.openNextFile();
        }
    }
    Config::load();
    Logger::setLevel((LogLevel)Config::get().logLevel);
    Logger::i("[sys] firmware %s  device: %s", FW_VERSION, Config::get().deviceName);

    // Auto-probe the onboard SD card reader, if this build targets a board
    // that has one (see SdCardManager) — no-op/harmless on boards without it.
    _sdCard.begin();

    battery.begin(Config::get().batteryMonitoringEnabled);

    setupWifi();
    TimeSync::begin(Config::get().timezone);
    channelMgr.begin(&mesh.peers);

    // Initialise one driver + runner per configured light
    Config::forEachLight([](uint8_t i, LightHardwareConfig& l) {
        uint16_t numLeds = (uint16_t)l.width * l.height;
        if (numLeds == 0) numLeds = 1;
        LedDriver* drv;
        if (l.ledType == LedType::WS2801) {
            _ws2801Pool[i].setup(l.dataPin, l.clockPin, numLeds, l.colorOrder);
            _ws2801Pool[i].begin();
            drv = &_ws2801Pool[i];
            Logger::i("[led] light %u: WS2801 data=GPIO%d clock=GPIO%d leds=%u group=%u", i,
                      l.dataPin, l.clockPin, numLeds, l.groupId);
        } else {
            _ws2812bPool[i].setup(l.dataPin, numLeds, l.colorOrder);
            _ws2812bPool[i].begin();
            drv = &_ws2812bPool[i];
            Logger::i("[led] light %u: WS2812B data=GPIO%d leds=%u group=%u", i, l.dataPin, numLeds,
                      l.groupId);
        }
        _leds[i] = drv;
        _runners[i].begin(*drv);
        _runners[i].setDimensions(l.width, l.height);
        _runners[i].setMatrixLayout(l.matrixStart, l.matrixDir, l.matrixSerpentine);
        _runners[i].setWrap(l.wrapWidth, l.wrapHeight);
        _runners[i].setPeerRegistry(&mesh.peers);
        _runners[i].setGroupId(l.groupId);
        auto* g = Config::group(l.groupId);
        if (g) _runners[i].applyConfig(withBrightnessOverride(g->light, l));
    });

    // Bring up the device-wide I2C bus, if configured — shared by the sound
    // codec's control interface and any TCA9555-backed button (see
    // DeviceConfig::i2cSdaPin/i2cSclPin). Centralized here (rather than each
    // consumer calling Wire.begin() itself) since ESP32 has a single default
    // I2C bus that every consumer must agree on the same pins for.
    if (Config::get().i2cSdaPin != PIN_UNUSED && Config::get().i2cSclPin != PIN_UNUSED) {
        Wire.begin(Config::get().i2cSdaPin, Config::get().i2cSclPin);
        Logger::i("[sys] I2C bus: sda=GPIO%d scl=GPIO%d", Config::get().i2cSdaPin,
                  Config::get().i2cSclPin);
    }

    // Initialise the sound output, if configured.
    Config::forEachSound([](uint8_t, SoundHardwareConfig& s) {
        _sound.setup(s, Config::get().i2cSdaPin, Config::get().i2cSclPin,
                     Config::get().expanderAddress);
        _sound.begin();
        _sound.setSdCard(&_sdCard);
    });

    // Wire MQTT: every group and light gets its own topic (see MqttManager.h) —
    // funnels into the same apply/propagate + mesh-broadcast paths web/buttons use.
    mqtt.setOnGroupLight([](uint8_t groupId, const LightConfig& cfg) {
        applyAndPropagateLightConfig(groupId, cfg, /*bumpRevision=*/true);
    });
    mqtt.setOnGroupSyncToggle([](const GroupConfig& g) { publishGroupSync(g); });
    mqtt.setOnLightOverride([](uint8_t lightIndex) { applyLightBrightnessOverride(lightIndex); });
    mqtt.setOnSceneSyncEnabled([]() { sceneSync.onSyncEnabled(); });
    mqtt.setBatteryStatusProvider([]() { return battery.status(); });
    mqtt.begin(Config::get());

    // Wire the action layer: buttons (and, perspectively, other future trigger
    // sources) execute actions through this, which funnels into the same
    // apply/propagate + mesh-broadcast paths as web/MQTT/mesh already use.
    actionExecutor.setApplyFn([](uint8_t groupId, const LightConfig& cfg) {
        applyAndPropagateLightConfig(groupId, cfg, /*bumpRevision=*/true);
    });
    actionExecutor.setBroadcastGroupSyncFn([](const GroupConfig& g) { publishGroupSync(g); });
    actionExecutor.setApplyLightBrightnessFn(
        [](uint8_t lightIndex) { applyLightBrightnessOverride(lightIndex); });
    buttonManager.setExecutor(&actionExecutor);
    buttonManager.begin();

    if (MDNS.begin(Config::get().deviceName))
        Logger::i("[mdns] http://%s.local", Config::get().deviceName);

    if (Config::get().otaEnabled) setupOta();
    mesh.begin();
    wifiElection.begin(&mesh.peers);
    mesh.setWifiAttemptingProvider([]() { return wifiElection.isAttempting(); });
    mesh.setWifiConnectedProvider([]() { return wifiElection.isAdvertisableConnected(); });
    wifiElection.setOnAttemptingChanged([]() { publishTelemetry(); });
    mesh.setOnPeerHeard([](const uint8_t* mac) { channelMgr.onPeerHeard(mac); });
    mesh.setOnMeshPolicy(
        [](const MeshManager::MeshPolicyState& state) { applyWifiPolicyState(state, "mesh"); });
    mesh.setOnWifiRetry([]() { wifiElection.retryNow(); });
    mesh.setOnMeshSearch([]() { channelMgr.beginSearch(); });
    mesh.setBatteryStatusProvider([]() { return battery.status(); });

    // Wire SceneSyncManager → MeshManager
    sceneSync.setBroadcastFns(
        [](const char* id, uint32_t hash) { mesh.broadcastSceneForceSet(id, hash); },
        [](const char* id) { mesh.broadcastSceneRequest(id); },
        [](const SceneChunkMsg& msg) { mesh.broadcastSceneChunk(msg); },
        [](const SceneManifestMsg& msg) { mesh.broadcastSceneManifest(msg); });
    sceneSync.setEditPushFn(
        [](const char* id, uint32_t prevHash) { mesh.broadcastSceneEditPush(id, prevHash); });
    sceneSync.setRequestManifestFn([]() { mesh.broadcastRequestManifest(); });

    // Wire PlaylistSyncManager → MeshManager (mirrors SceneSyncManager above)
    playlistSync.setBroadcastFns(
        [](const char* id, uint32_t hash) { mesh.broadcastPlaylistForceSet(id, hash); },
        [](const char* id) { mesh.broadcastPlaylistRequest(id); },
        [](const PlaylistChunkMsg& msg) { mesh.broadcastPlaylistChunk(msg); },
        [](const PlaylistManifestMsg& msg) { mesh.broadcastPlaylistManifest(msg); });
    playlistSync.setEditPushFn(
        [](const char* id, uint32_t prevHash) { mesh.broadcastPlaylistEditPush(id, prevHash); });
    playlistSync.setRequestManifestFn([]() { mesh.broadcastRequestPlaylistManifest(); });

    // ── Mesh callbacks ────────────────────────────────────────────────────────

    mesh.setOnLightConfig([](uint8_t groupId, const LightConfig& cfg) {
        // Relaying a peer's already-attributed edit — don't bump, see
        // applyAndPropagateLightConfig's comment on bumpRevision.
        applyAndPropagateLightConfig(groupId, cfg, /*bumpRevision=*/false);
    });

    mesh.setOnPresence([](const uint8_t* mac, const char*, bool isNew) {
        publishTelemetry();
        if (isNew) {
            sceneSync.onNewPeer(mac);
            playlistSync.onNewPeer(mac);
        }
    });

    mesh.setOnSceneManifest(
        [](const uint8_t* mac, const SceneManifestMsg* msg) { sceneSync.onManifest(mac, msg); });
    mesh.setOnSceneRequest(
        [](const uint8_t* mac, const char* id) { sceneSync.onRequest(mac, id); });
    mesh.setOnSceneChunk([](const SceneChunkMsg* msg) { sceneSync.onChunk(msg); });
    mesh.setOnSceneForceSet([](const char* id, uint32_t hash) { sceneSync.onForceSet(id, hash); });
    mesh.setOnSceneEditPush([](const uint8_t* mac, const char* id, uint32_t prevHash) {
        sceneSync.onSceneEditPush(mac, id, prevHash);
    });
    mesh.setOnRequestManifest([]() { sceneSync.onRequestManifest(); });
    mesh.setOnSetSceneSync([](bool enabled) { sceneSync.onSetSceneSync(enabled); });

    mesh.setOnPlaylistManifest([](const uint8_t* mac, const PlaylistManifestMsg* msg) {
        playlistSync.onManifest(mac, msg);
    });
    mesh.setOnPlaylistRequest(
        [](const uint8_t* mac, const char* id) { playlistSync.onRequest(mac, id); });
    mesh.setOnPlaylistChunk([](const PlaylistChunkMsg* msg) { playlistSync.onChunk(msg); });
    mesh.setOnPlaylistForceSet(
        [](const char* id, uint32_t hash) { playlistSync.onForceSet(id, hash); });
    mesh.setOnPlaylistEditPush([](const uint8_t* mac, const char* id, uint32_t prevHash) {
        playlistSync.onPlaylistEditPush(mac, id, prevHash);
    });
    mesh.setOnRequestPlaylistManifest([]() { playlistSync.onRequestManifest(); });
    mesh.setOnSetPlaylistSync([](bool enabled) { playlistSync.onSetPlaylistSync(enabled); });

    mesh.setOnAudioGroupSync([](const AudioGroupConfig& g) {
        bool didApply = Config::applyAudioGroupSync(g);
        AudioGroupConfig* applied = Config::audioGroup(g.id);
        Config::save();
        if (!applied) {
            // Group deleted — move this device's sound output (if it was a member)
            // back to Default, mirroring how light-group deletion moves lights.
            Config::forEachSound([&](uint8_t, SoundHardwareConfig& s) {
                if (s.audioGroupId == g.id) s.audioGroupId = 0;
            });
            Config::save();
        }
        if (didApply || !applied) webServer.pushAudioGroups();
    });

    mesh.setOnTriggerUpdate(
        []() { wifiElection.requestTemporary([]() { Updater::triggerAsync(); }); });
    mesh.setOnCheckUpdate([]() { wifiElection.requestTemporary([]() { Updater::checkAsync(); }); });
    mesh.setOnTimeSync([](uint32_t epoch) { TimeSync::onPeerTime(epoch); });
    TimeSync::setBroadcastFn([](uint32_t epoch) { mesh.broadcastTimeSync(epoch); });

    mesh.setOnConfigChunk([](const uint8_t* srcMac, const ConfigChunkMsg* msg) {
        uint8_t own[6];
        WiFi.macAddress(own);
        if (memcmp(srcMac, own, 6) == 0) return;
        if (memcmp(msg->targetMac, own, 6) != 0) return;
        if (msg->chunkIndex == 0) {
            _cfgSyncBuf = "";
            _cfgSyncExpected = msg->totalChunks;
        }
        if (msg->totalChunks != _cfgSyncExpected) return;
        _cfgSyncBuf.concat((const char*)msg->data, msg->dataLen);
        if (msg->chunkIndex == _cfgSyncExpected - 1) {
            size_t cipherLen = _cfgSyncBuf.length();
            uint8_t* plain = new (std::nothrow) uint8_t[cipherLen];
            size_t plainLen = 0;
            if (!plain) {
                Logger::e("[cfg] config sync: out of memory decrypting %u bytes",
                          (unsigned)cipherLen);
            } else if (mesh.decryptConfigFromPeer(srcMac, (const uint8_t*)_cfgSyncBuf.c_str(),
                                                  cipherLen, plain, plainLen)) {
                Logger::i("[cfg] config sync received (%u bytes), applying", (unsigned)plainLen);
                Config::applyConfigSync((const char*)plain, plainLen);
            } else {
                Logger::e(
                    "[cfg] config sync decrypt failed (no session key or corrupt data), dropping");
            }
            delete[] plain;
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
                    if (g)
                        _runners[lightIndex].applyConfig(
                            withBrightnessOverride(g->light, Config::get().lights[lightIndex]));
                }
                Logger::i("[mesh] light %u moved to group %u", lightIndex, groupId);
            }
        }
        mesh.peers.updateLightGroup(targetMac, lightIndex, groupId);
        publishTelemetry();
    });

    mesh.setOnGroupSync([](const GroupConfig& g) {
        bool didApply = Config::applyGroupSync(g);
        // Re-check the actual local state after the merge: a stale tombstone
        // can lose the revision race (group still exists locally), and a
        // stale recreate can equally lose to an already-applied delete.
        GroupConfig* applied = Config::group(g.id);
        Config::save();
        if (!applied) {
            // Group deleted — move any lights in it to Default
            Config::forEachLight([&](uint8_t, LightHardwareConfig& l) {
                if (l.groupId == g.id) l.groupId = 0;
            });
            Config::save();
            applyAllLights();
            mqtt.clearGroupRetained(g.id);
        } else if (didApply) {
            // Merge was adopted — light may or may not actually differ, but
            // reapplying is cheap and name/exists/light always move together
            // now, so there's no cheaper way to tell them apart here.
            Config::forEachLight([&](uint8_t i, LightHardwareConfig& l) {
                if (l.groupId == g.id && _leds[i])
                    _runners[i].applyConfig(withBrightnessOverride(applied->light, l));
            });
        }
        if (applied) mqtt.publishGroupState(applied->id);
        webServer.pushGroups();
    });

    mesh.setOnPhaseSync([](uint8_t groupId, float phase) {
        Config::forEachLight([&](uint8_t i, LightHardwareConfig& l) {
            if (l.groupId != groupId) return;
            GroupConfig* g = Config::group(l.groupId);
            if (!g || !g->syncEnabled) return;
            _runners[i].snapPhase(phase);
        });
    });

    mesh.setGetPhase([](uint8_t lightIndex) -> float { return _runners[lightIndex].getPhase(); });

    // ── Web server callbacks ──────────────────────────────────────────────────

    webServer.begin(
        // onGroupChange: re-apply all lights when group assignment changed via web
        []() { applyAllLights(); },

        [](uint8_t groupId, const LightConfig& cfg) {
            applyAndPropagateLightConfig(groupId, cfg, /*bumpRevision=*/true);
        },

        [](const GroupConfig& g) { publishGroupSync(g); },

        [](const uint8_t* mac, uint8_t lightIndex, uint8_t groupId) {
            mesh.broadcastSetGroup(mac, lightIndex, groupId);
        },

        &mesh.peers, &sceneSync,

        [](const uint8_t* mac, bool enabled) { mesh.broadcastSetSceneSync(mac, enabled); },

        [](const char* id, const uint8_t* sourceMac) {
            if (sourceMac == nullptr) {
                sceneSync.resolveWithLocal(id);
            } else {
                sceneSync.setForcedAccept(id);
                mesh.broadcastSceneRequest(id);
            }
        },

        [](const uint8_t* targetMac, const char* json, size_t) {
            static const uint8_t kAllZeros[6] = {};
            if (memcmp(targetMac, kAllZeros, 6) == 0)
                mesh.pushConfigSecureToAll(json);
            else
                mesh.pushConfigSecure(targetMac, json);
        },

        [](const uint8_t* mac) { mesh.broadcastTriggerUpdate(mac); },

        []() {
            // Broadcast first, while still on the current channel -- beginSearch()
            // retunes the radio immediately, and once that's happened this
            // device is no longer on the channel its own former island-mates
            // are still listening on.
            mesh.broadcastMeshSearch();
            channelMgr.beginSearch();
        },

        [](const uint8_t* mac) { mesh.broadcastCheckUpdate(mac); },

        [](std::function<void()> onReady) { wifiElection.requestTemporary(onReady); },

        [](bool enabled) { setLocalWifiPolicy(enabled); },

        []() { return wifiElection.isAttempting(); },

        []() {
            wifiElection.retryNow();
            mesh.broadcastWifiRetry();
        },

        &channelMgr, &_sdCard);

    auto notifySceneUpdated = [](const char* id) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++)
            if (_leds[i]) _runners[i].notifySceneUpdated(id);
        // A save can also be a rename, which changes the scene's display name
        // in every group's MQTT effect_list — resync discovery to match.
        mqtt.resyncGroupDiscovery();
    };
    webServer.setOnSceneSaved(notifySceneUpdated);
    webServer.setOnGroupsChanged([]() { mqtt.resyncGroupDiscovery(); });
    webServer.setOnSceneListChanged([]() { mqtt.resyncGroupDiscovery(); });
    sceneSync.setOnSceneListChanged([]() { mqtt.resyncGroupDiscovery(); });
    webServer.setOnClearMqtt([]() { mqtt.clearRetainedAndDisable(); });
    webServer.setOnSceneSyncChanged([]() { mqtt.publishSceneSyncState(); });

    webServer.setOnTestLight([](uint8_t idx) {
        if (idx < MAX_LIGHTS && _leds[idx]) _runners[idx].showTest(5000);
    });
    webServer.setOnTestColorOrder([](uint8_t idx) {
        if (idx < MAX_LIGHTS && _leds[idx]) _runners[idx].showColorOrderTest();
    });
    webServer.setOnOrientationChange([](uint8_t idx) {
        if (idx < MAX_LIGHTS && _leds[idx]) {
            auto& l = Config::get().lights[idx];
            _runners[idx].setMatrixLayout(l.matrixStart, l.matrixDir, l.matrixSerpentine);
            _runners[idx].setWrap(l.wrapWidth, l.wrapHeight);
        }
    });
    webServer.setOnLightBrightnessChange([](uint8_t idx) { applyLightBrightnessOverride(idx); });
    webServer.setOnColorOrderChange([](uint8_t idx) {
        if (idx < MAX_LIGHTS && _leds[idx])
            _leds[idx]->setColorOrder(Config::get().lights[idx].colorOrder);
    });
    webServer.setOnButtonsChanged([]() { buttonManager.reconfigure(); });
    webServer.setBatteryStatusProvider([]() { return battery.status(); });
    webServer.setOnMqttReconfigure([]() { mqtt.reconfigure(Config::get()); });
    webServer.setOnBatteryMonitoringChanged([](bool enabled) {
        battery.setEnabled(enabled);
        mqtt.resyncGroupDiscovery();
        mqtt.publishBatteryState();
    });
    // AP-mode SSID/password only take effect while this device is actually
    // running its own AP (no WiFi networks configured, or all failed) — the
    // many other WiFi.softAP() call sites already gate the same way (see
    // setupWifi()/WifiElection.h). If it's currently connected as a station,
    // the new password takes effect the next time it falls back to AP.
    webServer.setOnApPasswordChanged([]() {
        if (!WiFi.isConnected()) {
            auto& c = Config::get();
            WiFi.softAP(c.deviceName, c.apPassword, 1);
            Logger::i("[wifi] AP password updated live");
        }
    });
    webServer.setOnTimezoneChanged([](const char* tz) { TimeSync::begin(tz); });
    webServer.setOnWifiConnectForConfirm([](std::function<void(bool)> onDone) {
        wifiElection.connectForConfirm([onDone](bool ok) {
            if (onDone) onDone(ok);
            publishTelemetry();
        });
    });
    webServer.setOnConfirmApDisable([]() {
        wifiElection.confirmApDisable();
        publishTelemetry();
    });
    webServer.setWifiAwaitingApConfirmProvider([]() { return wifiElection.awaitingApConfirm(); });
    webServer.setOnTestSound([](uint8_t idx) {
        if (idx < MAX_SOUNDS && Config::get().sounds[idx].exists) _sound.playTestMelody();
    });
    sceneSync.setOnSceneSaved(notifySceneUpdated);

    Logger::i("[sys] ready");
    if (Config::get().checkUpdateOnStartup) {
        if (Config::get().wifiSingleClientMode) {
            _startupUpdateCheckPending = true;
            Logger::i("[upd] startup check armed; waiting for WiFi election");
        } else if (WiFi.status() == WL_CONNECTED) {
            Updater::checkAsync();
        }
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    bool backgroundTick = now - _lastBackgroundTickMs >= BACKGROUND_TICK_INTERVAL_MS;
    if (backgroundTick) _lastBackgroundTickMs = now;
    bool slowTick = now - _lastSlowTickMs >= SLOW_TICK_INTERVAL_MS;
    if (slowTick) _lastSlowTickMs = now;

    // Idle OTA polling is throttled with everything else; once a transfer is
    // actually in flight (_otaActive), run every pass so flashing isn't slowed.
    if (Config::get().otaEnabled && (slowTick || _otaActive)) ArduinoOTA.handle();
    if (slowTick) webServer.loop();

    Updater::State updState = Updater::status().state;

    // Hand back a temporary WiFi connection once the OTA op that needed it
    // has actually settled (Idle or Error) — not just once it connected,
    // since Updater's checks/applies run asynchronously on their own task
    // and need the radio for their whole duration. Must run before the
    // Downloading/Done early-return below, which skips wifiElection.tick()
    // (and everything else) entirely while a download is in flight.
    if (updState == Updater::State::Idle || updState == Updater::State::Error)
        wifiElection.releaseTemporary();

    if (updState == Updater::State::Downloading || updState == Updater::State::Done) {
        int progress = Updater::status().progress;
        if (updState != _lastUpdateState || progress != _lastUpdateProgress) {
            _lastUpdateState = updState;
            _lastUpdateProgress = progress;
            mqtt.publishUpdateState();
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                if (!_leds[i]) continue;
                if (updState == Updater::State::Done)
                    _runners[i].showUpdateDone();
                else
                    _runners[i].showUpdateProgress((uint8_t)progress);
            }
        }
        return;
    }
    if (updState != _lastUpdateState) mqtt.publishUpdateState();
    _lastUpdateState = updState;

    if (!_otaActive && slowTick) {
        channelMgr.tick();
        mesh.tick();
        wifiElection.tick();
        if (_startupUpdateCheckPending && wifiElection.isAdvertisableConnected()) {
            _startupUpdateCheckPending = false;
            Logger::i("[upd] WiFi election connected; running deferred startup check");
            Updater::checkAsync();
        }
        TimeSync::tick();
        mqtt.loop();

        battery.tick();
        const auto& bs = battery.status();
        if (bs.present != _lastBatteryPresent || bs.percent != _lastBatteryPercent ||
            bs.state != _lastBatteryState) {
            _lastBatteryPresent = bs.present;
            _lastBatteryPercent = bs.percent;
            _lastBatteryState = bs.state;
            mqtt.publishBatteryState();
            publishTelemetry();
        }
    }
    if (!_otaActive && backgroundTick) {
        buttonManager.tick();
        sceneSync.tick();
        playlistSync.tick();
    }

    if (!_otaActive && now - _lastPatternTickMs >= PATTERN_TICK_INTERVAL_MS) {
        _lastPatternTickMs = now;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++)
            if (_leds[i]) _runners[i].tick();
    }
}
