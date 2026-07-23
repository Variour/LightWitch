#pragma once
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include <functional>
#include <vector>

#include "../battery/BatteryMonitor.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../mesh/ChannelManager.h"
#include "../mesh/PeerRegistry.h"
#include "../scenes/SceneManager.h"
#include "../scenes/SceneSyncManager.h"
#include "../storage/SdCardManager.h"
#include "../update/Updater.h"
#include "../version.h"
#include "ApiTypes.h"

// Called when this device's own group changes
using GroupChangeCb = std::function<void()>;
// Called when a group's light config changes (groupId, config)
using GroupLightCb = std::function<void(uint8_t, const LightConfig&)>;
// Called when a group is created/updated/deleted (the full GroupConfig)
using GroupSyncCb = std::function<void(const GroupConfig&)>;
// Called when we want to move a specific light on a remote peer to a group
using SetRemoteGroupCb =
    std::function<void(const uint8_t* mac, uint8_t lightIndex, uint8_t groupId)>;
// Called when we want to toggle sceneSyncEnabled on a remote peer
using SetRemoteSyncCb = std::function<void(const uint8_t* mac, bool enabled)>;
// Called when a conflict is resolved (id, sourceMac — null means local copy wins)
using ResolveConflictCb = std::function<void(const char* id, const uint8_t* sourceMac)>;
// Called to push syncable config to peers via mesh (targetMac all-zeros = all, json payload)
using PushConfigCb = std::function<void(const uint8_t* targetMac, const char* json, size_t len)>;
// Called to broadcast a firmware update trigger to a specific peer
using TriggerPeerUpdateCb = std::function<void(const uint8_t* mac)>;
// Called to broadcast a firmware update check to a specific peer (no auto-install)
using CheckPeerUpdateCb = std::function<void(const uint8_t* mac)>;
// Called to trigger a manual channel re-search
using MeshSearchCb = std::function<void()>;
// Called after a scene file is successfully written (locally or via mesh)
using SceneSavedCb = std::function<void(const char* sceneId)>;
// Called to run a test pattern on a specific light index
using TestLightCb = std::function<void(uint8_t)>;
using TestColorOrderCb = std::function<void(uint8_t)>;
// Called to play the hardware-verification test melody on a specific sound output index
using TestSoundCb = std::function<void(uint8_t)>;
// Called when matrix orientation (matrixStart/matrixDir) or wrap topology changes without reboot
using OrientationChangeCb = std::function<void(uint8_t)>;
using ColorOrderChangeCb = std::function<void(uint8_t)>;
// Called when a light's own brightnessOverride(Enabled) changes without reboot
using LightBrightnessChangeCb = std::function<void(uint8_t)>;
// Called after a button is added/updated/deleted, so GPIO pin modes can be re-applied live
using ButtonsChangedCb = std::function<void()>;
// Called whenever the group list changes (create/rename/delete, local or mesh-synced) —
// fired from the same point as the groups WebSocket push, so MQTT discovery can resync too.
using GroupsChangedCb = std::function<void()>;
// Called after a scene is created or deleted (not edited — see SceneSavedCb for that),
// so MQTT group discovery's scene-derived effect list can be rebuilt.
using SceneListChangedCb = std::function<void()>;
// Called before the MQTT broker config is wiped, so retained messages
// (state, discovery, telemetry) can be cleared from the broker first.
using ClearMqttCb = std::function<void()>;
// Called after this device's own sceneSyncEnabled changes via REST, so
// MQTT's retained scenesync state topic doesn't go stale.
using SceneSyncChangedCb = std::function<void()>;
// Called before a local OTA action so this device can connect to WiFi first if it's
// currently on single-WiFi-client standby; invokes the given callback once ready
// (immediately, if already connected).
using RequestWifiCb = std::function<void(std::function<void()>)>;
// Called when wifiSingleClientMode changes via this device's own web UI, so the
// mesh-wide policy state can be advanced, persisted, and synchronized.
using MeshPolicyCb = std::function<void(bool singleClientMode)>;
// Polled to report whether this device is right now mid-attempt to join WiFi.
using WifiAttemptingCb = std::function<bool()>;
// Called from the "Retry WiFi" button: give every mesh device (including any
// stuck in WifiElection::State::GaveUp) a fresh, immediate connect attempt.
using WifiRetryCb = std::function<void()>;
// Polled for this device's own current battery status (see BatteryMonitor.h).
using BatteryStatusCb = std::function<BatteryMonitor::Status()>;
// Called after the MQTT broker host/port/user/password change via /api/config,
// so the client can reconnect with the new settings without a reboot.
using MqttReconfigureCb = std::function<void()>;
// Called after batteryMonitoringEnabled changes via /api/config (new value),
// so the monitor can be enabled/disabled live instead of waiting for reboot.
using BatteryMonitoringChangedCb = std::function<void(bool)>;
// Called after apPassword changes via /api/config, so the AP can be
// restarted with the new password live if it's currently the active interface.
using ApPasswordChangedCb = std::function<void()>;
// Called after timezone changes via /api/config (new POSIX TZ string), so
// TimeSync can re-apply it live instead of waiting for reboot.
using TimezoneChangedCb = std::function<void(const char*)>;
// Called from POST /api/wifi/add when this device has no active WiFi
// connection, to kick off a live connect attempt for the newly-saved
// network instead of waiting for the next reboot. Passed a callback that
// fires once the attempt settles (success or failure).
using WifiConnectForConfirmCb = std::function<void(std::function<void(bool)>)>;
// Called from POST /api/wifi/confirm-disable-ap to end an AwaitingConfirm
// hold early, disabling the AP right away instead of waiting out the timeout.
using ConfirmApDisableCb = std::function<void()>;
// Polled to report whether this device is holding its AP open, connected,
// waiting for the user to confirm it's safe to disable (see WifiConnectForConfirmCb).
using WifiAwaitingApConfirmCb = std::function<bool()>;

class BatteryWebServer {
   private:
    // Logs every incoming request; always returns false so real handlers proceed.
    // Must be added before any routes.
    struct RequestLogger : public AsyncWebHandler {
        bool canHandle(AsyncWebServerRequest* r) const override {
            Logger::d("[web] %s %s t=%lu", r->methodToString(), r->url().c_str(), millis());
            return false;
        }
    };

    struct SceneSaveState {
        String buffer;
        String id;
        File file;
        uint32_t prevHash = 0;
        bool failed = false;
        bool written = false;
        const char* error = nullptr;
    };

    struct StorageUploadState {
        File file;
        bool failed = false;
        const char* error = nullptr;
    };

    // What a streaming route (scene save, storage upload) stores in
    // AsyncWebServerRequest::_tempObject: the handler's own per-request
    // scratch state (state) plus the response it computed once the final
    // chunk was processed — the response can only actually be sent from the
    // onRequest callback (below, after every onBody call has fired), not
    // from inside onBody itself.
    struct StreamCtx {
        void* state = nullptr;
        bool done = false;
        int status = 200;
        JsonDocument body;
        bool restart = false;
    };

   public:
    void begin(GroupChangeCb onGroupChange, GroupLightCb onGroupLight, GroupSyncCb onGroupSync,
               SetRemoteGroupCb onSetRemote, PeerRegistry* peers,
               SceneSyncManager* sceneSync = nullptr, SetRemoteSyncCb onSetRemoteSync = nullptr,
               ResolveConflictCb onResolveConflict = nullptr, PushConfigCb onPushConfig = nullptr,
               TriggerPeerUpdateCb onTriggerPeerUpdate = nullptr,
               MeshSearchCb onMeshSearch = nullptr, CheckPeerUpdateCb onCheckPeerUpdate = nullptr,
               RequestWifiCb onRequestWifi = nullptr, MeshPolicyCb onMeshPolicyChange = nullptr,
               WifiAttemptingCb onWifiAttempting = nullptr, WifiRetryCb onWifiRetry = nullptr,
               ChannelManager* channelMgr = nullptr, SdCardManager* sdCard = nullptr) {
        _onGroupChange = onGroupChange;
        _onGroupLight = onGroupLight;
        _onGroupSync = onGroupSync;
        _onSetRemote = onSetRemote;
        _peers = peers;
        _sceneSync = sceneSync;
        _onSetRemoteSync = onSetRemoteSync;
        _onResolveConflict = onResolveConflict;
        _onPushConfig = onPushConfig;
        _onTriggerPeerUpdate = onTriggerPeerUpdate;
        _onMeshSearch = onMeshSearch;
        _onCheckPeerUpdate = onCheckPeerUpdate;
        _onRequestWifi = onRequestWifi;
        _onMeshPolicyChange = onMeshPolicyChange;
        _onWifiAttempting = onWifiAttempting;
        _onWifiRetry = onWifiRetry;
        _channelMgr = channelMgr;
        _sdCard = sdCard;

        Logger::i("[web] starting on port 80");
        _server.addHandler(&_reqLogger);

        _ws = new AsyncWebSocket("/ws");
        _ws->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType t, void*,
                            uint8_t*, size_t) {
            if (t == WS_EVT_CONNECT) {
                Logger::d("[web] WS #%u connected", c->id());
                _pushPeers();
                _pushGroups();
            }
        });
        _server.addHandler(_ws);

        // Every endpoint below is registered exactly once via _get/_post/
        // _postSimple/_postStream, which both wires it up for HTTP (through
        // ESPAsyncWebServer) and records it in _routes — the table
        // SerialConfigServer's dispatch() walks for the USB-serial config
        // transport (see #355). A new endpoint only needs to be added here
        // once to work over both.
        //
        // API routes must be registered before serveStatic, otherwise the
        // static handler matches /api/* paths and tries to open them from LittleFS.
        _get("/api/config", [this](ApiRequest& q, ApiResponse& s) { _getConfig(q, s); });
        _post("/api/config", [this](ApiRequest& q, ApiResponse& s) { _postConfig(q, s); });

        _postSimple("/api/mqtt/clear", [this](ApiRequest& q, ApiResponse& s) { _clearMqtt(q, s); });

        _get("/api/wifi", [this](ApiRequest& q, ApiResponse& s) { _getWifi(q, s); });
        _post("/api/wifi/add", [this](ApiRequest& q, ApiResponse& s) { _addWifi(q, s); });
        _post("/api/wifi/delete", [this](ApiRequest& q, ApiResponse& s) { _deleteWifi(q, s); });
        _post("/api/wifi/move", [this](ApiRequest& q, ApiResponse& s) { _moveWifi(q, s); });
        _postSimple("/api/wifi/confirm-disable-ap", [this](ApiRequest&, ApiResponse& s) {
            if (_onConfirmApDisable) _onConfirmApDisable();
            s.body["ok"] = true;
        });

        _get("/api/peers", [this](ApiRequest& q, ApiResponse& s) { _getPeers(q, s); });

        _post("/api/groups/create", [this](ApiRequest& q, ApiResponse& s) { _createGroup(q, s); });
        _post("/api/groups/update", [this](ApiRequest& q, ApiResponse& s) { _updateGroup(q, s); });
        _post("/api/groups/delete", [this](ApiRequest& q, ApiResponse& s) { _deleteGroup(q, s); });

        _post("/api/peers/setgroup",
              [this](ApiRequest& q, ApiResponse& s) { _setRemoteGroup(q, s); });

        _get("/api/lights", [this](ApiRequest& q, ApiResponse& s) { _getLights(q, s); });
        _post("/api/lights/add", [this](ApiRequest& q, ApiResponse& s) { _addLight(q, s); });
        _post("/api/lights/update", [this](ApiRequest& q, ApiResponse& s) { _updateLight(q, s); });
        _post("/api/lights/delete", [this](ApiRequest& q, ApiResponse& s) { _deleteLight(q, s); });
        _post("/api/lights/test", [this](ApiRequest& q, ApiResponse& s) { _testLight(q, s); });
        _post("/api/lights/testcolor",
              [this](ApiRequest& q, ApiResponse& s) { _testColorOrder(q, s); });

        _get("/api/sounds", [this](ApiRequest& q, ApiResponse& s) { _getSounds(q, s); });
        _post("/api/sounds/add", [this](ApiRequest& q, ApiResponse& s) { _addSound(q, s); });
        _post("/api/sounds/update", [this](ApiRequest& q, ApiResponse& s) { _updateSound(q, s); });
        _post("/api/sounds/delete", [this](ApiRequest& q, ApiResponse& s) { _deleteSound(q, s); });
        _post("/api/sounds/test", [this](ApiRequest& q, ApiResponse& s) { _testSound(q, s); });

        _get("/api/storage", [this](ApiRequest& q, ApiResponse& s) { _getStorage(q, s); });
        _postStream("/api/storage/upload",
                    [this](ApiRequest& q, ApiResponse& s) { _storageUpload(q, s); });
        _post("/api/storage/delete",
              [this](ApiRequest& q, ApiResponse& s) { _deleteStorageFile(q, s); });

        _get("/api/buttons", [this](ApiRequest& q, ApiResponse& s) { _getButtons(q, s); });
        _post("/api/buttons/add", [this](ApiRequest& q, ApiResponse& s) { _addButton(q, s); });
        _post("/api/buttons/update",
              [this](ApiRequest& q, ApiResponse& s) { _updateButton(q, s); });
        _post("/api/buttons/delete",
              [this](ApiRequest& q, ApiResponse& s) { _deleteButton(q, s); });

        // ── Scene API ─────────────────────────────────────────────────────────
        // Specific routes must be registered before /api/scenes because
        // ESPAsyncWebServer prefix-matches: /api/scenes would otherwise
        // intercept /api/scenes/get, /api/scenes/save, etc.
        SceneManager::init();

        _get("/api/scenes/get", [this](ApiRequest& q, ApiResponse& s) { _getScene(q, s); });
        _post("/api/scenes/create", [this](ApiRequest& q, ApiResponse& s) { _createScene(q, s); });
        _post("/api/scenes/delete", [this](ApiRequest& q, ApiResponse& s) { _deleteScene(q, s); });
        _postStream("/api/scenes/save",
                    [this](ApiRequest& q, ApiResponse& s) { _saveScene(q, s); });
        _get("/api/scenes", [this](ApiRequest& q, ApiResponse& s) { _getScenes(q, s); });

        // ── Scene sync API ────────────────────────────────────────────────────
        _get("/api/scenes/sync/conflicts",
             [this](ApiRequest& q, ApiResponse& s) { _getSyncConflicts(q, s); });
        _post("/api/scenes/sync/resolve",
              [this](ApiRequest& q, ApiResponse& s) { _resolveSyncConflict(q, s); });
        _post("/api/peers/setscenesync",
              [this](ApiRequest& q, ApiResponse& s) { _setRemoteSceneSync(q, s); });
        _post("/api/peers/pushconfig",
              [this](ApiRequest& q, ApiResponse& s) { _pushConfig(q, s); });
        _post("/api/peers/triggerupdate",
              [this](ApiRequest& q, ApiResponse& s) { _triggerPeerUpdate(q, s); });
        _post("/api/peers/checkupdate",
              [this](ApiRequest& q, ApiResponse& s) { _checkPeerUpdate(q, s); });

        _postSimple("/api/update/trigger", [this](ApiRequest&, ApiResponse& s) {
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::triggerAsync(); });
            else
                Updater::triggerAsync();
            s.body["ok"] = true;
        });

        _get("/api/update/status", [](ApiRequest&, ApiResponse& s) {
            auto& u = Updater::status();
            s.body["currentVersion"] = u.currentVersion;
            s.body["latestVersion"] = u.latestVersion;
            s.body["hasUpdate"] = u.hasUpdate;
            s.body["progress"] = u.progress;
            s.body["state"] = _fwStateToString(u.state);
            if (u.error) s.body["error"] = u.error;
        });

        _postSimple("/api/update/check", [this](ApiRequest&, ApiResponse& s) {
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::checkAsync(); });
            else
                Updater::checkAsync();
            s.body["ok"] = true;
        });

        _postSimple("/api/update/apply", [this](ApiRequest&, ApiResponse& s) {
            auto& u = Updater::status();
            if (!u.hasUpdate) {
                s.status = 400;
                s.body["error"] = "no update available";
                return;
            }
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::applyAsync(); });
            else
                Updater::applyAsync();
            s.body["ok"] = true;
        });

        // Experimental "install from PR" flow — every endpoint below requires
        // Config::prOtaEnabled, so it costs nothing unless explicitly opted in.
        _postSimple("/api/update/prs/refresh", [this](ApiRequest&, ApiResponse& s) {
            if (!Config::get().prOtaEnabled) {
                s.status = 403;
                s.body["error"] = "PR installs are disabled";
                return;
            }
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::listPrBuildsAsync(); });
            else
                Updater::listPrBuildsAsync();
            s.body["ok"] = true;
        });

        _get("/api/update/prs", [](ApiRequest&, ApiResponse& s) {
            auto& st = Updater::prListStatus();
            s.body["state"] = _prListStateToString(st.state);
            JsonArray arr = s.body["prs"].to<JsonArray>();
            for (auto& b : st.builds) {
                JsonObject o = arr.add<JsonObject>();
                o["number"] = b.number;
                o["title"] = b.title;
                o["tag"] = b.tag;
            }
            if (st.error) s.body["error"] = st.error;
        });

        _post("/api/update/apply-pr", [this](ApiRequest& q, ApiResponse& s) {
            if (!Config::get().prOtaEnabled) {
                s.status = 403;
                s.body["error"] = "PR installs are disabled";
                return;
            }
            String tag = q.body["tag"] | "";
            if (_onRequestWifi)
                _onRequestWifi([tag]() { Updater::applyPrAsync(tag); });
            else
                Updater::applyPrAsync(tag);
            s.body["ok"] = true;
        });

        _postSimple("/api/reset", [](ApiRequest&, ApiResponse& s) {
            Config::reset();
            s.body["ok"] = true;
            s.restart = true;
        });

        _postSimple("/api/mesh/search", [this](ApiRequest&, ApiResponse& s) {
            if (_onMeshSearch) _onMeshSearch();
            s.body["ok"] = true;
        });

        // Body: {enabled}. Runtime-safe, mesh-wide toggle — no reboot, applies
        // to this device immediately and broadcasts to peers (see WifiElection).
        _post("/api/mesh/wifipolicy",
              [this](ApiRequest& q, ApiResponse& s) { _setWifiPolicy(q, s); });

        // Manual "retry WiFi now" — applies locally and broadcasts to every
        // peer, so a mesh where every candidate gave up doesn't need the
        // mode toggled off and back on to try again.
        _postSimple("/api/mesh/wifiretry", [this](ApiRequest&, ApiResponse& s) {
            if (_onWifiRetry) _onWifiRetry();
            s.body["ok"] = true;
        });

        // Browsers always request a favicon; return 204 so the request doesn't
        // fall through serveStatic's default-file fallback and generate log noise.
        _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(204); });

        // Client-side tab routes must serve the SPA shell so direct browser
        // navigation to /dashboard, /settings, /scenes, or /scenes/<id>
        // does not 404 before the frontend router takes over.
        auto sendIndex = [](AsyncWebServerRequest* r) {
            r->send(LittleFS, "/index.html", "text/html");
        };
        _server.on(AsyncURIMatcher::exact("/dashboard"), HTTP_GET, sendIndex);
        _server.on(AsyncURIMatcher::exact("/settings"), HTTP_GET, sendIndex);
        _server.on(AsyncURIMatcher::exact("/scenes"), HTTP_GET, sendIndex);
        _server.on(AsyncURIMatcher::dir("/scenes"), HTTP_GET, sendIndex);

        // Static files last — catches everything not matched above
        _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        Logger::addSink([this](LogLevel lv, const char* msg) { _pushLog(lv, msg); });

        _server.begin();
        Logger::i("[web] started");
    }

    void loop() {
        if (_ws) _ws->cleanupClients();
    }

    // Called from main when peer list changes (via mesh callback)
    void pushPeers() { _pushPeers(); }
    void pushGroups() { _pushGroups(); }
    void setOnSceneSaved(SceneSavedCb cb) { _onSceneSaved = cb; }
    void setOnTestLight(TestLightCb cb) { _onTestLight = cb; }
    void setOnTestColorOrder(TestColorOrderCb cb) { _onTestColorOrder = cb; }
    void setOnTestSound(TestSoundCb cb) { _onTestSound = cb; }
    void setOnOrientationChange(OrientationChangeCb cb) { _onOrientationChange = cb; }
    void setOnColorOrderChange(ColorOrderChangeCb cb) { _onColorOrderChange = cb; }
    void setOnLightBrightnessChange(LightBrightnessChangeCb cb) { _onLightBrightnessChange = cb; }
    void setOnButtonsChanged(ButtonsChangedCb cb) { _onButtonsChanged = cb; }
    void setOnGroupsChanged(GroupsChangedCb cb) { _onGroupsChanged = cb; }
    void setOnSceneListChanged(SceneListChangedCb cb) { _onSceneListChanged = cb; }
    void setOnClearMqtt(ClearMqttCb cb) { _onClearMqtt = cb; }
    void setOnSceneSyncChanged(SceneSyncChangedCb cb) { _onSceneSyncChanged = cb; }
    void setBatteryStatusProvider(BatteryStatusCb cb) { _batteryStatusProvider = cb; }
    void setOnMqttReconfigure(MqttReconfigureCb cb) { _onMqttReconfigure = cb; }
    void setOnBatteryMonitoringChanged(BatteryMonitoringChangedCb cb) {
        _onBatteryMonitoringChanged = cb;
    }
    void setOnApPasswordChanged(ApPasswordChangedCb cb) { _onApPasswordChanged = cb; }
    void setOnTimezoneChanged(TimezoneChangedCb cb) { _onTimezoneChanged = cb; }
    void setOnWifiConnectForConfirm(WifiConnectForConfirmCb cb) { _onWifiConnectForConfirm = cb; }
    void setOnConfirmApDisable(ConfirmApDisableCb cb) { _onConfirmApDisable = cb; }
    void setWifiAwaitingApConfirmProvider(WifiAwaitingApConfirmCb cb) {
        _onWifiAwaitingApConfirm = cb;
    }

    // Looks up and invokes a registered route by (method, path) — the single
    // entry point SerialConfigServer uses to reach every endpoint above, so a
    // new endpoint only has to be registered once (via _get/_post/_postSimple/
    // _postStream) to work over both HTTP and USB-serial (see #355). Returns
    // false if no route matches; the caller sends a 404 itself in that case.
    bool dispatch(ApiRequest& req, ApiResponse& resp) {
        for (auto& rt : _routes) {
            if (rt.method == req.method && strcmp(rt.path, req.path) == 0) {
                rt.handler(req, resp);
                return true;
            }
        }
        return false;
    }

    // True for the two endpoints registered via _postStream (scene save,
    // storage upload) — SerialConfigServer needs this to know whether an
    // incoming request should be fed to the handler in chunks (one dispatch()
    // call per chunk) or all at once.
    bool isStreamingRoute(ApiMethod method, const char* path) const {
        for (auto& rt : _routes)
            if (rt.method == method && strcmp(rt.path, path) == 0) return rt.streaming;
        return false;
    }

   private:
    AsyncWebServer _server{80};
    AsyncWebSocket* _ws = nullptr;
    RequestLogger _reqLogger;
    PeerRegistry* _peers = nullptr;
    ChannelManager* _channelMgr = nullptr;
    SdCardManager* _sdCard = nullptr;
    std::vector<ApiRoute> _routes;

    GroupChangeCb _onGroupChange;
    GroupLightCb _onGroupLight;
    GroupSyncCb _onGroupSync;
    SetRemoteGroupCb _onSetRemote;
    SetRemoteSyncCb _onSetRemoteSync;
    ResolveConflictCb _onResolveConflict;
    PushConfigCb _onPushConfig;
    TriggerPeerUpdateCb _onTriggerPeerUpdate;
    CheckPeerUpdateCb _onCheckPeerUpdate;
    MeshSearchCb _onMeshSearch;
    RequestWifiCb _onRequestWifi;
    MeshPolicyCb _onMeshPolicyChange;
    WifiAttemptingCb _onWifiAttempting;
    WifiRetryCb _onWifiRetry;
    BatteryStatusCb _batteryStatusProvider;
    SceneSyncManager* _sceneSync = nullptr;
    SceneSavedCb _onSceneSaved;
    TestLightCb _onTestLight;
    TestColorOrderCb _onTestColorOrder;
    TestSoundCb _onTestSound;
    OrientationChangeCb _onOrientationChange;
    ColorOrderChangeCb _onColorOrderChange;
    LightBrightnessChangeCb _onLightBrightnessChange;
    ButtonsChangedCb _onButtonsChanged;
    GroupsChangedCb _onGroupsChanged;
    SceneListChangedCb _onSceneListChanged;
    ClearMqttCb _onClearMqtt;
    SceneSyncChangedCb _onSceneSyncChanged;
    MqttReconfigureCb _onMqttReconfigure;
    BatteryMonitoringChangedCb _onBatteryMonitoringChanged;
    ApPasswordChangedCb _onApPasswordChanged;
    TimezoneChangedCb _onTimezoneChanged;
    WifiConnectForConfirmCb _onWifiConnectForConfirm;
    ConfirmApDisableCb _onConfirmApDisable;
    WifiAwaitingApConfirmCb _onWifiAwaitingApConfirm;

    // ── transport-agnostic route registration ───────────────────────────────
    // Each of these both (a) registers the handler in _routes for
    // SerialConfigServer::dispatch(), and (b) wires up the matching
    // AsyncWebServer route, translating to/from ApiRequest/ApiResponse so the
    // handler body itself never touches AsyncWebServerRequest.

    void _sendApiResponse(AsyncWebServerRequest* r, ApiResponse& resp) {
        if (resp.rawFilePath.length())
            r->send(LittleFS, resp.rawFilePath, "application/json");
        else
            _sendJson(r, resp.status, resp.body);
        if (resp.restart) {
            delay(200);
            ESP.restart();
        }
    }

    // GET endpoints never carry a body; /api/scenes/get is the one route
    // that takes a query parameter ("id") instead.
    void _get(const char* path, ApiHandler h) {
        _routes.push_back({ApiMethod::GET, path, h, false});
        _server.on(path, HTTP_GET, [this, path, h](AsyncWebServerRequest* r) {
            ApiRequest req;
            req.method = ApiMethod::GET;
            req.path = path;
            String qv;
            if (r->hasParam("id")) {
                qv = r->getParam("id")->value();
                req.query = qv.c_str();
            }
            ApiResponse resp;
            h(req, resp);
            _sendApiResponse(r, resp);
        });
    }

    // POST endpoints whose whole JSON body arrives in a single onBody call —
    // true for every non-streaming POST here, same assumption the original
    // per-endpoint handlers already made via _parseJson.
    void _post(const char* path, ApiHandler h) {
        _routes.push_back({ApiMethod::POST, path, h, false});
        _server.on(
            path, HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this, path, h](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                JsonDocument doc;
                if (l && deserializeJson(doc, d, l)) {
                    auto e = _makeErr("bad json");
                    _sendJson(r, 400, e);
                    return;
                }
                ApiRequest req;
                req.method = ApiMethod::POST;
                req.path = path;
                req.body = doc.as<JsonVariantConst>();
                ApiResponse resp;
                h(req, resp);
                _sendApiResponse(r, resp);
            });
    }

    // POST endpoints that take no body at all — registered as onRequest-only
    // (matching how these already behaved before this route table existed),
    // since relying on the onBody callback for a body-less request isn't
    // guaranteed to fire.
    void _postSimple(const char* path, ApiHandler h) {
        _routes.push_back({ApiMethod::POST, path, h, false});
        _server.on(path, HTTP_POST, [this, path, h](AsyncWebServerRequest* r) {
            ApiRequest req;
            req.method = ApiMethod::POST;
            req.path = path;
            ApiResponse resp;
            h(req, resp);
            _sendApiResponse(r, resp);
        });
    }

    // POST endpoints whose body streams in over multiple chunks (scene save,
    // storage upload) — h is called once per chunk via onBody, computing a
    // response once the final chunk (chunkIndex + chunkLen >= chunkTotal) has
    // been processed. The response can only be sent once ESPAsyncWebServer
    // calls the onRequest callback (guaranteed to fire once, after every
    // onBody call) — calling request->send() from inside onBody itself isn't
    // the library's supported pattern, so the computed response is stashed in
    // StreamCtx (via _tempObject) for onRequest to actually send.
    void _postStream(const char* path, ApiHandler h) {
        _routes.push_back({ApiMethod::POST, path, h, true});
        _server.on(
            path, HTTP_POST,
            [this](AsyncWebServerRequest* r) {
                auto* ctx = static_cast<StreamCtx*>(r->_tempObject);
                if (!ctx || !ctx->done) {
                    auto e = _makeErr("no body");
                    _sendJson(r, 400, e);
                    delete ctx;
                    r->_tempObject = nullptr;
                    return;
                }
                _sendJson(r, ctx->status, ctx->body);
                bool restart = ctx->restart;
                delete ctx;
                r->_tempObject = nullptr;
                if (restart) {
                    delay(200);
                    ESP.restart();
                }
            },
            nullptr,
            [this, path, h](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t index,
                            size_t total) {
                auto* ctx = static_cast<StreamCtx*>(r->_tempObject);
                if (!ctx) {
                    ctx = new StreamCtx();
                    r->_tempObject = ctx;
                }

                ApiRequest req;
                req.method = ApiMethod::POST;
                req.path = path;
                req.chunk = d;
                req.chunkLen = l;
                req.chunkIndex = index;
                req.chunkTotal = total;
                req.streamState = ctx->state;
                String qv;
                // Only /api/storage/upload takes a query param ("name");
                // harmless no-op for /api/scenes/save.
                if (r->hasParam("name")) {
                    qv = r->getParam("name")->value();
                    req.query = qv.c_str();
                }
                ApiResponse resp;
                resp.streamDone = false;
                h(req, resp);
                ctx->state = req.streamState;
                if (resp.streamDone) {
                    ctx->done = true;
                    ctx->status = resp.status;
                    ctx->body = resp.body;
                    ctx->restart = resp.restart;
                }
            });
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    static JsonDocument _makeErr(const char* e) {
        JsonDocument d;
        d["error"] = e;
        return d;
    }

    static void _sendJson(AsyncWebServerRequest* r, int code, JsonDocument& doc) {
        String s;
        serializeJson(doc, s);
        r->send(code, "application/json", s);
    }

    // Parses a "xx:xx:xx:xx:xx:xx" MAC string into a 6-byte array.
    static bool _parseMac(const char* macStr, uint8_t* mac) {
        return sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3],
                      &mac[4], &mac[5]) == 6;
    }

    static const char* _fwStateName(uint8_t v) {
        static const char* kNames[] = {"idle", "checking", "downloading", "error", "done"};
        return v < 5 ? kNames[v] : "idle";
    }
    static const char* _fwStateToString(Updater::State s) { return _fwStateName((uint8_t)s); }
    static const char* _fwStateToString(FwState s) { return _fwStateName((uint8_t)s); }

    static const char* _prListStateToString(Updater::PrListState s) {
        static const char* kNames[] = {"idle", "loading", "done", "error"};
        uint8_t v = (uint8_t)s;
        return v < 4 ? kNames[v] : "idle";
    }

    // ── GET /api/config ──────────────────────────────────────────────────────
    void _getConfig(ApiRequest&, ApiResponse& s) {
        auto& c = Config::get();
        JsonDocument& doc = s.body;
        doc["deviceName"] = c.deviceName;
        doc["otaPort"] = c.otaPort;
        doc["otaEnabled"] = c.otaEnabled;
        doc["mac"] = WiFi.macAddress();
        doc["version"] = FW_VERSION;
        doc["logLevel"] = c.logLevel;
        doc["sceneSyncEnabled"] = c.sceneSyncEnabled;
        doc["checkUpdateOnStartup"] = c.checkUpdateOnStartup;
        doc["wifiSingleClientMode"] = c.wifiSingleClientMode;
        doc["batteryHwSupported"] = BatteryMonitor::kHwSupported;
        doc["batteryMonitoringEnabled"] = c.batteryMonitoringEnabled;
        doc["prOtaBoardSupported"] = Updater::supportsPrOta();
        doc["prOtaEnabled"] = c.prOtaEnabled;
        doc["prTrack"] = c.prTrack;
        doc["i2cSdaPin"] = c.i2cSdaPin;
        doc["i2cSclPin"] = c.i2cSclPin;
        doc["expanderChip"] = (uint8_t)c.expanderChip;
        doc["expanderAddress"] = c.expanderAddress;
        doc["mqttHost"] = c.mqttHost;
        doc["mqttPort"] = c.mqttPort;
        doc["mqttUser"] = c.mqttUser;
        // mqttPassword intentionally omitted — write-only from UI
        doc["githubRepo"] = c.githubRepo;
        // githubToken intentionally omitted — write-only from UI
        doc["timezone"] = c.timezone;

        JsonArray lightsArr = doc["lights"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            if (!c.lights[i].exists) continue;
            auto& l = c.lights[i];
            JsonObject lo = lightsArr.add<JsonObject>();
            lo["index"] = i;
            lo["name"] = l.name;
            lo["ledType"] = (uint8_t)l.ledType;
            lo["colorOrder"] = (uint8_t)l.colorOrder;
            lo["dataPin"] = l.dataPin;
            lo["clockPin"] = l.clockPin;
            lo["width"] = l.width;
            lo["height"] = l.height;
            lo["matrixStart"] = (uint8_t)l.matrixStart;
            lo["matrixDir"] = (uint8_t)l.matrixDir;
            lo["matrixSerpentine"] = l.matrixSerpentine;
            lo["wrapWidth"] = l.wrapWidth;
            lo["wrapHeight"] = l.wrapHeight;
            lo["groupId"] = l.groupId;
            lo["brightnessOverrideEnabled"] = l.brightnessOverrideEnabled;
            lo["brightnessOverride"] = l.brightnessOverride;
        }

        JsonArray arr = doc["groups"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            if (!c.groups[i].exists) continue;
            serializeGroup(arr.add<JsonObject>(), c.groups[i]);
        }
    }

    // ── POST /api/config ─────────────────────────────────────────────────────
    // Only deviceName/otaPort/otaEnabled genuinely require a reboot: mDNS,
    // ArduinoOTA, the AP SSID, and MQTT's topic prefix all derive from
    // deviceName and are only initialized once at boot, and ArduinoOTA itself
    // is only begin()'d once (conditionally on otaEnabled). Every other field
    // is applied live via the callbacks below, and the response reports
    // whether a reboot is actually happening so the web UI only shows/waits
    // for one when it's really going to happen.
    void _postConfig(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        auto& c = Config::get();

        bool rebootNeeded = false;
        if (!doc["deviceName"].isNull() && strcmp(c.deviceName, doc["deviceName"] | "") != 0) {
            strlcpy(c.deviceName, doc["deviceName"], sizeof(c.deviceName));
            rebootNeeded = true;
        }
        if (!doc["otaPort"].isNull() && (uint16_t)doc["otaPort"] != c.otaPort) {
            c.otaPort = doc["otaPort"];
            rebootNeeded = true;
        }
        if (!doc["otaEnabled"].isNull() && (bool)doc["otaEnabled"] != c.otaEnabled) {
            c.otaEnabled = (bool)doc["otaEnabled"];
            rebootNeeded = true;
        }

        bool apPasswordChanged = false;
        if (!doc["apPassword"].isNull()) {
            strlcpy(c.apPassword, doc["apPassword"], sizeof(c.apPassword));
            apPasswordChanged = true;
        }

        if (!doc["logLevel"].isNull()) {
            c.logLevel = (uint8_t)doc["logLevel"];
            Logger::setLevel((LogLevel)c.logLevel);
        }
        if (!doc["sceneSyncEnabled"].isNull()) {
            bool prev = c.sceneSyncEnabled;
            c.sceneSyncEnabled = (bool)doc["sceneSyncEnabled"];
            if (c.sceneSyncEnabled && !prev && _sceneSync) _sceneSync->onSyncEnabled();
            if (c.sceneSyncEnabled != prev && _onSceneSyncChanged) _onSceneSyncChanged();
        }
        if (!doc["checkUpdateOnStartup"].isNull())
            c.checkUpdateOnStartup = (bool)doc["checkUpdateOnStartup"];

        bool batteryChanged = false;
        if (!doc["batteryMonitoringEnabled"].isNull()) {
            bool newVal = (bool)doc["batteryMonitoringEnabled"];
            batteryChanged = newVal != c.batteryMonitoringEnabled;
            c.batteryMonitoringEnabled = newVal;
        }
        if (!doc["prOtaEnabled"].isNull()) c.prOtaEnabled = (bool)doc["prOtaEnabled"];

        // The I2C bus is only ever brought up once at boot (see main.cpp) —
        // unlike battery/mqtt/timezone above, there's no live-reconfigure path,
        // so changing it always needs a reboot.
        if (!doc["i2cSdaPin"].isNull() && !doc["i2cSclPin"].isNull()) {
            uint8_t sda = doc["i2cSdaPin"];
            uint8_t scl = doc["i2cSclPin"];
            if (sda == PIN_UNUSED || scl == PIN_UNUSED) {
                if (Config::i2cBusInUse()) {
                    s.status = 400;
                    s.body["error"] =
                        "I2C bus still used by a configured sound output, button, or "
                        "the configured expander";
                    return;
                }
                if (c.i2cSdaPin != PIN_UNUSED || c.i2cSclPin != PIN_UNUSED) rebootNeeded = true;
                c.i2cSdaPin = PIN_UNUSED;
                c.i2cSclPin = PIN_UNUSED;
            } else {
                if (sda == scl) {
                    s.status = 400;
                    s.body["error"] = "SDA and SCL must be different pins";
                    return;
                }
                if (Config::isPinInUse(sda, -1, -1, -1, /*excludeI2cBus=*/true) ||
                    Config::isPinInUse(scl, -1, -1, -1, /*excludeI2cBus=*/true)) {
                    s.status = 400;
                    s.body["error"] = "pin already in use";
                    return;
                }
                if (c.i2cSdaPin != sda || c.i2cSclPin != scl) rebootNeeded = true;
                c.i2cSdaPin = sda;
                c.i2cSclPin = scl;
            }
        }

        // The device's single I2C expander (see IoExpanderChip) — same
        // reasoning as the I2C bus above: no live-reconfigure path, so
        // changing it always needs a reboot. Uses c.i2cSdaPin/i2cSclPin as
        // already updated by the block above, so enabling the bus and the
        // expander in the same save works.
        if (!doc["expanderChip"].isNull() || !doc["expanderAddress"].isNull()) {
            IoExpanderChip newChip = doc["expanderChip"].isNull()
                                         ? c.expanderChip
                                         : (IoExpanderChip)(uint8_t)doc["expanderChip"];
            uint8_t newAddr = doc["expanderAddress"].isNull() ? c.expanderAddress
                                                              : (uint8_t)doc["expanderAddress"];
            if (newChip != IoExpanderChip::None &&
                (c.i2cSdaPin == PIN_UNUSED || c.i2cSclPin == PIN_UNUSED)) {
                s.status = 400;
                s.body["error"] = "configure the device I2C bus first";
                return;
            }
            if (newChip == IoExpanderChip::None && Config::expanderInUse()) {
                s.status = 400;
                s.body["error"] = "expander still used by a configured sound output or button";
                return;
            }
            if (c.expanderChip != newChip || c.expanderAddress != newAddr) rebootNeeded = true;
            c.expanderChip = newChip;
            c.expanderAddress = newAddr;
        }
        // wifiSingleClientMode is intentionally not handled here — it's a
        // runtime-safe, mesh-wide toggle exposed from the device list instead
        // (POST /api/mesh/wifipolicy), so flipping it doesn't force the
        // "save settings" reboot that every other field here used to trigger.

        bool mqttChanged = false;
        if (!doc["mqttHost"].isNull() && strcmp(c.mqttHost, doc["mqttHost"] | "") != 0) {
            strlcpy(c.mqttHost, doc["mqttHost"], sizeof(c.mqttHost));
            mqttChanged = true;
        }
        if (!doc["mqttPort"].isNull() && (uint16_t)doc["mqttPort"] != c.mqttPort) {
            c.mqttPort = (uint16_t)doc["mqttPort"];
            mqttChanged = true;
        }
        if (!doc["mqttUser"].isNull() && strcmp(c.mqttUser, doc["mqttUser"] | "") != 0) {
            strlcpy(c.mqttUser, doc["mqttUser"], sizeof(c.mqttUser));
            mqttChanged = true;
        }
        if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0) {
            strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
            mqttChanged = true;
        }

        if (!doc["githubRepo"].isNull())
            strlcpy(c.githubRepo, doc["githubRepo"], sizeof(c.githubRepo));
        if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
            strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));

        bool timezoneChanged = false;
        if (!doc["timezone"].isNull() && strcmp(c.timezone, doc["timezone"] | "") != 0) {
            strlcpy(c.timezone, doc["timezone"], sizeof(c.timezone));
            timezoneChanged = true;
        }

        Config::save();

        if (mqttChanged && _onMqttReconfigure) _onMqttReconfigure();
        if (batteryChanged && _onBatteryMonitoringChanged)
            _onBatteryMonitoringChanged(c.batteryMonitoringEnabled);
        if (apPasswordChanged && _onApPasswordChanged) _onApPasswordChanged();
        if (timezoneChanged && _onTimezoneChanged) _onTimezoneChanged(c.timezone);

        s.body["ok"] = true;
        s.body["rebooting"] = rebootNeeded;
        s.restart = rebootNeeded;
    }

    // ── POST /api/mqtt/clear ─────────────────────────────────────────────────
    // Removes the MQTT broker config and clears every retained message this
    // device may have published (state, discovery, telemetry) — a plain
    // /api/config save with an emptied mqttHost would leave all of that
    // stale on the broker forever, since nothing else ever cleans it up.
    // Runtime-safe, like the dedicated /api/mesh/wifipolicy endpoint — no
    // reboot needed, MqttManager just disables itself for the rest of this session.
    void _clearMqtt(ApiRequest&, ApiResponse& s) {
        if (_onClearMqtt) _onClearMqtt();
        auto& c = Config::get();
        c.mqttHost[0] = '\0';
        c.mqttPort = 1883;
        c.mqttUser[0] = '\0';
        c.mqttPassword[0] = '\0';
        Config::save();
        s.body["ok"] = true;
    }

    // ── GET /api/peers ───────────────────────────────────────────────────────
    void _getPeers(ApiRequest&, ApiResponse& s) { _buildPeersJson(s.body); }

    void _buildPeersJson(JsonDocument& doc) {
        auto& c = Config::get();
        const auto& us = Updater::status();

        doc["wifiSingleClientMode"] = c.wifiSingleClientMode;

        auto self = doc["self"].to<JsonObject>();
        self["mac"] = WiFi.macAddress();
        self["name"] = c.deviceName;
        self["online"] = true;
        self["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
        self["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
        self["hasWifiNetworks"] = Config::wifiCount() > 0;
        self["wifiConnecting"] = _onWifiAttempting && _onWifiAttempting();
        self["wifiAwaitingApConfirm"] = _onWifiAwaitingApConfirm && _onWifiAwaitingApConfirm();
        self["channel"] = _channelMgr ? _channelMgr->lockedChannel() : 0;
        self["channelSearching"] = _channelMgr && _channelMgr->isSearching();
        self["version"] = FW_VERSION;
        self["fwState"] = _fwStateToString(us.state);
        {
            BatteryMonitor::Status bs =
                _batteryStatusProvider ? _batteryStatusProvider() : BatteryMonitor::Status{};
            self["batteryPresent"] = bs.present;
            self["batteryPercent"] = bs.percent;
            self["batteryCharging"] = bs.state == BatteryMonitor::State::Charging;
        }
        {
            JsonArray la = self["lights"].to<JsonArray>();
            for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
                if (!c.lights[i].exists) continue;
                JsonObject lo = la.add<JsonObject>();
                lo["index"] = i;
                lo["name"] = c.lights[i].name;
                lo["groupId"] = c.lights[i].groupId;
                lo["ledType"] = (uint8_t)c.lights[i].ledType;
                lo["width"] = c.lights[i].width;
                lo["height"] = c.lights[i].height;
                lo["wrapWidth"] = c.lights[i].wrapWidth;
                lo["brightnessOverrideEnabled"] = c.lights[i].brightnessOverrideEnabled;
                lo["brightnessOverride"] = c.lights[i].brightnessOverride;
            }
        }

        JsonArray arr = doc["peers"].to<JsonArray>();
        if (_peers) {
            for (auto& p : *_peers) {
                if (!p.active) continue;
                auto o = arr.add<JsonObject>();
                o["mac"] = p.macStr();
                o["name"] = p.name;
                o["online"] = p.online();
                o["rssi"] = p.rssi;
                o["sceneSyncEnabled"] = p.sceneSyncEnabled;
                o["wifiConnected"] = p.wifiConnected;
                o["hasWifiNetworks"] = p.hasWifiNetworks;
                o["wifiConnecting"] = p.wifiConnecting;
                o["version"] = p.fwVersion;
                o["fwState"] = _fwStateToString(p.fwState);
                o["batteryPresent"] = p.batteryPresent;
                o["batteryPercent"] = p.batteryPercent;
                o["batteryCharging"] = p.batteryCharging;
                JsonArray la = o["lights"].to<JsonArray>();
                for (uint8_t i = 0; i < p.lightCount && i < MAX_LIGHTS; i++) {
                    JsonObject lo = la.add<JsonObject>();
                    lo["index"] = i;
                    lo["name"] = p.lightNames[i];
                    lo["groupId"] = p.lightGroupIds[i];
                }
            }
        }
    }

    // ── POST /api/groups/create ──────────────────────────────────────────────
    void _createGroup(ApiRequest& q, ApiResponse& s) {
        const char* name = q.body["name"] | "New Group";
        uint8_t id = Config::createGroup(name);
        if (id == 0xFF) {
            s.status = 400;
            s.body["error"] = "group limit reached";
            return;
        }
        const GroupConfig& g = Config::get().groups[id];
        Config::save();
        if (_onGroupSync) _onGroupSync(g);

        s.body["ok"] = true;
        s.body["id"] = id;
        _pushGroups();
    }

    // ── POST /api/groups/update ──────────────────────────────────────────────
    // Body: {id, name?, pattern?, r?, g?, b?, brightness?, speed?}
    void _updateGroup(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t id = doc["id"] | (uint8_t)0;
        GroupConfig* g = Config::group(id);
        if (!g) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }

        bool nameChanged = false;
        if (!doc["name"].isNull()) {
            nameChanged = strcmp(g->name, (const char*)doc["name"]) != 0;
            strlcpy(g->name, doc["name"], sizeof(g->name));
        }

        if (!doc["syncEnabled"].isNull()) {
            g->syncEnabled = (bool)doc["syncEnabled"];
            Config::bumpGroupRevision(*g);
            Config::save();
            if (_onGroupSync) _onGroupSync(*g);
            s.body["ok"] = true;
            _pushGroups();
            return;
        }

        // Any key besides id/name/syncEnabled is a LightConfig field — this stays in
        // sync with deserializeLightConfig automatically, no field list to maintain here.
        bool lightChanged = false;
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
            const char* key = kv.key().c_str();
            if (strcmp(key, "id") && strcmp(key, "name") && strcmp(key, "syncEnabled")) {
                lightChanged = true;
                break;
            }
        }
        if (lightChanged) {
            g->light = deserializeLightConfig(doc, g->light);
            g->light.seq++;
            if (_onGroupLight) _onGroupLight(id, g->light);
        }
        if (nameChanged) Config::bumpGroupRevision(*g);

        Config::save();
        if (nameChanged && _onGroupSync) _onGroupSync(*g);
        s.body["ok"] = true;
        if (nameChanged) _pushGroups();
    }

    // ── POST /api/groups/delete ──────────────────────────────────────────────
    void _deleteGroup(ApiRequest& q, ApiResponse& s) {
        uint8_t id = q.body["id"] | (uint8_t)0;
        if (id == 0) {
            s.status = 400;
            s.body["error"] = "cannot delete Default";
            return;
        }
        GroupConfig* g = Config::group(id);
        if (!g) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }

        Config::bumpGroupRevision(*g);
        GroupConfig tombstone = *g;
        tombstone.exists = false;
        g->exists = false;

        // Move any lights in the deleted group to Default
        bool anyMoved = false;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (l.exists && l.groupId == id) {
                l.groupId = 0;
                anyMoved = true;
            }
        }
        if (anyMoved && _onGroupChange) _onGroupChange();

        Config::save();
        if (_onGroupSync) _onGroupSync(tombstone);
        s.body["ok"] = true;
        _pushGroups();
    }

    // ── POST /api/peers/setgroup ─────────────────────────────────────────────
    // Body: {mac, lightIndex, groupId}
    void _setRemoteGroup(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t lightIndex = doc["lightIndex"] | (uint8_t)0;
        uint8_t groupId = doc["groupId"] | (uint8_t)0;
        const char* macStr = doc["mac"] | "";

        if (lightIndex >= MAX_LIGHTS) {
            s.status = 400;
            s.body["error"] = "invalid lightIndex";
            return;
        }

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            if (Config::group(groupId)) {
                Config::get().lights[lightIndex].groupId = groupId;
                Config::save();
                if (_onGroupChange) _onGroupChange();
            }
            s.body["ok"] = true;
            _pushPeers();
            return;
        }

        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            s.status = 400;
            s.body["error"] = "bad mac";
            return;
        }

        if (_onSetRemote) _onSetRemote(mac, lightIndex, groupId);
        s.body["ok"] = true;
    }

    // ── WebSocket push ───────────────────────────────────────────────────────
    void _pushPeers() {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "peers";
        _buildPeersJson(doc);
        String s;
        serializeJson(doc, s);
        _ws->textAll(s);
    }

    void _pushGroups() {
        if (_onGroupsChanged) _onGroupsChanged();
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "groups";
        JsonArray arr = doc["list"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            auto& g = Config::get().groups[i];
            if (!g.exists) continue;
            serializeGroup(arr.add<JsonObject>(), g);
        }
        String s;
        serializeJson(doc, s);
        _ws->textAll(s);
    }

    void _pushLog(LogLevel level, const char* msg) {
        if (!_ws || _ws->count() == 0) return;
        const char* lv = level == LogLevel::ERROR     ? "E"
                         : level == LogLevel::WARN    ? "W"
                         : level == LogLevel::INFO    ? "I"
                         : level == LogLevel::VERBOSE ? "V"
                                                      : "D";
        char buf[320];
        snprintf(buf, sizeof(buf), "{\"t\":\"log\",\"l\":\"%s\",\"m\":%s}", lv,
                 _jsonStr(msg).c_str());
        _ws->textAll(buf);
    }

    // ── Scene handlers ───────────────────────────────────────────────────────

    void _getScenes(ApiRequest&, ApiResponse& s) {
        SceneManager::buildList(s.body);
        JsonArray arr = s.body["scenes"].as<JsonArray>();
        Logger::d("[scene] list: %u scene(s)", arr ? (unsigned)arr.size() : 0);
    }

    void _getScene(ApiRequest& q, ApiResponse& s) {
        if (!q.query) {
            Logger::w("[scene] get: missing id param");
            s.status = 400;
            s.body["error"] = "missing id";
            return;
        }
        String path = SceneManager::path(q.query);
        Logger::d("[scene] get: id=%s path=%s exists=%d", q.query, path.c_str(),
                  LittleFS.exists(path));
        if (!LittleFS.exists(path)) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        s.rawFilePath = path;
    }

    void _createScene(ApiRequest& q, ApiResponse& s) {
        const char* name = q.body["name"] | "Unnamed";
        uint16_t w = q.body["w"] | 20;
        uint16_t h = q.body["h"] | 10;
        Logger::i("[scene] create: name=%s w=%u h=%u", name, w, h);
        String id = SceneManager::create(name, w, h);
        if (id.isEmpty()) {
            Logger::e("[scene] create: failed");
            s.status = 500;
            s.body["error"] = "create failed";
            return;
        }
        Logger::i("[scene] create: ok id=%s", id.c_str());
        if (_onSceneListChanged) _onSceneListChanged();
        s.body["ok"] = true;
        s.body["id"] = id;
    }

    void _deleteScene(ApiRequest& q, ApiResponse& s) {
        const char* id = q.body["id"] | "";
        if (!id[0]) {
            Logger::w("[scene] delete: missing id");
            s.status = 400;
            s.body["error"] = "missing id";
            return;
        }
        Logger::i("[scene] delete: id=%s", id);
        bool ok = _sceneSync ? _sceneSync->deleteScene(id) : SceneManager::remove(id);
        Logger::i("[scene] delete: %s", ok ? "ok" : "not found");
        if (ok && _onSceneListChanged) _onSceneListChanged();
        s.status = ok ? 200 : 404;
        if (ok)
            s.body["ok"] = true;
        else
            s.body["error"] = "not found";
    }

    // ── POST /api/scenes/save ────────────────────────────────────────────────
    // Streaming: the body's id field isn't known until enough of it has
    // arrived, so bytes are buffered until SceneManager::extractId finds it,
    // then written straight to LittleFS from there on (mirrors the original
    // AsyncWebServerRequest-based implementation, just keyed off
    // req.streamState instead of r->_tempObject).
    void _saveScene(ApiRequest& q, ApiResponse& s) {
        auto* st = static_cast<SceneSaveState*>(q.streamState);
        if (!st) {
            st = new SceneSaveState();
            q.streamState = st;
            Logger::d("[scene] save: body start, total=%u bytes", (unsigned)q.chunkTotal);
        }

        s.streamDone = false;

        if (!st->failed && q.chunkLen) {
            if (!st->file) {
                st->buffer.concat((const char*)q.chunk, q.chunkLen);
                Logger::d("[scene] save: buffering chunk %u bytes (buf=%u): %.80s",
                          (unsigned)q.chunkLen, (unsigned)st->buffer.length(), st->buffer.c_str());
                String found;
                if (!SceneManager::extractId(st->buffer.c_str(), st->buffer.length(), found)) {
                    if (st->buffer.length() > 16384) {
                        Logger::e("[scene] save: id not found after %u bytes",
                                  (unsigned)st->buffer.length());
                        st->failed = true;
                        st->error = "missing id";
                    } else {
                        Logger::w(
                            "[scene] save: extractId returned false for buf=%u bytes (body "
                            "complete at total=%u)",
                            (unsigned)st->buffer.length(), (unsigned)q.chunkTotal);
                    }
                } else {
                    st->id = found;
                    st->prevHash = SceneManager::crc32(st->id.c_str());
                    Logger::d("[scene] save: id=%s prevHash=%08x, opening file", st->id.c_str(),
                              st->prevHash);
                    SceneManager::init();
                    st->file = LittleFS.open(SceneManager::path(st->id.c_str()), "w");
                    if (!st->file) {
                        Logger::e("[scene] save: open failed for %s", st->id.c_str());
                        st->failed = true;
                        st->error = "open failed";
                    } else {
                        size_t written =
                            st->file.write((const uint8_t*)st->buffer.c_str(), st->buffer.length());
                        Logger::d("[scene] save: wrote initial buffer %u/%u bytes",
                                  (unsigned)written, (unsigned)st->buffer.length());
                        if (written != st->buffer.length()) {
                            Logger::e("[scene] save: initial write incomplete (%u/%u)",
                                      (unsigned)written, (unsigned)st->buffer.length());
                            st->failed = true;
                            st->error = "write failed";
                            st->file.close();
                            st->file = File();
                        } else {
                            st->buffer = "";
                            st->written = true;
                        }
                    }
                }
            } else {
                size_t written = st->file.write(q.chunk, q.chunkLen);
                Logger::d("[scene] save: chunk at index=%u len=%u written=%u",
                          (unsigned)q.chunkIndex, (unsigned)q.chunkLen, (unsigned)written);
                if (written != q.chunkLen) {
                    Logger::e("[scene] save: chunk write incomplete (%u/%u) at index=%u",
                              (unsigned)written, (unsigned)q.chunkLen, (unsigned)q.chunkIndex);
                    st->failed = true;
                    st->error = "write failed";
                    st->file.close();
                    st->file = File();
                } else {
                    st->written = true;
                }
            }
        }

        if (q.chunkIndex + q.chunkLen < q.chunkTotal) return;  // more chunks coming

        bool ok = !st->failed && st->written;
        if (st->file) st->file.close();

        if (ok) {
            Logger::i("[scene] save ok: %s", st->id.c_str());
            if (_sceneSync) _sceneSync->onSceneChanged(st->id.c_str(), st->prevHash);
            if (_onSceneSaved) _onSceneSaved(st->id.c_str());
        } else {
            Logger::e("[scene] save failed: %s (failed=%d written=%d)", st->error ? st->error : "?",
                      st->failed, st->written);
        }

        s.status = ok ? 200 : 500;
        if (ok)
            s.body["ok"] = true;
        else
            s.body["error"] = st->failed ? (st->error ? st->error : "save failed") : "save failed";
        s.streamDone = true;
        delete st;
        q.streamState = nullptr;
    }

    // ── Scene sync handlers ──────────────────────────────────────────────────

    void _getSyncConflicts(ApiRequest&, ApiResponse& s) {
        if (_sceneSync) {
            _sceneSync->buildConflictsJson(s.body);
            _sceneSync->buildPeerScenesJson(s.body);
        } else {
            s.body["conflicts"].to<JsonArray>();
            s.body["peerScenes"].to<JsonArray>();
        }
    }

    // Body: {id, sourceMac}  — sourceMac is the device whose copy wins.
    // Use sourceMac == own MAC or omit to use local copy.
    void _resolveSyncConflict(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        const char* id = doc["id"] | "";
        const char* macStr = doc["sourceMac"] | "";
        if (!id[0]) {
            s.status = 400;
            s.body["error"] = "missing id";
            return;
        }

        // Determine if sourceMac is this device or a remote one
        bool isLocal = !macStr[0] || WiFi.macAddress().equalsIgnoreCase(macStr);

        if (isLocal) {
            if (_onResolveConflict) _onResolveConflict(id, nullptr);
        } else {
            uint8_t mac[6];
            if (!_parseMac(macStr, mac)) {
                s.status = 400;
                s.body["error"] = "bad mac";
                return;
            }
            if (_onResolveConflict) _onResolveConflict(id, mac);
        }
        s.body["ok"] = true;
    }

    // ── GET /api/wifi ─────────────────────────────────────────────────────────
    void _getWifi(ApiRequest&, ApiResponse& s) {
        if (WiFi.status() == WL_CONNECTED) {
            // Use Config::wifiLast() rather than WiFi.SSID() to avoid calling
            // esp_wifi_sta_get_ap_info() from an async handler — that call can
            // block waiting for the WiFi driver lock during reconnect events.
            uint8_t last = Config::wifiLast();
            s.body["connected"] = (last < Config::wifiCount())
                                      ? (const char*)Config::wifiNetworks()[last].ssid
                                      : (const char*)nullptr;
        } else {
            s.body["connected"] = nullptr;
        }
        JsonArray arr = s.body["networks"].to<JsonArray>();
        for (uint8_t i = 0; i < Config::wifiCount(); i++) arr.add(Config::wifiNetworks()[i].ssid);
    }

    // ── POST /api/wifi/add ────────────────────────────────────────────────────
    // Body: {ssid, password}
    void _addWifi(ApiRequest& q, ApiResponse& s) {
        const char* ssid = q.body["ssid"] | "";
        const char* pass = q.body["password"] | "";
        if (strlen(ssid) == 0) {
            s.status = 400;
            s.body["error"] = "ssid required";
            return;
        }
        if (!Config::addWifiNetwork(ssid, pass)) {
            s.status = 409;
            s.body["error"] = "network list full";
            return;
        }
        // Nothing else actually connects to a newly-saved network until the
        // next reboot — kick off a live attempt now if this device isn't
        // already on WiFi, so onboarding doesn't require a manual power cycle.
        if (WiFi.status() != WL_CONNECTED && _onWifiConnectForConfirm) {
            _onWifiConnectForConfirm([](bool) {});
        }
        s.body["ok"] = true;
    }

    // ── POST /api/wifi/delete ─────────────────────────────────────────────────
    // Body: {ssid}
    void _deleteWifi(ApiRequest& q, ApiResponse& s) {
        const char* ssid = q.body["ssid"] | "";
        if (strlen(ssid) == 0) {
            s.status = 400;
            s.body["error"] = "ssid required";
            return;
        }
        Config::deleteWifiNetwork(ssid);
        s.body["ok"] = true;
    }

    // ── POST /api/wifi/move ───────────────────────────────────────────────────
    // Body: {ssid, direction: "up"|"down"} — swaps ssid with its immediate
    // neighbor; connect order is list order, so this changes priority.
    void _moveWifi(ApiRequest& q, ApiResponse& s) {
        const char* ssid = q.body["ssid"] | "";
        const char* dir = q.body["direction"] | "";
        if (strlen(ssid) == 0) {
            s.status = 400;
            s.body["error"] = "ssid required";
            return;
        }
        int8_t direction;
        if (strcmp(dir, "up") == 0)
            direction = -1;
        else if (strcmp(dir, "down") == 0)
            direction = 1;
        else {
            s.status = 400;
            s.body["error"] = "direction must be up or down";
            return;
        }
        if (!Config::moveWifiNetwork(ssid, direction)) {
            s.status = 400;
            s.body["error"] = "cannot move";
            return;
        }
        s.body["ok"] = true;
    }

    // Body: {mac?, deviceName?, ledType?, addWifiNetworks?, apPassword?,
    //        mqttHost?, mqttPort?, mqttUser?, mqttPassword?, githubRepo?, githubToken?,
    //        otaEnabled?}
    // mac omitted or empty = push to all peers. Only present fields are pushed;
    // deviceName and ledType require a specific target mac.
    void _pushConfig(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        const char* macStr = doc["mac"] | "";
        uint8_t targetMac[6] = {0, 0, 0, 0, 0, 0};
        bool hasTarget = macStr[0] != '\0';
        if (hasTarget && !_parseMac(macStr, targetMac)) {
            s.status = 400;
            s.body["error"] = "bad mac";
            return;
        }

        JsonDocument payload;
        bool any = false;

        auto addStr = [&](const char* key, size_t minLen = 0) {
            if (doc[key].isNull()) return;
            const char* v = doc[key] | "";
            if (strlen(v) < minLen) return;
            payload[key] = v;
            any = true;
        };
        auto addNum = [&](const char* key) {
            if (doc[key].isNull()) return;
            payload[key] = doc[key];
            any = true;
        };
        auto addBool = [&](const char* key) {
            if (doc[key].isNull()) return;
            payload[key] = (bool)doc[key];
            any = true;
        };

        // Per-device fields — only allowed when targeting a specific device
        if (hasTarget) {
            if (!doc["deviceName"].isNull()) {
                const char* newName = doc["deviceName"] | "";
                if (newName[0] != '\0') {
                    // Uniqueness check against peer registry and own name
                    if (strcmp(Config::get().deviceName, newName) == 0) {
                        s.status = 409;
                        s.body["error"] = "name already in use";
                        return;
                    }
                    if (_peers) {
                        for (auto& p : *_peers) {
                            if (!p.active || memcmp(p.mac, targetMac, 6) == 0) continue;
                            if (strcmp(p.name, newName) == 0) {
                                s.status = 409;
                                s.body["error"] = "name already in use";
                                return;
                            }
                        }
                    }
                    payload["deviceName"] = newName;
                    any = true;
                }
            }
        }

        // useLocal: fields the UI wants filled from this device's own config
        auto& c = Config::get();
        JsonArrayConst useLocal = doc["useLocal"].as<JsonArrayConst>();
        auto isUseLocal = [&](const char* key) -> bool {
            for (JsonVariantConst v : useLocal)
                if (strcmp(v.as<const char*>(), key) == 0) return true;
            return false;
        };
        // Shared fields
        if (isUseLocal("wifiNetworks")) {
            // Push this device's full wifi list for merge on target
            uint8_t n = Config::wifiCount();
            if (n > 0) {
                JsonArray nets = payload["addWifiNetworks"].to<JsonArray>();
                for (uint8_t i = 0; i < n; i++) {
                    JsonObject o = nets.add<JsonObject>();
                    o["ssid"] = Config::wifiNetworks()[i].ssid;
                    o["password"] = Config::wifiNetworks()[i].password;
                }
                any = true;
            }
        } else if (!doc["addWifiNetworks"].isNull()) {
            payload["addWifiNetworks"] = doc["addWifiNetworks"];
            any = true;
        }
        if (isUseLocal("apPassword")) {
            if (strlen(c.apPassword) >= 8) {
                payload["apPassword"] = c.apPassword;
                any = true;
            }
        } else
            addStr("apPassword", 8);
        addStr("mqttHost");
        addNum("mqttPort");
        addStr("mqttUser");
        if (isUseLocal("mqttPassword")) {
            payload["mqttPassword"] = c.mqttPassword;
            any = true;
        } else
            addStr("mqttPassword", 1);
        addStr("githubRepo");
        if (isUseLocal("githubToken")) {
            payload["githubToken"] = c.githubToken;
            any = true;
        } else
            addStr("githubToken", 1);
        addBool("otaEnabled");

        if (!any) {
            s.status = 400;
            s.body["error"] = "no fields to push";
            return;
        }

        String json;
        serializeJson(payload, json);
        if (_onPushConfig) _onPushConfig(targetMac, json.c_str(), json.length());
        s.body["ok"] = true;
    }

    // Body: {mac, enabled}
    void _setRemoteSceneSync(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        const char* macStr = doc["mac"] | "";
        bool enabled = doc["enabled"] | true;

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            bool prev = Config::get().sceneSyncEnabled;
            Config::get().sceneSyncEnabled = enabled;
            Config::save();
            if (enabled && !prev && _sceneSync) _sceneSync->onSyncEnabled();
            if (enabled != prev && _onSceneSyncChanged) _onSceneSyncChanged();
            s.body["ok"] = true;
            return;
        }

        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            s.status = 400;
            s.body["error"] = "bad mac";
            return;
        }
        if (_onSetRemoteSync) _onSetRemoteSync(mac, enabled);
        s.body["ok"] = true;
    }

    // Body: {enabled}
    void _setWifiPolicy(ApiRequest& q, ApiResponse& s) {
        bool enabled = q.body["enabled"] | false;
        if (_onMeshPolicyChange)
            _onMeshPolicyChange(enabled);
        else {
            Config::get().wifiSingleClientMode = enabled;
            Config::save();
        }
        s.body["ok"] = true;
        _pushPeers();  // wifi icon colors in the device list depend on this flag
    }

    // Shared body for /api/peers/triggerupdate and /api/peers/checkupdate:
    // parse the target MAC, reject if that peer is known but offline, then
    // invoke the given callback.
    void _peerUpdateRequest(ApiRequest& q, ApiResponse& s, const char* logVerb,
                            const std::function<void(const uint8_t*)>& cb) {
        const char* macStr = q.body["mac"] | "";
        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            s.status = 400;
            s.body["error"] = "bad mac";
            return;
        }
        if (_peers) {
            for (auto& p : *_peers) {
                if (p.active && memcmp(p.mac, mac, 6) == 0) {
                    if (!p.online()) {
                        s.status = 409;
                        s.body["error"] = "peer offline";
                        return;
                    }
                    // In single-client mode, an online candidate peer that's
                    // merely on standby (not currently the elected WiFi client)
                    // can still connect on demand for this request (see
                    // WifiElection::requestTemporary) — only reject peers that
                    // are offline, or online but neither connected nor able to
                    // join on demand under the current mesh policy.
                    bool canConnectOnDemand =
                        Config::get().wifiSingleClientMode && p.hasWifiNetworks;
                    if (!p.wifiConnected && !canConnectOnDemand) {
                        s.status = 409;
                        s.body["error"] = "peer not connected to WiFi";
                        return;
                    }
                    break;
                }
            }
        }
        Logger::i("[web] %s for %s", logVerb, macStr);
        if (cb) cb(mac);
        s.body["ok"] = true;
    }

    // ── POST /api/peers/triggerupdate ─────────────────────────────────────────
    void _triggerPeerUpdate(ApiRequest& q, ApiResponse& s) {
        _peerUpdateRequest(q, s, "trigger-update", _onTriggerPeerUpdate);
    }

    // ── POST /api/peers/checkupdate ───────────────────────────────────────────
    void _checkPeerUpdate(ApiRequest& q, ApiResponse& s) {
        _peerUpdateRequest(q, s, "check-update", _onCheckPeerUpdate);
    }

    // ── GET /api/lights ───────────────────────────────────────────────────────
    void _getLights(ApiRequest&, ApiResponse& s) {
        JsonArray arr = s.body["lights"].to<JsonArray>();
        s.body["maxLights"] = MAX_LIGHTS;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = Config::get().lights[i];
            if (!l.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            o["name"] = l.name;
            o["ledType"] = (uint8_t)l.ledType;
            o["colorOrder"] = (uint8_t)l.colorOrder;
            o["dataPin"] = l.dataPin;
            o["clockPin"] = l.clockPin;
            o["width"] = l.width;
            o["height"] = l.height;
            o["matrixStart"] = (uint8_t)l.matrixStart;
            o["matrixDir"] = (uint8_t)l.matrixDir;
            o["matrixSerpentine"] = l.matrixSerpentine;
            o["wrapWidth"] = l.wrapWidth;
            o["wrapHeight"] = l.wrapHeight;
            o["groupId"] = l.groupId;
            o["brightnessOverrideEnabled"] = l.brightnessOverrideEnabled;
            o["brightnessOverride"] = l.brightnessOverride;
        }
    }

    // Returns an error string if the light's active pins collide with each
    // other or with a light/button/sound pin, else nullptr. clockPin only
    // counts as active for WS2801 — WS2812B is single-wire and leaves
    // clockPin unused, so comparing it would produce false conflicts between
    // two WS2812B lights left at the same default clock pin (see
    // Config::isPinInUse). Pass the light's own index as excludeLightIndex
    // when validating an update.
    static const char* _lightPinConflict(const LightHardwareConfig& l, int8_t excludeLightIndex) {
        bool usesClock = l.ledType == LedType::WS2801;
        if (usesClock && l.dataPin == l.clockPin) return "duplicate pin within light config";
        if (Config::isPinInUse(l.dataPin, -1, -1, excludeLightIndex)) return "pin already in use";
        if (usesClock && Config::isPinInUse(l.clockPin, -1, -1, excludeLightIndex))
            return "pin already in use";
        return nullptr;
    }

    // ── POST /api/lights/add ──────────────────────────────────────────────────
    // Body: {ledType, colorOrder, dataPin, clockPin, width, height, matrixStart, matrixDir,
    // matrixSerpentine, wrapWidth, wrapHeight, groupId}
    void _addLight(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        // Find first free slot
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            if (!Config::get().lights[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            s.status = 400;
            s.body["error"] = "light limit reached";
            return;
        }
        LightHardwareConfig l;
        strlcpy(l.name, doc["name"] | "", sizeof(l.name));
        l.ledType = (LedType)(uint8_t)(doc["ledType"] | 0);
        l.colorOrder =
            (ColorOrder)(uint8_t)(doc["colorOrder"] | (uint8_t)defaultColorOrder(l.ledType));
        l.dataPin = doc["dataPin"] | (uint8_t)LED_DATA_PIN;
        l.clockPin = doc["clockPin"] | (uint8_t)LED_CLOCK_PIN;
        l.width = doc["width"] | (uint16_t)1;
        l.height = doc["height"] | (uint16_t)1;
        l.matrixStart = (MatrixStart)(uint8_t)(doc["matrixStart"] | (uint8_t)0);
        l.matrixDir = (MatrixDirection)(uint8_t)(doc["matrixDir"] | (uint8_t)0);
        l.matrixSerpentine = doc["matrixSerpentine"] | false;
        l.wrapWidth = doc["wrapWidth"] | false;
        l.wrapHeight = doc["wrapHeight"] | false;
        l.groupId = doc["groupId"] | (uint8_t)0;
        if (l.width == 0) l.width = 1;
        if (l.height == 0) l.height = 1;
        if (const char* conflict = _lightPinConflict(l, /*excludeLightIndex=*/idx)) {
            s.status = 400;
            s.body["error"] = conflict;
            return;
        }
        l.exists = true;
        Config::get().lights[idx] = l;
        Config::save();
        s.body["ok"] = true;
        s.body["index"] = idx;
        // Hardware config changes require restart to take effect
        s.restart = true;
    }

    // ── POST /api/lights/update ───────────────────────────────────────────────
    // Body: {index, ledType?, colorOrder?, dataPin?, clockPin?, width?, height?, matrixStart?,
    // matrixDir?, matrixSerpentine?, wrapWidth?, wrapHeight?, groupId?}
    void _updateLight(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        auto& l = Config::get().lights[idx];
        bool hwChanged = false;
        if (!doc["name"].isNull()) strlcpy(l.name, doc["name"] | "", sizeof(l.name));

        // ledType/dataPin/clockPin together determine which pins this light
        // actually drives — validate them as a group on a scratch copy first,
        // so a rejected conflict leaves the saved config untouched.
        bool pinsChanged = false;
        LightHardwareConfig pinCandidate = l;
        if (!doc["ledType"].isNull()) {
            pinCandidate.ledType = (LedType)(uint8_t)doc["ledType"];
            hwChanged = true;
            pinsChanged = true;
        }
        if (!doc["dataPin"].isNull()) {
            pinCandidate.dataPin = doc["dataPin"];
            hwChanged = true;
            pinsChanged = true;
        }
        if (!doc["clockPin"].isNull()) {
            pinCandidate.clockPin = doc["clockPin"];
            hwChanged = true;
            pinsChanged = true;
        }
        if (pinsChanged) {
            if (const char* conflict = _lightPinConflict(pinCandidate, (int8_t)idx)) {
                s.status = 400;
                s.body["error"] = conflict;
                return;
            }
            l.ledType = pinCandidate.ledType;
            l.dataPin = pinCandidate.dataPin;
            l.clockPin = pinCandidate.clockPin;
        }

        bool colorOrderChanged = false;
        if (!doc["colorOrder"].isNull()) {
            l.colorOrder = (ColorOrder)(uint8_t)doc["colorOrder"];
            colorOrderChanged = true;
        }
        if (!doc["width"].isNull()) {
            l.width = max((uint16_t)1, (uint16_t)doc["width"]);
            hwChanged = true;
        }
        if (!doc["height"].isNull()) {
            l.height = max((uint16_t)1, (uint16_t)doc["height"]);
            hwChanged = true;
        }
        bool orientationChanged = false;
        if (!doc["matrixStart"].isNull()) {
            l.matrixStart = (MatrixStart)(uint8_t)doc["matrixStart"];
            orientationChanged = true;
        }
        if (!doc["matrixDir"].isNull()) {
            l.matrixDir = (MatrixDirection)(uint8_t)doc["matrixDir"];
            orientationChanged = true;
        }
        if (!doc["matrixSerpentine"].isNull()) {
            l.matrixSerpentine = (bool)doc["matrixSerpentine"];
            orientationChanged = true;
        }
        if (!doc["wrapWidth"].isNull()) {
            l.wrapWidth = (bool)doc["wrapWidth"];
            orientationChanged = true;
        }
        if (!doc["wrapHeight"].isNull()) {
            l.wrapHeight = (bool)doc["wrapHeight"];
            orientationChanged = true;
        }
        // groupId change: soft config, no restart needed
        if (!doc["groupId"].isNull()) {
            uint8_t gid = doc["groupId"];
            if (Config::group(gid)) {
                l.groupId = gid;
                if (_onGroupChange) _onGroupChange();
            }
        }
        // brightness override: soft config, no restart needed
        bool brightnessOverrideChanged = false;
        if (!doc["brightnessOverrideEnabled"].isNull()) {
            l.brightnessOverrideEnabled = (bool)doc["brightnessOverrideEnabled"];
            brightnessOverrideChanged = true;
        }
        if (!doc["brightnessOverride"].isNull()) {
            l.brightnessOverride = (uint8_t)constrain((int)doc["brightnessOverride"], 0, 255);
            brightnessOverrideChanged = true;
        }
        Config::save();
        s.body["ok"] = true;
        if (hwChanged) {
            s.restart = true;
            return;
        }
        if (orientationChanged && _onOrientationChange) _onOrientationChange(idx);
        if (brightnessOverrideChanged && _onLightBrightnessChange) _onLightBrightnessChange(idx);
        if (colorOrderChanged && _onColorOrderChange) _onColorOrderChange(idx);
    }

    // ── POST /api/lights/test ─────────────────────────────────────────────────
    // Body: {index}
    void _testLight(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        if (Config::get().lights[idx].height < 2) {
            s.status = 400;
            s.body["error"] = "not a matrix";
            return;
        }
        if (_onTestLight) _onTestLight(idx);
        s.body["ok"] = true;
    }

    // ── POST /api/lights/testcolor ────────────────────────────────────────────
    // Body: {index} — cycles the light through solid red/green/blue so the
    // user can check colorOrder against the strip's actual wiring. Unlike
    // /api/lights/test (orientation), this isn't restricted to matrix lights.
    void _testColorOrder(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        if (_onTestColorOrder) _onTestColorOrder(idx);
        s.body["ok"] = true;
    }

    // ── POST /api/lights/delete ───────────────────────────────────────────────
    // Body: {index}
    void _deleteLight(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        Config::get().lights[idx].exists = false;
        Config::save();
        s.body["ok"] = true;
        s.restart = true;
    }

    // ── GET /api/sounds ────────────────────────────────────────────────────────
    void _getSounds(ApiRequest&, ApiResponse& s) {
        JsonArray arr = s.body["sounds"].to<JsonArray>();
        s.body["maxSounds"] = MAX_SOUNDS;
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            auto& snd = Config::get().sounds[i];
            if (!snd.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            serializeSound(o, snd);
        }
    }

    // True if the device-wide I2C bus (DeviceConfig::i2cSdaPin/i2cSclPin) has
    // been configured — required before a sound output can be added, or
    // before anything can be routed through the expander.
    static bool _i2cBusConfigured() {
        return Config::get().i2cSdaPin != PIN_UNUSED && Config::get().i2cSclPin != PIN_UNUSED;
    }

    // True if the device's single I2C expander (DeviceConfig::expanderChip)
    // has been configured — required before a sound's PA-enable pin or a
    // button can be routed through it.
    static bool _expanderConfigured() { return Config::get().expanderChip != IoExpanderChip::None; }

    // Returns an error string if any of s's non-unused pins collide with each
    // other or with a light/button/other-sound/device-I2C-bus pin, else
    // nullptr. Pass the sound's own index as excludeSoundIndex when
    // validating an update. paEnablePin is only checked as a real ESP32 GPIO
    // when paViaExpander is false — when true it's a pin index in the
    // device expander's separate address space, checked against other
    // expander-backed pins via Config::isExpanderPinInUse instead.
    static const char* _soundPinConflict(const SoundHardwareConfig& s, int8_t excludeSoundIndex) {
        uint8_t pins[] = {s.i2sMclkPin, s.i2sBclkPin, s.i2sWsPin, s.i2sDoutPin, s.paEnablePin};
        size_t count = s.paViaExpander ? 4 : 5;
        for (size_t i = 0; i < count; i++) {
            if (pins[i] == PIN_UNUSED) continue;
            for (size_t j = i + 1; j < count; j++) {
                if (pins[j] == pins[i]) return "duplicate pin within sound config";
            }
            if (Config::isPinInUse(pins[i], -1, excludeSoundIndex)) return "pin already in use";
        }
        if (s.paViaExpander && s.paEnablePin != PIN_UNUSED) {
            if (!_expanderConfigured()) return "configure the device I2C expander first";
            if (Config::isExpanderPinInUse(s.paEnablePin, -1, /*excludeSoundPa=*/true))
                return "expander pin already in use";
        }
        return nullptr;
    }

    // ── POST /api/sounds/add ──────────────────────────────────────────────────
    // Body: {name?, chip?, i2cAddress?, i2sMclkPin?, i2sBclkPin, i2sWsPin, i2sDoutPin,
    // paEnablePin?, paEnableActiveHigh?, paViaExpander?}
    void _addSound(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        if (!_i2cBusConfigured()) {
            s.status = 400;
            s.body["error"] = "configure the device I2C bus in Hardware settings first";
            return;
        }
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            if (!Config::get().sounds[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            s.status = 400;
            s.body["error"] = "sound limit reached";
            return;
        }
        SoundHardwareConfig snd;
        deserializeSound(doc, snd);
        if (snd.i2sBclkPin == PIN_UNUSED || snd.i2sWsPin == PIN_UNUSED ||
            snd.i2sDoutPin == PIN_UNUSED) {
            s.status = 400;
            s.body["error"] = "missing required pin";
            return;
        }
        if (const char* conflict = _soundPinConflict(snd, /*excludeSoundIndex=*/idx)) {
            s.status = 400;
            s.body["error"] = conflict;
            return;
        }
        snd.exists = true;
        Config::get().sounds[idx] = snd;
        Config::save();
        s.body["ok"] = true;
        s.body["index"] = idx;
        // Hardware config changes require restart to take effect
        s.restart = true;
    }

    // ── POST /api/sounds/update ─────────────────────────────────────────────────
    // Body: {index, name?, chip?, i2cAddress?, i2sMclkPin?, i2sBclkPin?, i2sWsPin?,
    // i2sDoutPin?, paEnablePin?, paEnableActiveHigh?, paViaExpander?}
    void _updateSound(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        auto& existing = Config::get().sounds[idx];
        if (!doc["name"].isNull()) strlcpy(existing.name, doc["name"] | "", sizeof(existing.name));

        // Every other field is hardware config — apply on top of a copy first so a
        // rejected pin conflict leaves the saved config untouched.
        SoundHardwareConfig candidate = existing;
        bool hwChanged = false;
        if (!doc["chip"].isNull()) {
            candidate.chip = (SoundChip)(uint8_t)doc["chip"];
            hwChanged = true;
        }
        if (!doc["i2cAddress"].isNull()) {
            candidate.i2cAddress = doc["i2cAddress"];
            hwChanged = true;
        }
        if (!doc["i2sMclkPin"].isNull()) {
            candidate.i2sMclkPin = doc["i2sMclkPin"];
            hwChanged = true;
        }
        if (!doc["i2sBclkPin"].isNull()) {
            candidate.i2sBclkPin = doc["i2sBclkPin"];
            hwChanged = true;
        }
        if (!doc["i2sWsPin"].isNull()) {
            candidate.i2sWsPin = doc["i2sWsPin"];
            hwChanged = true;
        }
        if (!doc["i2sDoutPin"].isNull()) {
            candidate.i2sDoutPin = doc["i2sDoutPin"];
            hwChanged = true;
        }
        if (!doc["paEnablePin"].isNull()) {
            candidate.paEnablePin = doc["paEnablePin"];
            hwChanged = true;
        }
        if (!doc["paEnableActiveHigh"].isNull()) {
            candidate.paEnableActiveHigh = (bool)doc["paEnableActiveHigh"];
            hwChanged = true;
        }
        if (!doc["paViaExpander"].isNull()) {
            candidate.paViaExpander = (bool)doc["paViaExpander"];
            hwChanged = true;
        }

        if (hwChanged) {
            if (candidate.i2sBclkPin == PIN_UNUSED || candidate.i2sWsPin == PIN_UNUSED ||
                candidate.i2sDoutPin == PIN_UNUSED) {
                s.status = 400;
                s.body["error"] = "missing required pin";
                return;
            }
            if (const char* conflict = _soundPinConflict(candidate, /*excludeSoundIndex=*/idx)) {
                s.status = 400;
                s.body["error"] = conflict;
                return;
            }
            candidate.exists = true;
            existing = candidate;
        }
        Config::save();
        s.body["ok"] = true;
        if (hwChanged) s.restart = true;
    }

    // ── POST /api/sounds/delete ───────────────────────────────────────────────
    // Body: {index}
    void _deleteSound(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        Config::get().sounds[idx].exists = false;
        Config::save();
        s.body["ok"] = true;
        s.restart = true;
    }

    // ── POST /api/sounds/test ─────────────────────────────────────────────────
    // Body: {index} — plays the built-in hardware-verification melody.
    void _testSound(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        if (_onTestSound) _onTestSound(idx);
        s.body["ok"] = true;
    }

    // ── Storage (SD card) ────────────────────────────────────────────────────
    // Auto-detected hardware (see SdCardManager) — unlike sounds/lights/buttons
    // there's no per-device config to add/edit, just status + file management.

    // Bare filename only (no path separators), non-empty, ending in ".wav" —
    // see src/storage/README.md for why uploads are restricted to that format.
    static bool _isValidWavName(const String& name) {
        if (name.length() < 5 || name.indexOf('/') != -1 || name.indexOf('\\') != -1) return false;
        String lower = name;
        lower.toLowerCase();
        return lower.endsWith(".wav");
    }

    // ── GET /api/storage ─────────────────────────────────────────────────────
    void _getStorage(ApiRequest&, ApiResponse& s) {
        s.body["hwSupported"] = SdCardManager::kHwSupported;
        bool present = _sdCard && _sdCard->present();
        s.body["present"] = present;
        s.body["totalBytes"] = present ? _sdCard->totalBytes() : 0;
        s.body["usedBytes"] = present ? _sdCard->usedBytes() : 0;
        // Only list files this API can actually manage (see _isValidWavName) —
        // the card's root can otherwise hold arbitrary pre-existing files (a
        // prior recording, OS-created metadata from formatting the card on a
        // computer, ...) that would show up here but always 400 on delete.
        JsonArray files = s.body["files"].to<JsonArray>();
        if (present) {
            _sdCard->forEachFile([&](const String& name, size_t size) {
                if (!_isValidWavName(name)) return;
                JsonObject o = files.add<JsonObject>();
                o["name"] = name;
                o["size"] = size;
            });
        }
    }

    // ── POST /api/storage/upload?name=<file.wav> ────────────────────────────
    // Streaming: raw file bytes written straight to the SD card as they
    // arrive, not buffered — audio files are too large for that, unlike the
    // scene-save JSON above.
    void _storageUpload(ApiRequest& q, ApiResponse& s) {
        auto* st = static_cast<StorageUploadState*>(q.streamState);
        s.streamDone = false;
        if (!st) {
            st = new StorageUploadState();
            q.streamState = st;

            String name = q.query ? q.query : "";
            if (!_isValidWavName(name)) {
                Logger::w("[storage] upload: rejected filename %s", name.c_str());
                st->failed = true;
                st->error = "invalid filename";
            } else if (!_sdCard || !_sdCard->present()) {
                st->failed = true;
                st->error = "no SD card";
            } else {
                st->file = _sdCard->openForWrite(name);
                if (!st->file) {
                    Logger::e("[storage] upload: open failed for %s", name.c_str());
                    st->failed = true;
                    st->error = "open failed";
                } else {
                    Logger::i("[storage] upload: %s", name.c_str());
                }
            }
        }

        if (!st->failed && q.chunkLen) {
            size_t written = st->file.write(q.chunk, q.chunkLen);
            if (written != q.chunkLen) {
                Logger::e("[storage] upload: chunk write incomplete (%u/%u) at index=%u",
                          (unsigned)written, (unsigned)q.chunkLen, (unsigned)q.chunkIndex);
                st->failed = true;
                st->error = "write failed";
                st->file.close();
                st->file = File();
            }
        }

        if (q.chunkIndex + q.chunkLen < q.chunkTotal) return;  // more chunks coming

        if (st->file) st->file.close();
        s.streamDone = true;
        if (st->failed) {
            s.status = 400;
            s.body["error"] = st->error ? st->error : "upload failed";
        } else {
            s.body["ok"] = true;
        }
        delete st;
        q.streamState = nullptr;
    }

    // ── POST /api/storage/delete ─────────────────────────────────────────────
    // Body: {name}
    void _deleteStorageFile(ApiRequest& q, ApiResponse& s) {
        String name = q.body["name"] | "";
        if (!_isValidWavName(name)) {
            s.status = 400;
            s.body["error"] = "invalid filename";
            return;
        }
        if (!_sdCard || !_sdCard->deleteFile(name)) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        s.body["ok"] = true;
    }

    // ── GET /api/buttons ──────────────────────────────────────────────────────
    void _getButtons(ApiRequest&, ApiResponse& s) {
        s.body["maxButtons"] = MAX_BUTTONS;
        JsonArray arr = s.body["buttons"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            auto& b = Config::get().buttons[i];
            if (!b.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            serializeButton(o, b);
        }
    }

    // Returns an error string if b's pin collides with another configured
    // pin, else nullptr. Pass the button's own index as excludeButtonIndex
    // when validating an update. b.pin is checked as a real ESP32 GPIO only
    // when viaExpander is false — when true it's a pin index in the device
    // expander's separate address space, checked against other
    // expander-backed pins via Config::isExpanderPinInUse instead.
    static const char* _buttonPinConflict(const ButtonHardwareConfig& b,
                                          int8_t excludeButtonIndex) {
        if (b.viaExpander) {
            if (!_expanderConfigured()) return "configure the device I2C expander first";
            if (Config::isExpanderPinInUse(b.pin, excludeButtonIndex))
                return "expander pin already in use";
            return nullptr;
        }
        if (Config::isPinInUse(b.pin, excludeButtonIndex)) return "pin already in use";
        return nullptr;
    }

    // ── POST /api/buttons/add ─────────────────────────────────────────────────
    // Body: {name?, pin, activeLow?, viaExpander?, onShortPress?, onLongPress?,
    // onDoubleClick?}
    void _addButton(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            if (!Config::get().buttons[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            s.status = 400;
            s.body["error"] = "button limit reached";
            return;
        }
        ButtonHardwareConfig b;
        deserializeButton(doc, b);
        if (const char* conflict = _buttonPinConflict(b, /*excludeButtonIndex=*/idx)) {
            s.status = 400;
            s.body["error"] = conflict;
            return;
        }
        b.exists = true;  // deserializeButton defaults "exists" to false when the key is absent
        Config::get().buttons[idx] = b;
        Config::save();
        s.body["ok"] = true;
        s.body["index"] = idx;
        if (_onButtonsChanged) _onButtonsChanged();
    }

    // ── POST /api/buttons/update ──────────────────────────────────────────────
    // Body: {index, name?, pin?, activeLow?, viaExpander?, onShortPress?, onLongPress?,
    // onDoubleClick?}
    void _updateButton(ApiRequest& q, ApiResponse& s) {
        JsonVariantConst doc = q.body;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_BUTTONS || !Config::get().buttons[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        auto& existing = Config::get().buttons[idx];
        if (!doc["name"].isNull()) strlcpy(existing.name, doc["name"] | "", sizeof(existing.name));

        // pin/viaExpander together decide the pin's address space — apply on
        // top of a copy first so a rejected conflict leaves the saved config
        // untouched, mirroring _updateSound's candidate/hwChanged pattern.
        ButtonHardwareConfig candidate = existing;
        bool pinChanged = false;
        if (!doc["activeLow"].isNull()) candidate.activeLow = (bool)doc["activeLow"];
        if (!doc["pin"].isNull()) {
            candidate.pin = doc["pin"];
            pinChanged = true;
        }
        if (!doc["viaExpander"].isNull()) {
            candidate.viaExpander = (bool)doc["viaExpander"];
            pinChanged = true;
        }
        if (pinChanged) {
            if (const char* conflict = _buttonPinConflict(candidate, (int8_t)idx)) {
                s.status = 400;
                s.body["error"] = conflict;
                return;
            }
        }
        existing = candidate;
        if (!doc["onShortPress"].isNull())
            existing.onShortPress =
                deserializeButtonAction(doc["onShortPress"], existing.onShortPress);
        if (!doc["onLongPress"].isNull())
            existing.onLongPress =
                deserializeButtonAction(doc["onLongPress"], existing.onLongPress);
        if (!doc["onDoubleClick"].isNull())
            existing.onDoubleClick =
                deserializeButtonAction(doc["onDoubleClick"], existing.onDoubleClick);
        Config::save();
        s.body["ok"] = true;
        if (_onButtonsChanged) _onButtonsChanged();
    }

    // ── POST /api/buttons/delete ──────────────────────────────────────────────
    // Body: {index}
    void _deleteButton(ApiRequest& q, ApiResponse& s) {
        uint8_t idx = q.body["index"] | (uint8_t)0xFF;
        if (idx >= MAX_BUTTONS || !Config::get().buttons[idx].exists) {
            s.status = 404;
            s.body["error"] = "not found";
            return;
        }
        Config::get().buttons[idx].exists = false;
        Config::save();
        s.body["ok"] = true;
        if (_onButtonsChanged) _onButtonsChanged();
    }

    static String _jsonStr(const char* s) {
        String o = "\"";
        for (; *s; ++s) {
            if (*s == '"')
                o += "\\\"";
            else if (*s == '\\')
                o += "\\\\";
            else if (*s == '\n')
                o += "\\n";
            else
                o += *s;
        }
        return o + '"';
    }
};
