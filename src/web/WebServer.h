#pragma once
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include <functional>

#include "../battery/BatteryMonitor.h"
#include "../config/Config.h"
#include "../events/EventLog.h"
#include "../logging/Logger.h"
#include "../mesh/ChannelManager.h"
#include "../mesh/PeerRegistry.h"
#include "../scenes/SceneManager.h"
#include "../scenes/SceneSyncManager.h"
#include "../sound/PlaylistManager.h"
#include "../sound/PlaylistSyncManager.h"
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
// Called after this device's own sound volume changes via REST, so it can be
// applied live to the driver without a reboot (see SoundHardwareConfig::volume).
using SoundVolumeChangeCb = std::function<void(uint8_t soundIndex, uint8_t volume)>;
// Called after an audio group is created/updated/deleted (the full AudioGroupConfig)
using AudioGroupSyncCb = std::function<void(const AudioGroupConfig&)>;
// Called to start synchronized playback of a single SD-card file across an audio group
using PlayFileCb = std::function<void(uint8_t audioGroupId, const char* filename, bool loop)>;
// Called to start synchronized playback of a saved playlist across an audio group
using PlayPlaylistCb = std::function<void(uint8_t audioGroupId, const char* playlistId)>;
// Called to stop playback across an audio group
using StopAudioCb = std::function<void(uint8_t audioGroupId)>;
// Called after a playlist is created or deleted (not edited) — mirrors SceneListChangedCb
using PlaylistListChangedCb = std::function<void()>;
// Called to move a specific peer's sound output to a different audio group — mirrors
// SetRemoteGroupCb, called only for a genuinely remote target (own-mac is applied directly).
using SetRemoteAudioGroupCb = std::function<void(const uint8_t* mac, uint8_t audioGroupId)>;
// Called to change a specific peer's sound output volume — mirrors SetRemoteAudioGroupCb.
using SetRemoteVolumeCb =
    std::function<void(const uint8_t* mac, uint8_t volume, bool overrideEnabled)>;

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
        _server.on("/api/wifi/confirm-disable-ap", HTTP_POST, [this](AsyncWebServerRequest* r) {
            if (_onConfirmApDisable) _onConfirmApDisable();
            auto ok = _makeOk();
            _sendJson(r, 200, ok);
        });

        _server.on("/api/peers", HTTP_GET, [this](AsyncWebServerRequest* r) { _getPeers(r); });
        _server.on("/api/events", HTTP_GET, [this](AsyncWebServerRequest* r) { _getEvents(r); });
        _server.on("/api/events/clear", HTTP_POST,
                   [this](AsyncWebServerRequest* r) { _clearEvents(r); });

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
        _server.on(
            "/api/peers/setaudiogroup", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _setRemoteAudioGroup(r, d, l);
            });
        _server.on(
            "/api/peers/setvolume", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _setRemoteVolume(r, d, l);
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

        // ── Audio groups ─────────────────────────────────────────────────────
        _server.on("/api/audiogroups", HTTP_GET,
                   [this](AsyncWebServerRequest* r) { _getAudioGroups(r); });
        _server.on(
            "/api/audiogroups/create", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _createAudioGroup(r, d, l);
            });
        _server.on(
            "/api/audiogroups/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateAudioGroup(r, d, l);
            });
        _server.on(
            "/api/audiogroups/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteAudioGroup(r, d, l);
            });

        // ── Playlists ────────────────────────────────────────────────────────
        // Specific routes registered before /api/playlists for the same
        // prefix-matching reason as scenes above.
        PlaylistManager::init();
        _server.on(
            "/api/playlists/create", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _createPlaylist(r, d, l);
            });
        _server.on(
            "/api/playlists/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deletePlaylist(r, d, l);
            });
        _server.on(
            "/api/playlists/save", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _savePlaylist(r, d, l);
            });
        _server.on("/api/playlists", HTTP_GET,
                   [this](AsyncWebServerRequest* r) { _getPlaylists(r); });

        // ── Playback triggers ────────────────────────────────────────────────
        _server.on(
            "/api/audio/play/file", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _playFile(r, d, l);
            });
        _server.on(
            "/api/audio/play/playlist", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _playPlaylist(r, d, l);
            });
        _server.on(
            "/api/audio/stop", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _stopAudioGroup(r, d, l);
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

        _server.on("/api/automations", HTTP_GET,
                   [this](AsyncWebServerRequest* r) { _getAutomations(r); });
        _server.on(
            "/api/automations/add", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _addAutomation(r, d, l);
            });
        _server.on(
            "/api/automations/update", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _updateAutomation(r, d, l);
            });
        _server.on(
            "/api/automations/delete", HTTP_POST, [](AsyncWebServerRequest*) {}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t) {
                _deleteAutomation(r, d, l);
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
    void setPlaylistSync(PlaylistSyncManager* ps) { _playlistSync = ps; }
    void setOnSoundVolumeChange(SoundVolumeChangeCb cb) { _onSoundVolumeChange = cb; }
    void setOnAudioGroupSync(AudioGroupSyncCb cb) { _onAudioGroupSync = cb; }
    void setOnPlayFile(PlayFileCb cb) { _onPlayFile = cb; }
    void setOnPlayPlaylist(PlayPlaylistCb cb) { _onPlayPlaylist = cb; }
    void setOnStopAudio(StopAudioCb cb) { _onStopAudio = cb; }
    void setOnPlaylistListChanged(PlaylistListChangedCb cb) { _onPlaylistListChanged = cb; }
    void setOnSetRemoteAudioGroup(SetRemoteAudioGroupCb cb) { _onSetRemoteAudioGroup = cb; }
    void setOnSetRemoteVolume(SetRemoteVolumeCb cb) { _onSetRemoteVolume = cb; }
    void pushAudioGroups() { _pushAudioGroups(); }
    void setEventLog(EventLog* log) { _eventLog = log; }
    void pushEvent(const EventLogEntry& e) { _pushEvent(e); }
    void pushEventsCleared() { _pushEventsCleared(); }

   private:
    AsyncWebServer _server{80};
    AsyncWebSocket* _ws = nullptr;
    RequestLogger _reqLogger;
    PeerRegistry* _peers = nullptr;
    ChannelManager* _channelMgr = nullptr;
    SdCardManager* _sdCard = nullptr;
    EventLog* _eventLog = nullptr;

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
    PlaylistSyncManager* _playlistSync = nullptr;
    SoundVolumeChangeCb _onSoundVolumeChange;
    AudioGroupSyncCb _onAudioGroupSync;
    PlayFileCb _onPlayFile;
    PlayPlaylistCb _onPlayPlaylist;
    StopAudioCb _onStopAudio;
    PlaylistListChangedCb _onPlaylistListChanged;
    SetRemoteAudioGroupCb _onSetRemoteAudioGroup;
    SetRemoteVolumeCb _onSetRemoteVolume;

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
        doc["eventLogLimit"] = c.eventLogLimit;
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
        _sendJson(r, 200, doc);
    }

    // ── POST /api/config ─────────────────────────────────────────────────────
    // Only deviceName/otaPort/otaEnabled genuinely require a reboot: mDNS,
    // ArduinoOTA, the AP SSID, and MQTT's topic prefix all derive from
    // deviceName and are only initialized once at boot, and ArduinoOTA itself
    // is only begin()'d once (conditionally on otaEnabled). Every other field
    // is applied live via the callbacks below, and the response reports
    // whether a reboot is actually happening so the web UI only shows/waits
    // for one when it's really going to happen.
    void _postConfig(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
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
        if (!doc["eventLogLimit"].isNull())
            c.eventLogLimit =
                (uint8_t)constrain((int)doc["eventLogLimit"], 1, (int)EventLog::CAPACITY);

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
                    auto e = _makeErr(
                        "I2C bus still used by a configured sound output, button, or "
                        "the configured expander");
                    _sendJson(r, 400, e);
                    return;
                }
                if (c.i2cSdaPin != PIN_UNUSED || c.i2cSclPin != PIN_UNUSED) rebootNeeded = true;
                c.i2cSdaPin = PIN_UNUSED;
                c.i2cSclPin = PIN_UNUSED;
            } else {
                if (sda == scl) {
                    auto e = _makeErr("SDA and SCL must be different pins");
                    _sendJson(r, 400, e);
                    return;
                }
                if (Config::isPinInUse(sda, -1, -1, -1, /*excludeI2cBus=*/true) ||
                    Config::isPinInUse(scl, -1, -1, -1, /*excludeI2cBus=*/true)) {
                    auto e = _makeErr("pin already in use");
                    _sendJson(r, 400, e);
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
                auto e = _makeErr("configure the device I2C bus first");
                _sendJson(r, 400, e);
                return;
            }
            if (newChip == IoExpanderChip::None && Config::expanderInUse()) {
                auto e = _makeErr("expander still used by a configured sound output or button");
                _sendJson(r, 400, e);
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

        auto ok = _makeOk();
        ok["rebooting"] = rebootNeeded;
        _sendJson(r, 200, ok);
        if (rebootNeeded) {
            delay(200);
            ESP.restart();
        }
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

    // ── GET /api/events ──────────────────────────────────────────────────────
    void _getEvents(AsyncWebServerRequest* r) {
        String filter = r->hasParam("eventType") ? r->getParam("eventType")->value() : "";
        JsonDocument doc;
        JsonArray arr = doc["events"].to<JsonArray>();
        if (_eventLog) {
            _eventLog->forEach(filter.c_str(), Config::get().eventLogLimit,
                               [&](const EventLogEntry& e) {
                                   JsonObject o = arr.add<JsonObject>();
                                   o["name"] = e.peerName;
                                   o["eventType"] = e.eventType;
                                   o["payload"] = e.payload;
                                   o["order"] = e.order;
                               });
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/events/clear ───────────────────────────────────────────────
    void _clearEvents(AsyncWebServerRequest* r) {
        if (_eventLog) _eventLog->clear();
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
        {
            self["sound"] = nullptr;
            for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
                if (!c.sounds[i].exists) continue;
                JsonObject so = self["sound"].to<JsonObject>();
                so["name"] = c.sounds[i].name;
                so["audioGroupId"] = c.sounds[i].audioGroupId;
                so["volumeOverrideEnabled"] = c.sounds[i].volumeOverrideEnabled;
                // Effective (currently applied) volume — own override if
                // enabled, else the audio group's shared volume. See
                // Config::effectiveSoundVolume.
                so["volume"] = Config::effectiveSoundVolume(c.sounds[i]);
                break;
            }
        }

        JsonArray arr = doc["peers"].to<JsonArray>();
        // Devices known only via HelloMsg (different/incompatible firmware —
        // see MeshTypes.h) aren't full peers: none of the fields below beyond
        // mac/name/version/online are populated for them. Listed separately
        // so the dashboard can offer just a WiFi-config push and an
        // update-check nudge instead of the full peer row.
        JsonArray discoveredArr = doc["discoveredPeers"].to<JsonArray>();
        if (_peers) {
            for (auto& p : *_peers) {
                if (!p.active) continue;
                if (p.helloOnly) {
                    auto d = discoveredArr.add<JsonObject>();
                    d["mac"] = p.macStr();
                    d["name"] = p.name;
                    d["version"] = p.fwVersion;
                    d["online"] = p.online();
                    continue;
                }
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
                if (p.hasSound) {
                    JsonObject so = o["sound"].to<JsonObject>();
                    so["name"] = p.soundName;
                    so["audioGroupId"] = p.soundAudioGroupId;
                    so["volumeOverrideEnabled"] = p.soundVolumeOverrideEnabled;
                    so["volume"] = p.soundVolume;  // effective volume, see PresenceMsg::soundVolume
                } else {
                    o["sound"] = nullptr;
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

    // ── POST /api/peers/setaudiogroup ────────────────────────────────────────
    // Body: {mac, audioGroupId} — mirrors _setRemoteGroup for the (single, see
    // MAX_SOUNDS) sound output a device may have.
    void _setRemoteAudioGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t audioGroupId = doc["audioGroupId"] | (uint8_t)0;
        const char* macStr = doc["mac"] | "";

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            if (Config::audioGroup(audioGroupId)) {
                Config::forEachSound(
                    [&](uint8_t, SoundHardwareConfig& s) { s.audioGroupId = audioGroupId; });
                Config::save();
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

        if (_onSetRemoteAudioGroup) _onSetRemoteAudioGroup(mac, audioGroupId);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/peers/setvolume ────────────────────────────────────────────
    // Body: {mac, volume, overrideEnabled} — mirrors _setRemoteAudioGroup.
    // overrideEnabled=false clears the override (the target reverts to
    // following its audio group's shared volume); volume is only meaningful
    // when overrideEnabled is true.
    void _setRemoteVolume(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t volume = (uint8_t)constrain((int)(doc["volume"] | (uint8_t)0), SOUND_VOLUME_MIN,
                                            SOUND_VOLUME_MAX);
        bool overrideEnabled = doc["overrideEnabled"] | false;
        const char* macStr = doc["mac"] | "";

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            Config::forEachSound([&](uint8_t, SoundHardwareConfig& s) {
                s.volumeOverrideEnabled = overrideEnabled;
                if (overrideEnabled) s.volume = volume;
            });
            Config::save();
            if (_onSoundVolumeChange) _onSoundVolumeChange(0, volume);
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

        if (_onSetRemoteVolume) _onSetRemoteVolume(mac, volume, overrideEnabled);
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

    void _pushEvent(const EventLogEntry& e) {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "event";
        doc["name"] = e.peerName;
        doc["eventType"] = e.eventType;
        doc["payload"] = e.payload;
        doc["order"] = e.order;
        String s;
        serializeJson(doc, s);
        _ws->textAll(s);
    }

    void _pushEventsCleared() {
        if (!_ws || _ws->count() == 0) return;
        _ws->textAll("{\"t\":\"eventsCleared\"}");
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
        // Nothing else actually connects to a newly-saved network until the
        // next reboot — kick off a live attempt now if this device isn't
        // already on WiFi, so onboarding doesn't require a manual power cycle.
        if (WiFi.status() != WL_CONNECTED && _onWifiConnectForConfirm) {
            _onWifiConnectForConfirm([](bool) {});
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
            auto e = _makeErr(conflict);
            _sendJson(r, 400, e);
            return;
        }
        l.exists = true;
        Config::get().lights[idx] = l;
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
                auto e = _makeErr(conflict);
                _sendJson(r, 400, e);
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
    void _addSound(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        if (!_i2cBusConfigured()) {
            auto e = _makeErr("configure the device I2C bus in Hardware settings first");
            _sendJson(r, 400, e);
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
            auto e = _makeErr("sound limit reached");
            _sendJson(r, 400, e);
            return;
        }
        SoundHardwareConfig s;
        deserializeSound(doc, s);
        if (s.i2sBclkPin == PIN_UNUSED || s.i2sWsPin == PIN_UNUSED || s.i2sDoutPin == PIN_UNUSED) {
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
    // Body: {index, name?, chip?, i2cAddress?, i2sMclkPin?, i2sBclkPin?, i2sWsPin?,
    // i2sDoutPin?, paEnablePin?, paEnableActiveHigh?, paViaExpander?}
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

        // Group membership, volume, and its override flag aren't hardware config
        // (no reboot needed) — applied immediately, volume live to the driver.
        // Also settable cross-device via /api/peers/setaudiogroup and
        // /api/peers/setvolume.
        bool volumeChanged = false;
        if (!doc["audioGroupId"].isNull()) {
            existing.audioGroupId = doc["audioGroupId"];
            volumeChanged = true;  // this sound's shared-volume source just changed
        }
        if (!doc["volumeOverrideEnabled"].isNull()) {
            existing.volumeOverrideEnabled = doc["volumeOverrideEnabled"];
            volumeChanged = true;
        }
        if (!doc["volume"].isNull()) {
            existing.volume =
                (uint8_t)constrain((int)doc["volume"], SOUND_VOLUME_MIN, SOUND_VOLUME_MAX);
            volumeChanged = true;
        }

        // Every remaining field is hardware config — apply on top of a copy first
        // so a rejected pin conflict leaves the saved config untouched.
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
        if (volumeChanged && _onSoundVolumeChange) _onSoundVolumeChange(idx, existing.volume);
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

    // ── Audio groups ─────────────────────────────────────────────────────────
    // Mirrors the light-group handlers above (_createGroup/_updateGroup/
    // _deleteGroup) — no LightConfig-equivalent payload, no syncEnabled, since
    // playback triggers are one-shot events, not continuously-synced state.

    void _getAudioGroups(AsyncWebServerRequest* r) {
        JsonDocument doc;
        JsonArray arr = doc["audioGroups"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_AUDIO_GROUPS; i++) {
            auto& g = Config::get().audioGroups[i];
            if (!g.exists) continue;
            serializeAudioGroup(arr.add<JsonObject>(), g);
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/audiogroups/create ─────────────────────────────────────────
    void _createAudioGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* name = doc["name"] | "New Group";
        uint8_t id = Config::createAudioGroup(name);
        if (id == 0xFF) {
            auto e = _makeErr("group limit reached");
            _sendJson(r, 400, e);
            return;
        }
        const AudioGroupConfig& g = Config::get().audioGroups[id];
        Config::save();
        if (_onAudioGroupSync) _onAudioGroupSync(g);

        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        _sendJson(r, 200, resp);
        _pushAudioGroups();
    }

    // ── POST /api/audiogroups/update ─────────────────────────────────────────
    // Body: {id, name?, volume?} — volume is this group's shared volume,
    // followed live by every member device without its own volumeOverrideEnabled.
    void _updateAudioGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t id = doc["id"] | (uint8_t)0;
        AudioGroupConfig* g = Config::audioGroup(id);
        if (!g) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (!doc["name"].isNull()) strlcpy(g->name, doc["name"], sizeof(g->name));
        if (!doc["volume"].isNull())
            g->volume = (uint8_t)constrain((int)doc["volume"], SOUND_VOLUME_MIN, SOUND_VOLUME_MAX);
        Config::bumpAudioGroupRevision(*g);
        Config::save();
        if (_onAudioGroupSync) _onAudioGroupSync(*g);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        _pushAudioGroups();
    }

    // ── POST /api/audiogroups/delete ─────────────────────────────────────────
    void _deleteAudioGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t id = doc["id"] | (uint8_t)0;
        if (id == 0) {
            auto e = _makeErr("cannot delete Default");
            _sendJson(r, 400, e);
            return;
        }
        AudioGroupConfig* g = Config::audioGroup(id);
        if (!g) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }

        Config::bumpAudioGroupRevision(*g);
        AudioGroupConfig tombstone = *g;
        tombstone.exists = false;
        g->exists = false;

        // Move this device's own sound output(s), if members, back to Default —
        // mirrors _deleteGroup moving lights.
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            auto& s = Config::get().sounds[i];
            if (s.exists && s.audioGroupId == id) s.audioGroupId = 0;
        }

        Config::save();
        if (_onAudioGroupSync) _onAudioGroupSync(tombstone);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
        _pushAudioGroups();
    }

    // ── Playlists ────────────────────────────────────────────────────────────
    // Mirrors the scene handlers above; unlike scenes, playlist bodies are
    // small (a name + a handful of filenames), so no chunked-buffering save
    // state is needed — same simple body-callback pattern as groups/sounds.

    void _getPlaylists(AsyncWebServerRequest* r) {
        JsonDocument resp;
        PlaylistManager::buildList(resp);
        _sendJson(r, 200, resp);
    }

    void _createPlaylist(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len, "create")) return;
        const char* name = doc["name"] | "Unnamed";
        String id = PlaylistManager::create(name);
        if (id.isEmpty()) {
            auto e = _makeErr("create failed");
            _sendJson(r, 500, e);
            return;
        }
        if (_onPlaylistListChanged) _onPlaylistListChanged();
        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        _sendJson(r, 200, resp);
    }

    void _deletePlaylist(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len, "delete")) return;
        const char* id = doc["id"] | "";
        if (!id[0]) {
            auto e = _makeErr("missing id");
            _sendJson(r, 400, e);
            return;
        }
        bool ok = _playlistSync ? _playlistSync->deletePlaylist(id) : PlaylistManager::remove(id);
        if (ok && _onPlaylistListChanged) _onPlaylistListChanged();
        JsonDocument resp;
        if (ok)
            resp["ok"] = true;
        else
            resp["error"] = "not found";
        _sendJson(r, ok ? 200 : 404, resp);
    }

    // Body: full playlist JSON {id, name, loop, files:[...]} — the browser is
    // responsible for including a valid "id" (create first, then save edits).
    void _savePlaylist(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        String id;
        if (!PlaylistManager::extractId((const char*)data, len, id)) {
            Logger::e("[playlist] save: missing or invalid id");
            auto e = _makeErr("missing or invalid id");
            _sendJson(r, 400, e);
            return;
        }
        uint32_t prevHash = PlaylistManager::crc32(id.c_str());
        bool ok = PlaylistManager::save((const char*)data, len);
        if (ok) {
            Logger::i("[playlist] save ok: %s", id.c_str());
            if (_playlistSync) _playlistSync->onPlaylistChanged(id.c_str(), prevHash);
            if (_onPlaylistListChanged) _onPlaylistListChanged();
            auto okResp = _makeOk();
            _sendJson(r, 200, okResp);
        } else {
            Logger::e("[playlist] save failed: %s", id.c_str());
            auto e = _makeErr("save failed");
            _sendJson(r, 500, e);
        }
    }

    // ── Playback triggers ────────────────────────────────────────────────────
    // Fire-and-forget, same as every other mesh-broadcasting endpoint here —
    // see the "no cross-device playback state" design decision: there is no
    // status to poll afterward, these calls just start/stop synchronized
    // playback on the target audio group.

    // Body: {audioGroupId, filename, loop?}
    void _playFile(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t groupId = doc["audioGroupId"] | (uint8_t)0;
        const char* filename = doc["filename"] | "";
        bool loop = doc["loop"] | false;
        if (!filename[0]) {
            auto e = _makeErr("missing filename");
            _sendJson(r, 400, e);
            return;
        }
        if (!Config::audioGroup(groupId)) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (_onPlayFile) _onPlayFile(groupId, filename, loop);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // Body: {audioGroupId, playlistId}
    void _playPlaylist(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t groupId = doc["audioGroupId"] | (uint8_t)0;
        const char* playlistId = doc["playlistId"] | "";
        if (!playlistId[0]) {
            auto e = _makeErr("missing playlistId");
            _sendJson(r, 400, e);
            return;
        }
        if (!Config::audioGroup(groupId)) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (_onPlayPlaylist) _onPlayPlaylist(groupId, playlistId);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // Body: {audioGroupId}
    void _stopAudioGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t groupId = doc["audioGroupId"] | (uint8_t)0;
        if (_onStopAudio) _onStopAudio(groupId);
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    void _pushAudioGroups() {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "audioGroups";
        JsonArray arr = doc["list"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_AUDIO_GROUPS; i++) {
            auto& g = Config::get().audioGroups[i];
            if (!g.exists) continue;
            serializeAudioGroup(arr.add<JsonObject>(), g);
        }
        String s;
        serializeJson(doc, s);
        _ws->textAll(s);
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
        // Only list files this API can actually manage (see _isValidWavName) —
        // the card's root can otherwise hold arbitrary pre-existing files (a
        // prior recording, OS-created metadata from formatting the card on a
        // computer, ...) that would show up here but always 400 on delete.
        JsonArray files = doc["files"].to<JsonArray>();
        if (present) {
            _sdCard->forEachFile([&](const String& name, size_t size) {
                if (!_isValidWavName(name)) return;
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
        ButtonHardwareConfig b;
        deserializeButton(doc, b);
        if (const char* conflict = _buttonPinConflict(b, /*excludeButtonIndex=*/idx)) {
            auto e = _makeErr(conflict);
            _sendJson(r, 400, e);
            return;
        }
        b.exists = true;  // deserializeButton defaults "exists" to false when the key is absent
        Config::get().buttons[idx] = b;
        Config::save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["index"] = idx;
        _sendJson(r, 200, resp);
        if (_onButtonsChanged) _onButtonsChanged();
    }

    // ── POST /api/buttons/update ──────────────────────────────────────────────
    // Body: {index, name?, pin?, activeLow?, viaExpander?, onShortPress?, onLongPress?,
    // onDoubleClick?}
    void _updateButton(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_BUTTONS || !Config::get().buttons[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
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
                auto e = _makeErr(conflict);
                _sendJson(r, 400, e);
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

    // ── GET /api/automations ──────────────────────────────────────────────────
    void _getAutomations(AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["maxAutomations"] = MAX_AUTOMATION_BINDINGS;
        doc["maxRulesPerBinding"] = MAX_RULES_PER_BINDING;
        doc["maxActionsPerRule"] = MAX_ACTIONS_PER_RULE;
        JsonArray arr = doc["automations"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_AUTOMATION_BINDINGS; i++) {
            auto& b = Config::get().automations[i];
            if (!b.exists) continue;
            JsonObject o = arr.add<JsonObject>();
            o["index"] = i;
            serializeAutomationBinding(o, b);
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/automations/add ─────────────────────────────────────────────
    // Body: {triggerType?, eventType, rules?}
    void _addAutomation(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        const char* eventType = doc["eventType"] | "";
        if (!eventType[0]) {
            auto e = _makeErr("eventType required");
            _sendJson(r, 400, e);
            return;
        }
        uint8_t idx = 0xFF;
        for (uint8_t i = 0; i < MAX_AUTOMATION_BINDINGS; i++) {
            if (!Config::get().automations[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx == 0xFF) {
            auto e = _makeErr("automation limit reached");
            _sendJson(r, 400, e);
            return;
        }
        AutomationBinding b;
        deserializeAutomationBinding(doc, b);
        b.exists = true;  // deserializeAutomationBinding defaults "exists" to false when absent
        Config::get().automations[idx] = b;
        Config::save();
        JsonDocument resp;
        resp["ok"] = true;
        resp["index"] = idx;
        _sendJson(r, 200, resp);
    }

    // ── POST /api/automations/update ──────────────────────────────────────────
    // Body: {index, triggerType?, eventType?, rules?}
    void _updateAutomation(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_AUTOMATION_BINDINGS || !Config::get().automations[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        if (!doc["eventType"].isNull() && !(doc["eventType"] | "")[0]) {
            auto e = _makeErr("eventType required");
            _sendJson(r, 400, e);
            return;
        }
        auto& existing = Config::get().automations[idx];
        if (!doc["triggerType"].isNull())
            existing.triggerType = (TriggerType)(uint8_t)(doc["triggerType"] | (uint8_t)0);
        if (!doc["eventType"].isNull())
            strlcpy(existing.eventType, doc["eventType"] | "", sizeof(existing.eventType));
        if (!doc["rules"].isNull()) {
            JsonArray arr = doc["rules"].as<JsonArray>();
            for (uint8_t i = 0; i < MAX_RULES_PER_BINDING; i++)
                existing.rules[i] = deserializeAutomationRule(arr[i], AutomationRule{});
        }
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
    }

    // ── POST /api/automations/delete ──────────────────────────────────────────
    // Body: {index}
    void _deleteAutomation(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (!_parseJson(r, doc, data, len)) return;
        uint8_t idx = doc["index"] | (uint8_t)0xFF;
        if (idx >= MAX_AUTOMATION_BINDINGS || !Config::get().automations[idx].exists) {
            auto e = _makeErr("not found");
            _sendJson(r, 404, e);
            return;
        }
        Config::get().automations[idx].exists = false;
        Config::save();
        auto ok = _makeOk();
        _sendJson(r, 200, ok);
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
