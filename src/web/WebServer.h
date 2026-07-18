#pragma once
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include <functional>

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

        // API routes must be registered before serveStatic, otherwise the
        // static handler matches /api/* paths and tries to open them from LittleFS.
        _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* r) { _getConfig(r); });
        _server.on(
            "/api/config", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _postConfig(r, d, l);
            });

        _server.on("/api/mqtt/clear", HTTP_POST,
                   [this](AsyncWebServerRequest* r) { _clearMqtt(r); });

        _server.on("/api/wifi", HTTP_GET, [this](AsyncWebServerRequest* r) { _getWifi(r); });
        _server.on(
            "/api/wifi/add", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _addWifi(r, d, l);
            });
        _server.on(
            "/api/wifi/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteWifi(r, d, l);
            });
        _server.on(
            "/api/wifi/move", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _moveWifi(r, d, l);
            });

        _server.on("/api/peers", HTTP_GET, [this](AsyncWebServerRequest* r) { _getPeers(r); });

        _server.on(
            "/api/groups/create", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _createGroup(r, d, l);
            });
        _server.on(
            "/api/groups/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateGroup(r, d, l);
            });
        _server.on(
            "/api/groups/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteGroup(r, d, l);
            });

        _server.on(
            "/api/peers/setgroup", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _setRemoteGroup(r, d, l);
            });

        _server.on("/api/lights", HTTP_GET, [this](AsyncWebServerRequest* r) { _getLights(r); });
        _server.on(
            "/api/lights/add", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _addLight(r, d, l);
            });
        _server.on(
            "/api/lights/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateLight(r, d, l);
            });
        _server.on(
            "/api/lights/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteLight(r, d, l);
            });
        _server.on(
            "/api/lights/test", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _testLight(r, d, l);
            });
        _server.on(
            "/api/lights/testcolor", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _testColorOrder(r, d, l);
            });

        _server.on("/api/sounds", HTTP_GET, [this](AsyncWebServerRequest* r) { _getSounds(r); });
        _server.on(
            "/api/sounds/add", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _addSound(r, d, l);
            });
        _server.on(
            "/api/sounds/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateSound(r, d, l);
            });
        _server.on(
            "/api/sounds/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteSound(r, d, l);
            });
        _server.on(
            "/api/sounds/test", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _testSound(r, d, l);
            });

        _server.on("/api/storage", HTTP_GET, [this](AsyncWebServerRequest* r) { _getStorage(r); });
        _server.on(
            "/api/storage/upload", HTTP_POST,
            [this](AsyncWebServerRequest* r) { _finishStorageUpload(r); }, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t index, size_t total) {
                _storageUploadChunk(r, d, l, index, total);
            });
        _server.on(
            "/api/storage/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteStorageFile(r, d, l);
            });

        _server.on("/api/buttons", HTTP_GET, [this](AsyncWebServerRequest* r) { _getButtons(r); });
        _server.on(
            "/api/buttons/add", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _addButton(r, d, l);
            });
        _server.on(
            "/api/buttons/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateButton(r, d, l);
            });
        _server.on(
            "/api/buttons/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteButton(r, d, l);
            });

        // ── Scene API ─────────────────────────────────────────────────────────
        // Specific routes must be registered before /api/scenes because
        // ESPAsyncWebServer prefix-matches: /api/scenes would otherwise
        // intercept /api/scenes/get, /api/scenes/save, etc.
        SceneManager::init();

        _server.on("/api/scenes/get", HTTP_GET, [this](AsyncWebServerRequest* r) { _getScene(r); });

        _server.on(
            "/api/scenes/create", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _createScene(r, d, l);
            });

        _server.on(
            "/api/scenes/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteScene(r, d, l);
            });

        // Scene save buffers the full body (may span multiple TCP packets)
        // before writing to LittleFS.
        _server.on(
            "/api/scenes/save", HTTP_POST,
            [this](AsyncWebServerRequest* r) {
                auto* st = static_cast<SceneSaveState*>(r->_tempObject);
                if (!st) {
                    Logger::e("[scene] save: request handler fired with no state");
                    r->send(400, "application/json", "{\"error\":\"no body\"}");
                    return;
                }

                bool ok = !st->failed && st->written;
                if (st->file) st->file.close();

                if (ok) {
                    Logger::i("[scene] save ok: %s", st->id.c_str());
                    if (_sceneSync) _sceneSync->onSceneChanged(st->id.c_str(), st->prevHash);
                    if (_onSceneSaved) _onSceneSaved(st->id.c_str());
                } else {
                    Logger::e("[scene] save failed: %s (failed=%d written=%d)",
                              st->error ? st->error : "?", st->failed, st->written);
                }

                JsonDocument resp;
                if (ok)
                    resp["ok"] = true;
                else
                    resp["error"] =
                        st->failed ? (st->error ? st->error : "save failed") : "save failed";

                delete st;
                r->_tempObject = nullptr;
                _sendJson(r, ok ? 200 : 500, resp);
            },
            nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index,
                   size_t total) {
                auto* st = static_cast<SceneSaveState*>(r->_tempObject);
                if (!st) {
                    st = new SceneSaveState();
                    r->_tempObject = st;
                    Logger::d("[scene] save: body start, total=%u bytes", (unsigned)total);
                }

                if (st->failed || !len) return;

                if (!st->file) {
                    st->buffer.concat((char*)data, len);
                    Logger::d("[scene] save: buffering chunk %u bytes (buf=%u): %.80s",
                              (unsigned)len, (unsigned)st->buffer.length(), st->buffer.c_str());
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
                                (unsigned)st->buffer.length(), (unsigned)total);
                        }
                        return;
                    }

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
                        return;
                    }

                    size_t written =
                        st->file.write((const uint8_t*)st->buffer.c_str(), st->buffer.length());
                    Logger::d("[scene] save: wrote initial buffer %u/%u bytes", (unsigned)written,
                              (unsigned)st->buffer.length());
                    if (written != st->buffer.length()) {
                        Logger::e("[scene] save: initial write incomplete (%u/%u)",
                                  (unsigned)written, (unsigned)st->buffer.length());
                        st->failed = true;
                        st->error = "write failed";
                        st->file.close();
                        st->file = File();
                        return;
                    }

                    st->buffer = "";
                    st->written = true;
                    return;
                }

                size_t written = st->file.write(data, len);
                Logger::d("[scene] save: chunk at index=%u len=%u written=%u", (unsigned)index,
                          (unsigned)len, (unsigned)written);
                if (written != len) {
                    Logger::e("[scene] save: chunk write incomplete (%u/%u) at index=%u",
                              (unsigned)written, (unsigned)len, (unsigned)index);
                    st->failed = true;
                    st->error = "write failed";
                    st->file.close();
                    st->file = File();
                    return;
                }
                st->written = true;
            });

        _server.on("/api/scenes", HTTP_GET, [this](AsyncWebServerRequest* r) { _getScenes(r); });

        // ── Scene sync API ────────────────────────────────────────────────────
        _server.on("/api/scenes/sync/conflicts", HTTP_GET,
                   [this](AsyncWebServerRequest* r) { _getSyncConflicts(r); });

        _server.on(
            "/api/scenes/sync/resolve", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _resolveSyncConflict(r, d, l);
            });

        _server.on(
            "/api/peers/setscenesync", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _setRemoteSceneSync(r, d, l);
            });

        _server.on(
            "/api/peers/pushconfig", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _pushConfig(r, d, l);
            });

        _server.on(
            "/api/peers/triggerupdate", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _triggerPeerUpdate(r, d, l);
            });

        _server.on(
            "/api/peers/checkupdate", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _checkPeerUpdate(r, d, l);
            });

        _server.on("/api/update/trigger", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::triggerAsync(); });
            else
                Updater::triggerAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/update/status", HTTP_GET, [](AsyncWebServerRequest* r) {
            auto& s = Updater::status();
            JsonDocument doc;
            doc["currentVersion"] = s.currentVersion;
            doc["latestVersion"] = s.latestVersion;
            doc["hasUpdate"] = s.hasUpdate;
            doc["progress"] = s.progress;
            doc["state"] = _fwStateToString(s.state);
            if (s.error) doc["error"] = s.error;
            String out;
            serializeJson(doc, out);
            r->send(200, "application/json", out);
        });

        _server.on("/api/update/check", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::checkAsync(); });
            else
                Updater::checkAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/update/apply", HTTP_POST, [this](AsyncWebServerRequest* r) {
            auto& s = Updater::status();
            if (!s.hasUpdate) {
                r->send(400, "application/json", "{\"error\":\"no update available\"}");
                return;
            }
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::applyAsync(); });
            else
                Updater::applyAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // Experimental "install from PR" flow — every endpoint below requires
        // Config::prOtaEnabled, so it costs nothing unless explicitly opted in.
        _server.on("/api/update/prs/refresh", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (!Config::get().prOtaEnabled) {
                r->send(403, "application/json", "{\"error\":\"PR installs are disabled\"}");
                return;
            }
            if (_onRequestWifi)
                _onRequestWifi([]() { Updater::listPrBuildsAsync(); });
            else
                Updater::listPrBuildsAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/update/prs", HTTP_GET, [](AsyncWebServerRequest* r) {
            auto& s = Updater::prListStatus();
            JsonDocument doc;
            doc["state"] = _prListStateToString(s.state);
            JsonArray arr = doc["prs"].to<JsonArray>();
            for (auto& b : s.builds) {
                JsonObject o = arr.add<JsonObject>();
                o["number"] = b.number;
                o["title"] = b.title;
                o["tag"] = b.tag;
            }
            if (s.error) doc["error"] = s.error;
            String out;
            serializeJson(doc, out);
            r->send(200, "application/json", out);
        });

        _server.on(
            "/api/update/apply-pr", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                if (!Config::get().prOtaEnabled) {
                    r->send(403, "application/json", "{\"error\":\"PR installs are disabled\"}");
                    return;
                }
                JsonDocument doc;
                if (!_parseJson(r, doc, d, l)) return;
                String tag = doc["tag"] | "";
                if (_onRequestWifi)
                    _onRequestWifi([tag]() { Updater::applyPrAsync(tag); });
                else
                    Updater::applyPrAsync(tag);
                r->send(200, "application/json", "{\"ok\":true}");
            });

        _server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
            Config::reset();
            r->send(200, "application/json", "{\"ok\":true}");
            delay(500);
            ESP.restart();
        });

        _server.on("/api/mesh/search", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (_onMeshSearch) _onMeshSearch();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // Body: {enabled}. Runtime-safe, mesh-wide toggle — no reboot, applies
        // to this device immediately and broadcasts to peers (see WifiElection).
        _server.on(
            "/api/mesh/wifipolicy", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _setWifiPolicy(r, d, l);
            });

        // Manual "retry WiFi now" — applies locally and broadcasts to every
        // peer, so a mesh where every candidate gave up doesn't need the
        // mode toggled off and back on to try again.
        _server.on("/api/mesh/wifiretry", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (_onWifiRetry) _onWifiRetry();
            r->send(200, "application/json", "{\"ok\":true}");
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

   private:
    AsyncWebServer _server{80};
    AsyncWebSocket* _ws = nullptr;
    RequestLogger _reqLogger;
    PeerRegistry* _peers = nullptr;
    ChannelManager* _channelMgr = nullptr;
    SdCardManager* _sdCard = nullptr;

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

    // ── helpers ──────────────────────────────────────────────────────────────

    static JsonDocument _makeOk() {
        JsonDocument d;
        d["ok"] = true;
        return d;
    }
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

    // Parses the request body into doc, sending a 400 "bad json" response on failure.
    // logCtx, if given, logs "[scene] <logCtx>: bad json" before responding.
    static bool _parseJson(AsyncWebServerRequest* r, JsonDocument& doc, uint8_t* data, size_t len,
                           const char* logCtx = nullptr) {
        if (deserializeJson(doc, data, len)) {
            if (logCtx) Logger::e("[scene] %s: bad json", logCtx);
            auto e = _makeErr("bad json");
            _sendJson(r, 400, e);
            return false;
        }
        return true;
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
    void _getConfig(AsyncWebServerRequest* r) {
        auto& c = Config::get();
        JsonDocument doc;
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
        _sendJson(r, 200, doc);
    }

    // ── POST /api/config ─────────────────────────────────────────────────────
    void _postConfig(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        auto& c = Config::get();
        if (!doc["deviceName"].isNull())
            strlcpy(c.deviceName, doc["deviceName"], sizeof(c.deviceName));
        if (!doc["apPassword"].isNull())
            strlcpy(c.apPassword, doc["apPassword"], sizeof(c.apPassword));
        if (!doc["otaPort"].isNull()) c.otaPort = doc["otaPort"];
        if (!doc["otaEnabled"].isNull()) c.otaEnabled = (bool)doc["otaEnabled"];
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
        if (!doc["batteryMonitoringEnabled"].isNull())
            c.batteryMonitoringEnabled = (bool)doc["batteryMonitoringEnabled"];
        if (!doc["prOtaEnabled"].isNull()) c.prOtaEnabled = (bool)doc["prOtaEnabled"];
        // wifiSingleClientMode is intentionally not handled here — it's a
        // runtime-safe, mesh-wide toggle exposed from the device list instead
        // (POST /api/mesh/wifipolicy), so flipping it doesn't force the
        // "save settings" reboot that every other field here triggers.
        if (!doc["mqttHost"].isNull()) strlcpy(c.mqttHost, doc["mqttHost"], sizeof(c.mqttHost));
        if (!doc["mqttPort"].isNull()) c.mqttPort = (uint16_t)doc["mqttPort"];
        if (!doc["mqttUser"].isNull()) strlcpy(c.mqttUser, doc["mqttUser"], sizeof(c.mqttUser));
        if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0)
            strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
        if (!doc["githubRepo"].isNull())
            strlcpy(c.githubRepo, doc["githubRepo"], sizeof(c.githubRepo));
        if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
            strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));
        if (!doc["timezone"].isNull()) strlcpy(c.timezone, doc["timezone"], sizeof(c.timezone));

        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        delay(200);
        ESP.restart();
    }

    // ── POST /api/mqtt/clear ─────────────────────────────────────────────────
    // Removes the MQTT broker config and clears every retained message this
    // device may have published (state, discovery, telemetry) — a plain
    // /api/config save with an emptied mqttHost would leave all of that
    // stale on the broker forever, since nothing else ever cleans it up.
    // Runtime-safe, like the dedicated /api/mesh/wifipolicy endpoint — no
    // reboot needed, MqttManager just disables itself for the rest of this session.
    void _clearMqtt(AsyncWebServerRequest* r) {
        if (_onClearMqtt) _onClearMqtt();
        auto& c = Config::get();
        c.mqttHost[0] = '\0';
        c.mqttPort = 1883;
        c.mqttUser[0] = '\0';
        c.mqttPassword[0] = '\0';
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── GET /api/peers ───────────────────────────────────────────────────────
    void _getPeers(AsyncWebServerRequest* r) {
        JsonDocument doc;
        _buildPeersJson(doc);
        _sendJson(r, 200, doc);
    }

    void _buildPeersJson(JsonDocument& doc) {
        auto& c = Config::get();
        const auto& us = Updater::status();

        doc["wifiSingleClientMode"] = c.wifiSingleClientMode;

        auto self = doc["self"].to<JsonObject>();
        self["mac"] = WiFi.macAddress();
        self["name"] = c.deviceName;
        self["online"] = true;
        self["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
        self["hasWifiNetworks"] = Config::wifiCount() > 0;
        self["wifiConnecting"] = _onWifiAttempting && _onWifiAttempting();
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
    void _createGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* name = doc["name"] | "New Group";
        uint8_t id = Config::createGroup(name);
        if (id == 0xFF) {
            auto e = _makeErr("group limit reached");
            _sendJson(r, 400, e);
            return;
        }
        const GroupConfig& g = Config::get().groups[id];
        Config::save();
        if (_onGroupSync) _onGroupSync(g);

        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        _sendJson(r, 200, resp);
        _pushGroups();
    }

    // ── POST /api/groups/update ──────────────────────────────────────────────
    // Body: {id, name?, pattern?, r?, g?, b?, brightness?, speed?}
    void _updateGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t id = doc["id"] | (uint8_t)0;
        GroupConfig* g = Config::group(id);
        if (!g) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
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
            auto ok = _makeOk();
            _sendJson(r, 200, ok);
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
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        if (nameChanged) _pushGroups();
    }

    // ── POST /api/groups/delete ──────────────────────────────────────────────
    void _deleteGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t id = doc["id"] | (uint8_t)0;
        if (id == 0) {
            auto e = _makeErr("cannot delete Default");
            _sendJson(r, 400, e);
            return;
        }
        GroupConfig* g = Config::group(id);
        if (!g) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
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
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        _pushGroups();
    }

    // ── POST /api/peers/setgroup ─────────────────────────────────────────────
    // Body: {mac, lightIndex, groupId}
    void _setRemoteGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t lightIndex = doc["lightIndex"] | (uint8_t)0;
        uint8_t groupId = doc["groupId"] | (uint8_t)0;
        const char* macStr = doc["mac"] | "";

        if (lightIndex >= MAX_LIGHTS) {
            auto e = _makeErr("invalid lightIndex");
            _sendJson(r, 400, e);
            return;
        }

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            if (Config::group(groupId)) {
                Config::get().lights[lightIndex].groupId = groupId;
                Config::save();
                if (_onGroupChange) _onGroupChange();
            }
            auto ok = _makeOk();
            _sendJson(r, 200, ok);
            _pushPeers();
            return;
        }

        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            auto e = _makeErr("bad mac");
            _sendJson(r, 400, e);
            return;
        }

        if (_onSetRemote) _onSetRemote(mac, lightIndex, groupId);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
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

    void _getScenes(AsyncWebServerRequest* r) {
        JsonDocument resp;
        SceneManager::buildList(resp);
        JsonArray arr = resp["scenes"].as<JsonArray>();
        Logger::d("[scene] list: %u scene(s)", arr ? (unsigned)arr.size() : 0);
        _sendJson(r, 200, resp);
    }

    void _getScene(AsyncWebServerRequest* r) {
        if (!r->hasParam("id")) {
            Logger::w("[scene] get: missing id param");
            auto e = _makeErr("missing id");
            _sendJson(r, 400, e);
            return;
        }
        String id = r->getParam("id")->value();
        String path = SceneManager::path(id.c_str());
        Logger::d("[scene] get: id=%s path=%s exists=%d", id.c_str(), path.c_str(),
                  LittleFS.exists(path));
        if (!LittleFS.exists(path)) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        r->send(LittleFS, path, "application/json");
    }

    void _createScene(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len, "create")) return;
        const char* name = doc["name"] | "Unnamed";
        uint16_t w = doc["w"] | 20;
        uint16_t h = doc["h"] | 10;
        Logger::i("[scene] create: name=%s w=%u h=%u", name, w, h);
        String id = SceneManager::create(name, w, h);
        if (id.isEmpty()) {
            Logger::e("[scene] create: failed");
            auto e = _makeErr("create failed");
            _sendJson(r, 500, e);
            return;
        }
        Logger::i("[scene] create: ok id=%s", id.c_str());
        if (_onSceneListChanged) _onSceneListChanged();
        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        _sendJson(r, 200, resp);
    }

    void _deleteScene(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len, "delete")) return;
        const char* id = doc["id"] | "";
        if (!id[0]) {
            Logger::w("[scene] delete: missing id");
            auto e = _makeErr("missing id");
            _sendJson(r, 400, e);
            return;
        }
        Logger::i("[scene] delete: id=%s", id);
        bool ok = _sceneSync ? _sceneSync->deleteScene(id) : SceneManager::remove(id);
        Logger::i("[scene] delete: %s", ok ? "ok" : "not found");
        if (ok && _onSceneListChanged) _onSceneListChanged();
        JsonDocument resp;
        if (ok)
            resp["ok"] = true;
        else
            resp["error"] = "not found";
        _sendJson(r, ok ? 200 : 404, resp);
    }

    // ── Scene sync handlers ──────────────────────────────────────────────────

    void _getSyncConflicts(AsyncWebServerRequest* r) {
        JsonDocument doc;
        if (_sceneSync) {
            _sceneSync->buildConflictsJson(doc);
            _sceneSync->buildPeerScenesJson(doc);
        } else {
            doc["conflicts"].to<JsonArray>();
            doc["peerScenes"].to<JsonArray>();
        }
        _sendJson(r, 200, doc);
    }

    // Body: {id, sourceMac}  — sourceMac is the device whose copy wins.
    // Use sourceMac == own MAC or omit to use local copy.
    void _resolveSyncConflict(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* id = doc["id"] | "";
        const char* macStr = doc["sourceMac"] | "";
        if (!id[0]) {
            auto e = _makeErr("missing id");
            _sendJson(r, 400, e);
            return;
        }

        // Determine if sourceMac is this device or a remote one
        bool isLocal = !macStr[0] || WiFi.macAddress().equalsIgnoreCase(macStr);

        if (isLocal) {
            if (_onResolveConflict) _onResolveConflict(id, nullptr);
        } else {
            uint8_t mac[6];
            if (!_parseMac(macStr, mac)) {
                auto e = _makeErr("bad mac");
                _sendJson(r, 400, e);
                return;
            }
            if (_onResolveConflict) _onResolveConflict(id, mac);
        }
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── GET /api/wifi ─────────────────────────────────────────────────────────
    void _getWifi(AsyncWebServerRequest* r) {
        JsonDocument doc;
        if (WiFi.status() == WL_CONNECTED) {
            // Use Config::wifiLast() rather than WiFi.SSID() to avoid calling
            // esp_wifi_sta_get_ap_info() from an async handler — that call can
            // block waiting for the WiFi driver lock during reconnect events.
            uint8_t last = Config::wifiLast();
            doc["connected"] = (last < Config::wifiCount())
                                   ? (const char*)Config::wifiNetworks()[last].ssid
                                   : (const char*)nullptr;
        } else {
            doc["connected"] = nullptr;
        }
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (uint8_t i = 0; i < Config::wifiCount(); i++) arr.add(Config::wifiNetworks()[i].ssid);
        _sendJson(r, 200, doc);
    }

    // ── POST /api/wifi/add ────────────────────────────────────────────────────
    // Body: {ssid, password}
    void _addWifi(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* ssid = doc["ssid"] | "";
        const char* pass = doc["password"] | "";
        if (strlen(ssid) == 0) {
            auto e = _makeErr("ssid required");
            _sendJson(r, 400, e);
            return;
        }
        if (!Config::addWifiNetwork(ssid, pass)) {
            auto e = _makeErr("network list full");
            _sendJson(r, 409, e);
            return;
        }
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/wifi/delete ─────────────────────────────────────────────────
    // Body: {ssid}
    void _deleteWifi(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* ssid = doc["ssid"] | "";
        if (strlen(ssid) == 0) {
            auto e = _makeErr("ssid required");
            _sendJson(r, 400, e);
            return;
        }
        Config::deleteWifiNetwork(ssid);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/wifi/move ───────────────────────────────────────────────────
    // Body: {ssid, direction: "up"|"down"} — swaps ssid with its immediate
    // neighbor; connect order is list order, so this changes priority.
    void _moveWifi(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* ssid = doc["ssid"] | "";
        const char* dir = doc["direction"] | "";
        if (strlen(ssid) == 0) {
            auto e = _makeErr("ssid required");
            _sendJson(r, 400, e);
            return;
        }
        int8_t direction;
        if (strcmp(dir, "up") == 0)
            direction = -1;
        else if (strcmp(dir, "down") == 0)
            direction = 1;
        else {
            auto e = _makeErr("direction must be up or down");
            _sendJson(r, 400, e);
            return;
        }
        if (!Config::moveWifiNetwork(ssid, direction)) {
            auto e = _makeErr("cannot move");
            _sendJson(r, 400, e);
            return;
        }
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // Body: {mac?, deviceName?, ledType?, addWifiNetworks?, apPassword?,
    //        mqttHost?, mqttPort?, mqttUser?, mqttPassword?, githubRepo?, githubToken?,
    //        otaEnabled?}
    // mac omitted or empty = push to all peers. Only present fields are pushed;
    // deviceName and ledType require a specific target mac.
    void _pushConfig(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* macStr = doc["mac"] | "";
        uint8_t targetMac[6] = {0, 0, 0, 0, 0, 0};
        bool hasTarget = macStr[0] != '\0';
        if (hasTarget && !_parseMac(macStr, targetMac)) {
            auto e = _makeErr("bad mac");
            _sendJson(r, 400, e);
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
                        auto e = _makeErr("name already in use");
                        _sendJson(r, 409, e);
                        return;
                    }
                    if (_peers) {
                        for (auto& p : *_peers) {
                            if (!p.active || memcmp(p.mac, targetMac, 6) == 0) continue;
                            if (strcmp(p.name, newName) == 0) {
                                auto e = _makeErr("name already in use");
                                _sendJson(r, 409, e);
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
        JsonArray useLocal = doc["useLocal"].as<JsonArray>();
        auto isUseLocal = [&](const char* key) -> bool {
            for (JsonVariant v : useLocal)
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
            auto e = _makeErr("no fields to push");
            _sendJson(r, 400, e);
            return;
        }

        String json;
        serializeJson(payload, json);
        if (_onPushConfig) _onPushConfig(targetMac, json.c_str(), json.length());
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // Body: {mac, enabled}
    void _setRemoteSceneSync(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* macStr = doc["mac"] | "";
        bool enabled = doc["enabled"] | true;

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            bool prev = Config::get().sceneSyncEnabled;
            Config::get().sceneSyncEnabled = enabled;
            Config::save();
            if (enabled && !prev && _sceneSync) _sceneSync->onSyncEnabled();
            if (enabled != prev && _onSceneSyncChanged) _onSceneSyncChanged();
            auto ok = _makeOk();
            _sendJson(r, 200, ok);
            return;
        }

        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            auto e = _makeErr("bad mac");
            _sendJson(r, 400, e);
            return;
        }
        if (_onSetRemoteSync) _onSetRemoteSync(mac, enabled);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // Body: {enabled}
    void _setWifiPolicy(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        bool enabled = doc["enabled"] | false;
        if (_onMeshPolicyChange)
            _onMeshPolicyChange(enabled);
        else {
            Config::get().wifiSingleClientMode = enabled;
            Config::save();
        }
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        _pushPeers();  // wifi icon colors in the device list depend on this flag
    }

    // Shared body for /api/peers/triggerupdate and /api/peers/checkupdate:
    // parse the target MAC, reject if that peer is known but offline, then
    // invoke the given callback.
    void _peerUpdateRequest(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                            const char* logVerb, const std::function<void(const uint8_t*)>& cb) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!_parseMac(macStr, mac)) {
            auto e = _makeErr("bad mac");
            _sendJson(r, 400, e);
            return;
        }
        if (_peers) {
            for (auto& p : *_peers) {
                if (p.active && memcmp(p.mac, mac, 6) == 0) {
                    if (!p.online()) {
                        auto e = _makeErr("peer offline");
                        _sendJson(r, 409, e);
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
                        auto e = _makeErr("peer not connected to WiFi");
                        _sendJson(r, 409, e);
                        return;
                    }
                    break;
                }
            }
        }
        Logger::i("[web] %s for %s", logVerb, macStr);
        if (cb) cb(mac);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/peers/triggerupdate ─────────────────────────────────────────
    void _triggerPeerUpdate(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        _peerUpdateRequest(r, data, len, "trigger-update", _onTriggerPeerUpdate);
    }

    // ── POST /api/peers/checkupdate ───────────────────────────────────────────
    void _checkPeerUpdate(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        _peerUpdateRequest(r, data, len, "check-update", _onCheckPeerUpdate);
    }

    // ── GET /api/lights ───────────────────────────────────────────────────────
    void _getLights(AsyncWebServerRequest* r) {
        JsonDocument doc;
        JsonArray arr = doc["lights"].to<JsonArray>();
        doc["maxLights"] = MAX_LIGHTS;
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
        _sendJson(r, 200, doc);
    }

    // ── POST /api/lights/add ──────────────────────────────────────────────────
    // Body: {ledType, colorOrder, dataPin, clockPin, width, height, matrixStart, matrixDir,
    // matrixSerpentine, wrapWidth, wrapHeight, groupId}
    void _addLight(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        // Find first free slot
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            if (!Config::get().lights[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            auto e = _makeErr("light limit reached");
            _sendJson(r, 400, e);
            return;
        }
        auto& l = Config::get().lights[idx];
        l.exists = true;
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
        Config::save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["index"] = idx;
        _sendJson(r, 200, resp);
        // Hardware config changes require restart to take effect
        delay(200);
        ESP.restart();
    }

    // ── POST /api/lights/update ───────────────────────────────────────────────
    // Body: {index, ledType?, colorOrder?, dataPin?, clockPin?, width?, height?, matrixStart?,
    // matrixDir?, matrixSerpentine?, wrapWidth?, wrapHeight?, groupId?}
    void _updateLight(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        auto& l = Config::get().lights[idx];
        bool hwChanged = false;
        if (!doc["name"].isNull()) strlcpy(l.name, doc["name"] | "", sizeof(l.name));
        if (!doc["ledType"].isNull()) {
            l.ledType = (LedType)(uint8_t)doc["ledType"];
            hwChanged = true;
        }
        bool colorOrderChanged = false;
        if (!doc["colorOrder"].isNull()) {
            l.colorOrder = (ColorOrder)(uint8_t)doc["colorOrder"];
            colorOrderChanged = true;
        }
        if (!doc["dataPin"].isNull()) {
            l.dataPin = doc["dataPin"];
            hwChanged = true;
        }
        if (!doc["clockPin"].isNull()) {
            l.clockPin = doc["clockPin"];
            hwChanged = true;
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
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        if (hwChanged) {
            delay(200);
            ESP.restart();
            return;
        }
        if (orientationChanged && _onOrientationChange) _onOrientationChange(idx);
        if (brightnessOverrideChanged && _onLightBrightnessChange) _onLightBrightnessChange(idx);
        if (colorOrderChanged && _onColorOrderChange) _onColorOrderChange(idx);
    }

    // ── POST /api/lights/test ─────────────────────────────────────────────────
    // Body: {index}
    void _testLight(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (Config::get().lights[idx].height < 2) {
            auto e = _makeErr("not a matrix");
            _sendJson(r, 400, e);
            return;
        }
        if (_onTestLight) _onTestLight(idx);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/lights/testcolor ────────────────────────────────────────────
    // Body: {index} — cycles the light through solid red/green/blue so the
    // user can check colorOrder against the strip's actual wiring. Unlike
    // /api/lights/test (orientation), this isn't restricted to matrix lights.
    void _testColorOrder(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (_onTestColorOrder) _onTestColorOrder(idx);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/lights/delete ───────────────────────────────────────────────
    // Body: {index}
    void _deleteLight(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        Config::get().lights[idx].exists = false;
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        delay(200);
        ESP.restart();
    }

    // ── GET /api/sounds ────────────────────────────────────────────────────────
    void _getSounds(AsyncWebServerRequest* r) {
        JsonDocument doc;
        JsonArray arr = doc["sounds"].to<JsonArray>();
        doc["maxSounds"] = MAX_SOUNDS;
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            auto& s = Config::get().sounds[i];
            if (!s.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            serializeSound(o, s);
        }
        _sendJson(r, 200, doc);
    }

    // Returns an error string if any of s's non-unused pins collide with each
    // other or with a light/button/other-sound pin, else nullptr. Pass the
    // sound's own index as excludeSoundIndex when validating an update.
    // paEnablePin is only checked as a real ESP32 GPIO when paExpander is
    // None — on an expander it's a pin index in a separate address space
    // (see IoExpanderChip) and would produce false conflicts if compared
    // against actual GPIO numbers.
    static const char* _soundPinConflict(const SoundHardwareConfig& s, int8_t excludeSoundIndex) {
        uint8_t pins[] = {s.i2cSdaPin, s.i2cSclPin,  s.i2sMclkPin, s.i2sBclkPin,
                          s.i2sWsPin,  s.i2sDoutPin, s.paEnablePin};
        size_t count = s.paExpander == IoExpanderChip::None ? 7 : 6;
        for (size_t i = 0; i < count; i++) {
            if (pins[i] == SOUND_PIN_UNUSED) continue;
            for (size_t j = i + 1; j < count; j++) {
                if (pins[j] == pins[i]) return "duplicate pin within sound config";
            }
            if (Config::isPinInUse(pins[i], -1, excludeSoundIndex)) return "pin already in use";
        }
        return nullptr;
    }

    // ── POST /api/sounds/add ──────────────────────────────────────────────────
    // Body: {name?, chip?, i2cSdaPin, i2cSclPin, i2cAddress?, i2sMclkPin?, i2sBclkPin,
    // i2sWsPin, i2sDoutPin, paEnablePin?, paEnableActiveHigh?, paExpander?, paExpanderAddress?}
    void _addSound(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            if (!Config::get().sounds[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            auto e = _makeErr("sound limit reached");
            _sendJson(r, 400, e);
            return;
        }
        SoundHardwareConfig s;
        deserializeSound(doc, s);
        if (s.i2cSdaPin == SOUND_PIN_UNUSED || s.i2cSclPin == SOUND_PIN_UNUSED ||
            s.i2sBclkPin == SOUND_PIN_UNUSED || s.i2sWsPin == SOUND_PIN_UNUSED ||
            s.i2sDoutPin == SOUND_PIN_UNUSED) {
            auto e = _makeErr("missing required pin");
            _sendJson(r, 400, e);
            return;
        }
        if (const char* conflict = _soundPinConflict(s, /*excludeSoundIndex=*/idx)) {
            auto e = _makeErr(conflict);
            _sendJson(r, 400, e);
            return;
        }
        s.exists = true;
        Config::get().sounds[idx] = s;
        Config::save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["index"] = idx;
        _sendJson(r, 200, resp);
        // Hardware config changes require restart to take effect
        delay(200);
        ESP.restart();
    }

    // ── POST /api/sounds/update ─────────────────────────────────────────────────
    // Body: {index, name?, chip?, i2cSdaPin?, i2cSclPin?, i2cAddress?, i2sMclkPin?,
    // i2sBclkPin?, i2sWsPin?, i2sDoutPin?, paEnablePin?, paEnableActiveHigh?, paExpander?,
    // paExpanderAddress?}
    void _updateSound(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
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
        if (!doc["i2cSdaPin"].isNull()) {
            candidate.i2cSdaPin = doc["i2cSdaPin"];
            hwChanged = true;
        }
        if (!doc["i2cSclPin"].isNull()) {
            candidate.i2cSclPin = doc["i2cSclPin"];
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
        if (!doc["paExpander"].isNull()) {
            candidate.paExpander = (IoExpanderChip)(uint8_t)doc["paExpander"];
            hwChanged = true;
        }
        if (!doc["paExpanderAddress"].isNull()) {
            candidate.paExpanderAddress = doc["paExpanderAddress"];
            hwChanged = true;
        }

        if (hwChanged) {
            if (candidate.i2cSdaPin == SOUND_PIN_UNUSED ||
                candidate.i2cSclPin == SOUND_PIN_UNUSED ||
                candidate.i2sBclkPin == SOUND_PIN_UNUSED ||
                candidate.i2sWsPin == SOUND_PIN_UNUSED ||
                candidate.i2sDoutPin == SOUND_PIN_UNUSED) {
                auto e = _makeErr("missing required pin");
                _sendJson(r, 400, e);
                return;
            }
            if (const char* conflict = _soundPinConflict(candidate, /*excludeSoundIndex=*/idx)) {
                auto e = _makeErr(conflict);
                _sendJson(r, 400, e);
                return;
            }
            candidate.exists = true;
            existing = candidate;
        }
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        if (hwChanged) {
            delay(200);
            ESP.restart();
        }
    }

    // ── POST /api/sounds/delete ───────────────────────────────────────────────
    // Body: {index}
    void _deleteSound(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        Config::get().sounds[idx].exists = false;
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        delay(200);
        ESP.restart();
    }

    // ── POST /api/sounds/test ─────────────────────────────────────────────────
    // Body: {index} — plays the built-in hardware-verification melody.
    void _testSound(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_SOUNDS || !Config::get().sounds[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (_onTestSound) _onTestSound(idx);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
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
    void _getStorage(AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["hwSupported"] = SdCardManager::kHwSupported;
        bool present = _sdCard && _sdCard->present();
        doc["present"] = present;
        doc["totalBytes"] = present ? _sdCard->totalBytes() : 0;
        doc["usedBytes"] = present ? _sdCard->usedBytes() : 0;
        JsonArray files = doc["files"].to<JsonArray>();
        if (present) {
            _sdCard->forEachFile([&](const String& name, size_t size) {
                JsonObject o = files.add<JsonObject>();
                o["name"] = name;
                o["size"] = size;
            });
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/storage/upload?name=<file.wav> ────────────────────────────
    // Body: raw file bytes (streamed straight to the SD card, not buffered —
    // audio files are too large for that, unlike the scene-save JSON above).
    void _storageUploadChunk(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index,
                             size_t) {
        auto* st = static_cast<StorageUploadState*>(r->_tempObject);
        if (!st) {
            st = new StorageUploadState();
            r->_tempObject = st;

            String name;
            if (r->hasParam("name")) name = r->getParam("name")->value();
            if (!_isValidWavName(name)) {
                Logger::w("[storage] upload: rejected filename %s", name.c_str());
                st->failed = true;
                st->error = "invalid filename";
                return;
            }
            if (!_sdCard || !_sdCard->present()) {
                st->failed = true;
                st->error = "no SD card";
                return;
            }
            st->file = _sdCard->openForWrite(name);
            if (!st->file) {
                Logger::e("[storage] upload: open failed for %s", name.c_str());
                st->failed = true;
                st->error = "open failed";
                return;
            }
            Logger::i("[storage] upload: %s", name.c_str());
        }

        if (st->failed || !len) return;

        size_t written = st->file.write(data, len);
        if (written != len) {
            Logger::e("[storage] upload: chunk write incomplete (%u/%u) at index=%u",
                      (unsigned)written, (unsigned)len, (unsigned)index);
            st->failed = true;
            st->error = "write failed";
            st->file.close();
            st->file = File();
        }
    }

    void _finishStorageUpload(AsyncWebServerRequest* r) {
        auto* st = static_cast<StorageUploadState*>(r->_tempObject);
        if (!st) {
            auto e = _makeErr("no body");
            _sendJson(r, 400, e);
            return;
        }
        if (st->file) st->file.close();

        JsonDocument resp;
        int code = 200;
        if (st->failed) {
            resp["error"] = st->error ? st->error : "upload failed";
            code = 400;
        } else {
            resp["ok"] = true;
        }
        delete st;
        r->_tempObject = nullptr;
        _sendJson(r, code, resp);
    }

    // ── POST /api/storage/delete ─────────────────────────────────────────────
    // Body: {name}
    void _deleteStorageFile(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        String name = doc["name"] | "";
        if (!_isValidWavName(name)) {
            auto e = _makeErr("invalid filename");
            _sendJson(r, 400, e);
            return;
        }
        if (!_sdCard || !_sdCard->deleteFile(name)) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── GET /api/buttons ──────────────────────────────────────────────────────
    void _getButtons(AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["maxButtons"] = MAX_BUTTONS;
        JsonArray arr = doc["buttons"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            auto& b = Config::get().buttons[i];
            if (!b.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            serializeButton(o, b);
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/buttons/add ─────────────────────────────────────────────────
    // Body: {name?, pin, activeLow?, onShortPress?, onLongPress?, onDoubleClick?}
    void _addButton(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            if (!Config::get().buttons[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            auto e = _makeErr("button limit reached");
            _sendJson(r, 400, e);
            return;
        }
        uint8_t pin = doc["pin"] | (uint8_t)0;
        if (Config::isPinInUse(pin)) {
            auto e = _makeErr("pin already in use");
            _sendJson(r, 400, e);
            return;
        }
        auto& b = Config::get().buttons[idx];
        b = ButtonHardwareConfig{};
        deserializeButton(doc, b);
        b.pin = pin;  // deserializeButton already applied it, but keep explicit — validated above
        b.exists = true;  // deserializeButton defaults "exists" to false when the key is absent
        Config::save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["index"] = idx;
        _sendJson(r, 200, resp);
        if (_onButtonsChanged) _onButtonsChanged();
    }

    // ── POST /api/buttons/update ──────────────────────────────────────────────
    // Body: {index, name?, pin?, activeLow?, onShortPress?, onLongPress?, onDoubleClick?}
    void _updateButton(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_BUTTONS || !Config::get().buttons[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        auto& b = Config::get().buttons[idx];
        if (!doc["name"].isNull()) strlcpy(b.name, doc["name"] | "", sizeof(b.name));
        if (!doc["activeLow"].isNull()) b.activeLow = (bool)doc["activeLow"];
        if (!doc["pin"].isNull()) {
            uint8_t pin = doc["pin"];
            if (Config::isPinInUse(pin, (int8_t)idx)) {
                auto e = _makeErr("pin already in use");
                _sendJson(r, 400, e);
                return;
            }
            b.pin = pin;
        }
        if (!doc["onShortPress"].isNull())
            b.onShortPress = deserializeButtonAction(doc["onShortPress"], b.onShortPress);
        if (!doc["onLongPress"].isNull())
            b.onLongPress = deserializeButtonAction(doc["onLongPress"], b.onLongPress);
        if (!doc["onDoubleClick"].isNull())
            b.onDoubleClick = deserializeButtonAction(doc["onDoubleClick"], b.onDoubleClick);
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        if (_onButtonsChanged) _onButtonsChanged();
    }

    // ── POST /api/buttons/delete ──────────────────────────────────────────────
    // Body: {index}
    void _deleteButton(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_BUTTONS || !Config::get().buttons[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        Config::get().buttons[idx].exists = false;
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
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
