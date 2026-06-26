#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <functional>
#include "../config/Config.h"
#include "../mesh/PeerRegistry.h"
#include "../logging/Logger.h"
#include "../version.h"
#include "../scenes/SceneManager.h"
#include "../scenes/SceneSyncManager.h"
#include "../update/Updater.h"

// Called when this device's own group changes
using GroupChangeCb  = std::function<void()>;
// Called when a group's light config changes (groupId, config)
using GroupLightCb   = std::function<void(uint8_t, const LightConfig&)>;
// Called when a group is created/updated/deleted (the full GroupConfig)
using GroupSyncCb    = std::function<void(const GroupConfig&)>;
// Called when we want to move a remote peer to a group
using SetRemoteGroupCb    = std::function<void(const uint8_t* mac, uint8_t groupId)>;
// Called when we want to toggle sceneSyncEnabled on a remote peer
using SetRemoteSyncCb     = std::function<void(const uint8_t* mac, bool enabled)>;
// Called when a conflict is resolved (id, sourceMac — null means local copy wins)
using ResolveConflictCb   = std::function<void(const char* id, const uint8_t* sourceMac)>;
// Called to push syncable config to peers via mesh (targetMac all-zeros = all, json payload)
using PushConfigCb        = std::function<void(const uint8_t* targetMac, const char* json, size_t len)>;
// Called to broadcast a firmware update trigger to a specific peer
using TriggerPeerUpdateCb = std::function<void(const uint8_t* mac)>;

class BatteryWebServer {
private:
    // Logs every incoming request; always returns false so real handlers proceed.
    // Must be added before any routes.
    struct RequestLogger : public AsyncWebHandler {
        bool canHandle(AsyncWebServerRequest* r) const override {
            Logger::d("[web] %s %s", r->methodToString(), r->url().c_str());
            return false;
        }
    };

    struct SceneSaveState {
        String  buffer;
        String  id;
        File    file;
        bool    failed = false;
        bool    written = false;
        const char* error = nullptr;
    };

public:
    void begin(GroupChangeCb onGroupChange, GroupLightCb onGroupLight,
               GroupSyncCb onGroupSync, SetRemoteGroupCb onSetRemote,
               PeerRegistry* peers,
               SceneSyncManager* sceneSync = nullptr,
               SetRemoteSyncCb onSetRemoteSync = nullptr,
               ResolveConflictCb onResolveConflict = nullptr,
               PushConfigCb onPushConfig = nullptr,
               TriggerPeerUpdateCb onTriggerPeerUpdate = nullptr) {
        _onGroupChange      = onGroupChange;
        _onGroupLight       = onGroupLight;
        _onGroupSync        = onGroupSync;
        _onSetRemote        = onSetRemote;
        _peers              = peers;
        _sceneSync          = sceneSync;
        _onSetRemoteSync    = onSetRemoteSync;
        _onResolveConflict    = onResolveConflict;
        _onPushConfig         = onPushConfig;
        _onTriggerPeerUpdate  = onTriggerPeerUpdate;

        Logger::i("[web] starting on port 80");
        _server.addHandler(&_reqLogger);

        _ws = new AsyncWebSocket("/ws");
        _ws->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType t,
                            void*, uint8_t*, size_t) {
            if (t == WS_EVT_CONNECT) {
                Logger::d("[web] WS #%u connected", c->id());
                // Send current state to new client
                _pushPeers();
                _pushGroups();
            }
        });
        _server.addHandler(_ws);

        // API routes must be registered before serveStatic, otherwise the
        // static handler matches /api/* paths and tries to open them from LittleFS.
        _server.on("/api/config", HTTP_GET,  [this](AsyncWebServerRequest* r){ _getConfig(r); });
        _server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _postConfig(r,d,l); });

        _server.on("/api/peers", HTTP_GET, [this](AsyncWebServerRequest* r){ _getPeers(r); });

        _server.on("/api/groups/create", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _createGroup(r,d,l); });
        _server.on("/api/groups/update", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _updateGroup(r,d,l); });
        _server.on("/api/groups/delete", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _deleteGroup(r,d,l); });

        _server.on("/api/peers/setgroup", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _setRemoteGroup(r,d,l); });

        // ── Scene API ─────────────────────────────────────────────────────────
        // Specific routes must be registered before /api/scenes because
        // ESPAsyncWebServer prefix-matches: /api/scenes would otherwise
        // intercept /api/scenes/get, /api/scenes/save, etc.
        SceneManager::init();

        _server.on("/api/scenes/get", HTTP_GET,
            [this](AsyncWebServerRequest* r){ _getScene(r); });

        _server.on("/api/scenes/create", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _createScene(r,d,l); });

        _server.on("/api/scenes/delete", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _deleteScene(r,d,l); });

        // Scene save buffers the full body (may span multiple TCP packets)
        // before writing to LittleFS.
        _server.on("/api/scenes/save", HTTP_POST,
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
                    if (_sceneSync) _sceneSync->onSceneChanged();
                } else {
                    Logger::e("[scene] save failed: %s (failed=%d written=%d)",
                              st->error ? st->error : "?", st->failed, st->written);
                }

                JsonDocument resp;
                if (ok) resp["ok"] = true;
                else    resp["error"] = st->failed ? (st->error ? st->error : "save failed") : "save failed";

                delete st;
                r->_tempObject = nullptr;
                _sendJson(r, ok ? 200 : 500, resp);
            },
            nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
                auto* st = static_cast<SceneSaveState*>(r->_tempObject);
                if (!st) {
                    st = new SceneSaveState();
                    r->_tempObject = st;
                    Logger::d("[scene] save: body start, total=%u bytes", (unsigned)total);
                }

                if (st->failed || !len) return;

                if (!st->file) {
                    st->buffer.concat((char*)data, len);
                    Logger::d("[scene] save: buffering chunk %u bytes (buf=%u): %.80s", (unsigned)len, (unsigned)st->buffer.length(), st->buffer.c_str());
                    String found;
                    if (!SceneManager::extractId(st->buffer.c_str(), st->buffer.length(), found)) {
                        if (st->buffer.length() > 16384) {
                            Logger::e("[scene] save: id not found after %u bytes", (unsigned)st->buffer.length());
                            st->failed = true;
                            st->error = "missing id";
                        } else {
                            Logger::w("[scene] save: extractId returned false for buf=%u bytes (body complete at total=%u)", (unsigned)st->buffer.length(), (unsigned)total);
                        }
                        return;
                    }

                    st->id = found;
                    Logger::d("[scene] save: id=%s, opening file", st->id.c_str());
                    SceneManager::init();
                    st->file = LittleFS.open(SceneManager::path(st->id.c_str()), "w");
                    if (!st->file) {
                        Logger::e("[scene] save: open failed for %s", st->id.c_str());
                        st->failed = true;
                        st->error = "open failed";
                        return;
                    }

                    size_t written = st->file.write((const uint8_t*)st->buffer.c_str(), st->buffer.length());
                    Logger::d("[scene] save: wrote initial buffer %u/%u bytes", (unsigned)written, (unsigned)st->buffer.length());
                    if (written != st->buffer.length()) {
                        Logger::e("[scene] save: initial write incomplete (%u/%u)", (unsigned)written, (unsigned)st->buffer.length());
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
                Logger::d("[scene] save: chunk at index=%u len=%u written=%u", (unsigned)index, (unsigned)len, (unsigned)written);
                if (written != len) {
                    Logger::e("[scene] save: chunk write incomplete (%u/%u) at index=%u", (unsigned)written, (unsigned)len, (unsigned)index);
                    st->failed = true;
                    st->error = "write failed";
                    st->file.close();
                    st->file = File();
                    return;
                }
                st->written = true;
            }
        );

        _server.on("/api/scenes", HTTP_GET,
            [this](AsyncWebServerRequest* r){ _getScenes(r); });

        // ── Scene sync API ────────────────────────────────────────────────────
        _server.on("/api/scenes/sync/conflicts", HTTP_GET,
            [this](AsyncWebServerRequest* r){ _getSyncConflicts(r); });

        _server.on("/api/scenes/sync/resolve", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _resolveSyncConflict(r,d,l); });

        _server.on("/api/peers/setscenesync", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _setRemoteSceneSync(r,d,l); });

        _server.on("/api/peers/pushconfig", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _pushConfig(r,d,l); });

        _server.on("/api/peers/triggerupdate", HTTP_POST, [](AsyncWebServerRequest*){}, nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t, size_t){ _triggerPeerUpdate(r,d,l); });

        _server.on("/api/update/trigger", HTTP_POST, [](AsyncWebServerRequest* r) {
            Updater::triggerAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/update/status", HTTP_GET, [](AsyncWebServerRequest* r) {
            auto& s = Updater::status();
            JsonDocument doc;
            doc["currentVersion"] = s.currentVersion;
            doc["latestVersion"]  = s.latestVersion;
            doc["hasUpdate"]      = s.hasUpdate;
            doc["progress"]       = s.progress;
            const char* stateStr =
                s.state == Updater::State::Checking    ? "checking"    :
                s.state == Updater::State::Downloading ? "downloading" :
                s.state == Updater::State::Error       ? "error"       :
                s.state == Updater::State::Done        ? "done"        : "idle";
            doc["state"] = stateStr;
            if (s.error) doc["error"] = s.error;
            String out; serializeJson(doc, out);
            r->send(200, "application/json", out);
        });

        _server.on("/api/update/check", HTTP_POST, [](AsyncWebServerRequest* r) {
            Updater::checkAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/update/apply", HTTP_POST, [](AsyncWebServerRequest* r) {
            auto& s = Updater::status();
            if (!s.hasUpdate) {
                r->send(400, "application/json", "{\"error\":\"no update available\"}");
                return;
            }
            Updater::applyAsync();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        _server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* r){
            Config::reset();
            r->send(200, "application/json", "{\"ok\":true}");
            delay(500); ESP.restart();
        });

        // Browsers always request a favicon; return 204 so the request doesn't
        // fall through serveStatic's default-file fallback and generate log noise.
        _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(204); });

        // Static files last — catches everything not matched above
        _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        Logger::addSink([this](LogLevel lv, const char* msg){ _pushLog(lv, msg); });

        _server.begin();
        Logger::i("[web] started");
    }

    void loop() { if (_ws) _ws->cleanupClients(); }

    // Called from main when peer list changes (via mesh callback)
    void pushPeers()  { _pushPeers(); }
    void pushGroups() { _pushGroups(); }

private:
    AsyncWebServer   _server{80};
    AsyncWebSocket*  _ws = nullptr;
    RequestLogger    _reqLogger;
    PeerRegistry*    _peers = nullptr;

    GroupChangeCb     _onGroupChange;
    GroupLightCb      _onGroupLight;
    GroupSyncCb       _onGroupSync;
    SetRemoteGroupCb  _onSetRemote;
    SetRemoteSyncCb     _onSetRemoteSync;
    ResolveConflictCb   _onResolveConflict;
    PushConfigCb        _onPushConfig;
    TriggerPeerUpdateCb _onTriggerPeerUpdate;
    SceneSyncManager*   _sceneSync = nullptr;

    // ── helpers ──────────────────────────────────────────────────────────────

    static JsonDocument _makeOk()  { JsonDocument d; d["ok"] = true; return d; }
    static JsonDocument _makeErr(const char* e) { JsonDocument d; d["error"] = e; return d; }

    static void _sendJson(AsyncWebServerRequest* r, int code, JsonDocument& doc) {
        String s; serializeJson(doc, s);
        r->send(code, "application/json", s);
    }

    static void _serializeGroup(JsonObject o, const GroupConfig& g) {
        o["id"]          = g.id;
        o["name"]        = g.name;
        o["exists"]      = g.exists;
        o["syncEnabled"] = g.syncEnabled;
        o["mode"]        = (uint8_t)g.light.mode;
        o["sceneId"]     = g.light.sceneId;
        o["pattern"]     = (uint8_t)g.light.pattern;
        o["r"]           = g.light.color.r;
        o["g"]           = g.light.color.g;
        o["b"]           = g.light.color.b;
        o["brightness"]        = g.light.brightness;
        o["speed"]             = g.light.speed;
        o["transitionEnabled"] = g.light.transitionEnabled;
        o["transitionTime"]    = g.light.transitionTime;
        o["proximityScale"]    = g.light.proximityScale;
    }

    static LightConfig _lightFromJson(JsonVariant j) {
        LightConfig l;
        if (!j["mode"].isNull())        l.mode       = (GroupMode)(uint8_t)j["mode"];
        if (!j["sceneId"].isNull())    strlcpy(l.sceneId, j["sceneId"], sizeof(l.sceneId));
        if (!j["pattern"].isNull())    l.pattern    = (PatternId)(uint8_t)j["pattern"];
        if (!j["r"].isNull())       l.color.r    = j["r"];
        if (!j["g"].isNull())       l.color.g    = j["g"];
        if (!j["b"].isNull())       l.color.b    = j["b"];
        if (!j["brightness"].isNull())        l.brightness        = j["brightness"];
        if (!j["speed"].isNull())             l.speed             = j["speed"];
        if (!j["transitionEnabled"].isNull()) l.transitionEnabled = (bool)j["transitionEnabled"];
        if (!j["transitionTime"].isNull())    l.transitionTime    = (float)j["transitionTime"];
        if (!j["proximityScale"].isNull())    l.proximityScale    = (float)j["proximityScale"];
        return l;
    }

    // ── GET /api/config ──────────────────────────────────────────────────────
    void _getConfig(AsyncWebServerRequest* r) {
        auto& c = Config::get();
        JsonDocument doc;
        doc["deviceName"] = c.deviceName;
        doc["wifiSsid"]   = c.wifiSsid;
        doc["otaPort"]    = c.otaPort;
        doc["otaEnabled"] = c.otaEnabled;
        doc["groupId"]    = c.groupId;
        doc["mac"]        = WiFi.macAddress();
        doc["version"]    = FW_VERSION;
        doc["ledType"]    = (uint8_t)c.ledType;
        doc["dataPin"]    = LED_DATA_PIN;
        doc["clockPin"]   = LED_CLOCK_PIN;
        doc["logLevel"]         = c.logLevel;
        doc["sceneSyncEnabled"] = c.sceneSyncEnabled;
        doc["mqttHost"]   = c.mqttHost;
        doc["mqttPort"]   = c.mqttPort;
        doc["mqttUser"]   = c.mqttUser;
        // mqttPassword intentionally omitted — write-only from UI
        doc["githubRepo"] = c.githubRepo;
        // githubToken intentionally omitted — write-only from UI

        JsonArray arr = doc["groups"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            if (!c.groups[i].exists) continue;
            _serializeGroup(arr.add<JsonObject>(), c.groups[i]);
        }
        _sendJson(r, 200, doc);
    }

    // ── POST /api/config ─────────────────────────────────────────────────────
    void _postConfig(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        auto& c = Config::get();
        if (!doc["deviceName"].isNull())   strlcpy(c.deviceName,   doc["deviceName"],   sizeof(c.deviceName));
        if (!doc["wifiSsid"].isNull())     strlcpy(c.wifiSsid,     doc["wifiSsid"],     sizeof(c.wifiSsid));
        if (!doc["wifiPassword"].isNull()) strlcpy(c.wifiPassword, doc["wifiPassword"], sizeof(c.wifiPassword));
        if (!doc["apPassword"].isNull())   strlcpy(c.apPassword,   doc["apPassword"],   sizeof(c.apPassword));
        if (!doc["otaPort"].isNull())      c.otaPort    = doc["otaPort"];
        if (!doc["otaEnabled"].isNull())   c.otaEnabled = (bool)doc["otaEnabled"];
        if (!doc["ledType"].isNull())      c.ledType  = (LedType)(uint8_t)doc["ledType"];
        if (!doc["logLevel"].isNull()) {
            c.logLevel = (uint8_t)doc["logLevel"];
            Logger::setLevel((LogLevel)c.logLevel);
        }
        if (!doc["sceneSyncEnabled"].isNull()) {
            c.sceneSyncEnabled = (bool)doc["sceneSyncEnabled"];
        }
        if (!doc["mqttHost"].isNull())     strlcpy(c.mqttHost, doc["mqttHost"], sizeof(c.mqttHost));
        if (!doc["mqttPort"].isNull())     c.mqttPort = (uint16_t)doc["mqttPort"];
        if (!doc["mqttUser"].isNull())     strlcpy(c.mqttUser, doc["mqttUser"], sizeof(c.mqttUser));
        if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0)
            strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
        if (!doc["githubRepo"].isNull())  strlcpy(c.githubRepo, doc["githubRepo"], sizeof(c.githubRepo));
        if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
            strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));

        if (!doc["groupId"].isNull()) {
            uint8_t newGroup = doc["groupId"];
            if (newGroup != c.groupId && Config::group(newGroup)) {
                c.groupId = newGroup;
                if (_onGroupChange) _onGroupChange();
            }
        }
        Config::save();
        auto ok = _makeOk(); _sendJson(r, 200, ok);
        delay(200); ESP.restart();
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
        const char* selfFwState =
            us.state == Updater::State::Checking    ? "checking"    :
            us.state == Updater::State::Downloading ? "downloading" :
            us.state == Updater::State::Error       ? "error"       :
            us.state == Updater::State::Done        ? "done"        : "idle";

        auto self = doc["self"].to<JsonObject>();
        self["mac"]           = WiFi.macAddress();
        self["name"]          = c.deviceName;
        self["groupId"]       = c.groupId;
        self["online"]        = true;
        self["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
        self["version"]       = FW_VERSION;
        self["fwState"]       = selfFwState;

        JsonArray arr = doc["peers"].to<JsonArray>();
        if (_peers) {
            for (auto& p : *_peers) {
                if (!p.active) continue;
                const char* pFwState =
                    p.fwState == FwState::Checking    ? "checking"    :
                    p.fwState == FwState::Downloading ? "downloading" :
                    p.fwState == FwState::Error       ? "error"       :
                    p.fwState == FwState::Done        ? "done"        : "idle";
                auto o = arr.add<JsonObject>();
                o["mac"]              = p.macStr();
                o["name"]             = p.name;
                o["groupId"]          = p.groupId;
                o["online"]           = p.online();
                o["rssi"]             = p.rssi;
                o["sceneSyncEnabled"] = p.sceneSyncEnabled;
                o["wifiConnected"]    = p.wifiConnected;
                o["version"]          = p.fwVersion;
                o["fwState"]          = pFwState;
            }
        }
    }

    // ── POST /api/groups/create ──────────────────────────────────────────────
    void _createGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* name = doc["name"] | "New Group";
        uint8_t id = Config::createGroup(name);
        if (id == 0xFF) {
            auto e = _makeErr("group limit reached"); _sendJson(r, 400, e); return;
        }
        Config::save();
        const GroupConfig& g = Config::get().groups[id];
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
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        uint8_t id = doc["id"] | (uint8_t)0;
        GroupConfig* g = Config::group(id);
        if (!g) { auto e = _makeErr("not found"); _sendJson(r, 404, e); return; }

        bool nameChanged = false;
        if (!doc["name"].isNull()) {
            nameChanged = strcmp(g->name, (const char*)doc["name"]) != 0;
            strlcpy(g->name, doc["name"], sizeof(g->name));
        }

        if (!doc["syncEnabled"].isNull()) {
            g->syncEnabled = (bool)doc["syncEnabled"];
            // Propagate the toggle to other devices
            Config::save();
            if (_onGroupSync) _onGroupSync(*g);
            auto ok = _makeOk(); _sendJson(r, 200, ok);
            _pushGroups();
            return;
        }

        bool lightChanged = !doc["mode"].isNull() || !doc["sceneId"].isNull()
                         || !doc["pattern"].isNull() || !doc["r"].isNull()
                         || !doc["g"].isNull()       || !doc["b"].isNull()
                         || !doc["brightness"].isNull() || !doc["speed"].isNull()
                         || !doc["transitionEnabled"].isNull() || !doc["transitionTime"].isNull()
                         || !doc["proximityScale"].isNull();
        if (lightChanged) {
            auto& l = g->light;
            if (!doc["mode"].isNull())             l.mode              = (GroupMode)(uint8_t)doc["mode"];
            if (!doc["sceneId"].isNull())          strlcpy(l.sceneId, doc["sceneId"], sizeof(l.sceneId));
            if (!doc["pattern"].isNull())          l.pattern           = (PatternId)(uint8_t)doc["pattern"];
            if (!doc["r"].isNull())                l.color.r           = doc["r"];
            if (!doc["g"].isNull())                l.color.g           = doc["g"];
            if (!doc["b"].isNull())                l.color.b           = doc["b"];
            if (!doc["brightness"].isNull())       l.brightness        = doc["brightness"];
            if (!doc["speed"].isNull())            l.speed             = doc["speed"];
            if (!doc["transitionEnabled"].isNull()) l.transitionEnabled = (bool)doc["transitionEnabled"];
            if (!doc["transitionTime"].isNull())   l.transitionTime    = (float)doc["transitionTime"];
            if (!doc["proximityScale"].isNull())   l.proximityScale    = (float)doc["proximityScale"];
            l.seq++;
            if (_onGroupLight) _onGroupLight(id, l);
        }
        if (nameChanged) {
            if (_onGroupSync) _onGroupSync(*g);
        }

        Config::save();
        auto ok = _makeOk(); _sendJson(r, 200, ok);
        if (nameChanged) _pushGroups();
    }

    // ── POST /api/groups/delete ──────────────────────────────────────────────
    void _deleteGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        uint8_t id = doc["id"] | (uint8_t)0;
        if (id == 0) { auto e = _makeErr("cannot delete Default"); _sendJson(r, 400, e); return; }
        GroupConfig* g = Config::group(id);
        if (!g) { auto e = _makeErr("not found"); _sendJson(r, 404, e); return; }

        GroupConfig tombstone = *g;
        tombstone.exists = false;
        g->exists = false;

        // Move this device to Default if it was in the deleted group
        if (Config::get().groupId == id) {
            Config::get().groupId = 0;
            if (_onGroupChange) _onGroupChange();
        }

        Config::save();
        if (_onGroupSync) _onGroupSync(tombstone);
        auto ok = _makeOk(); _sendJson(r, 200, ok);
        _pushGroups();
    }

    // ── POST /api/peers/setgroup ─────────────────────────────────────────────
    void _setRemoteGroup(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        uint8_t groupId = doc["groupId"] | (uint8_t)0;
        const char* macStr = doc["mac"] | "";

        // Check if it's this device
        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            if (Config::group(groupId)) {
                Config::get().groupId = groupId;
                Config::save();
                if (_onGroupChange) _onGroupChange();
            }
            auto ok = _makeOk(); _sendJson(r, 200, ok);
            _pushPeers();
            return;
        }

        // Parse mac string to bytes
        uint8_t mac[6];
        if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            auto e = _makeErr("bad mac"); _sendJson(r, 400, e); return;
        }

        if (_onSetRemote) _onSetRemote(mac, groupId);
        auto ok = _makeOk(); _sendJson(r, 200, ok);
    }

    // ── WebSocket push ───────────────────────────────────────────────────────
    void _pushPeers() {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "peers";
        _buildPeersJson(doc);
        String s; serializeJson(doc, s);
        _ws->textAll(s);
    }

    void _pushGroups() {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["t"] = "groups";
        JsonArray arr = doc["list"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            auto& g = Config::get().groups[i];
            if (!g.exists) continue;
            _serializeGroup(arr.add<JsonObject>(), g);
        }
        String s; serializeJson(doc, s);
        _ws->textAll(s);
    }

    void _pushLog(LogLevel level, const char* msg) {
        if (!_ws || _ws->count() == 0) return;
        const char* lv = level == LogLevel::ERROR ? "E"
                       : level == LogLevel::WARN  ? "W"
                       : level == LogLevel::INFO  ? "I" : "D";
        char buf[320];
        snprintf(buf, sizeof(buf), "{\"t\":\"log\",\"l\":\"%s\",\"m\":%s}",
                 lv, _jsonStr(msg).c_str());
        _ws->textAll(buf);
    }

    // ── Scene handlers ───────────────────────────────────────────────────────

    void _getScenes(AsyncWebServerRequest* r) {
        Logger::d("[scene] list requested");
        JsonDocument resp;
        SceneManager::buildList(resp);
        JsonArray arr = resp["scenes"].as<JsonArray>();
        Logger::d("[scene] list: %u scene(s)", arr ? (unsigned)arr.size() : 0);
        _sendJson(r, 200, resp);
    }

    void _getScene(AsyncWebServerRequest* r) {
        if (!r->hasParam("id")) {
            Logger::w("[scene] get: missing id param");
            auto e = _makeErr("missing id"); _sendJson(r, 400, e); return;
        }
        String id   = r->getParam("id")->value();
        String path = SceneManager::path(id.c_str());
        Logger::d("[scene] get: id=%s path=%s exists=%d", id.c_str(), path.c_str(), LittleFS.exists(path));
        if (!LittleFS.exists(path)) {
            auto e = _makeErr("not found"); _sendJson(r, 404, e); return;
        }
        r->send(LittleFS, path, "application/json");
    }

    void _createScene(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            Logger::e("[scene] create: bad json");
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* name = doc["name"] | "Unnamed";
        uint16_t w = doc["w"] | 20;
        uint16_t h = doc["h"] | 10;
        Logger::i("[scene] create: name=%s w=%u h=%u", name, w, h);
        String id = SceneManager::create(name, w, h);
        if (id.isEmpty()) {
            Logger::e("[scene] create: failed");
            auto e = _makeErr("create failed"); _sendJson(r, 500, e); return;
        }
        Logger::i("[scene] create: ok id=%s", id.c_str());
        if (_sceneSync) _sceneSync->onSceneChanged();
        JsonDocument resp;
        resp["ok"] = true;
        resp["id"] = id;
        _sendJson(r, 200, resp);
    }

    void _deleteScene(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            Logger::e("[scene] delete: bad json");
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* id = doc["id"] | "";
        if (!id[0]) {
            Logger::w("[scene] delete: missing id");
            auto e = _makeErr("missing id"); _sendJson(r, 400, e); return;
        }
        Logger::i("[scene] delete: id=%s", id);
        bool ok = _sceneSync ? _sceneSync->deleteScene(id) : SceneManager::remove(id);
        Logger::i("[scene] delete: %s", ok ? "ok" : "not found");
        JsonDocument resp;
        if (ok) resp["ok"] = true; else resp["error"] = "not found";
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
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* id      = doc["id"] | "";
        const char* macStr  = doc["sourceMac"] | "";
        if (!id[0]) { auto e = _makeErr("missing id"); _sendJson(r, 400, e); return; }

        // Determine if sourceMac is this device or a remote one
        bool isLocal = !macStr[0] || WiFi.macAddress().equalsIgnoreCase(macStr);

        if (isLocal) {
            if (_onResolveConflict) _onResolveConflict(id, nullptr);
        } else {
            uint8_t mac[6];
            if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                       &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
                auto e = _makeErr("bad mac"); _sendJson(r, 400, e); return;
            }
            if (_onResolveConflict) _onResolveConflict(id, mac);
        }
        auto ok = _makeOk(); _sendJson(r, 200, ok);
    }

    // Body: {mac?, deviceName?, ledType?, wifiSsid?, wifiPassword?, apPassword?,
    //        mqttHost?, mqttPort?, mqttUser?, mqttPassword?, githubRepo?, githubToken?,
    //        otaEnabled?, sceneSyncEnabled?}
    // mac omitted or empty = push to all peers. Only present fields are pushed;
    // deviceName and ledType require a specific target mac.
    void _pushConfig(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* macStr = doc["mac"] | "";
        uint8_t targetMac[6] = {0, 0, 0, 0, 0, 0};
        bool hasTarget = macStr[0] != '\0';
        if (hasTarget) {
            if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                       &targetMac[0], &targetMac[1], &targetMac[2],
                       &targetMac[3], &targetMac[4], &targetMac[5]) != 6) {
                auto e = _makeErr("bad mac"); _sendJson(r, 400, e); return;
            }
        }

        JsonDocument payload;
        bool any = false;

        auto addStr = [&](const char* key, size_t minLen = 0) {
            if (doc[key].isNull()) return;
            const char* v = doc[key] | "";
            if (strlen(v) < minLen) return;
            payload[key] = v; any = true;
        };
        auto addNum = [&](const char* key) {
            if (doc[key].isNull()) return;
            payload[key] = doc[key]; any = true;
        };
        auto addBool = [&](const char* key) {
            if (doc[key].isNull()) return;
            payload[key] = (bool)doc[key]; any = true;
        };

        // Per-device fields — only allowed when targeting a specific device
        if (hasTarget) {
            if (!doc["deviceName"].isNull()) {
                const char* newName = doc["deviceName"] | "";
                if (newName[0] != '\0') {
                    // Uniqueness check against peer registry and own name
                    if (strcmp(Config::get().deviceName, newName) == 0) {
                        auto e = _makeErr("name already in use"); _sendJson(r, 409, e); return;
                    }
                    if (_peers) {
                        for (auto& p : *_peers) {
                            if (!p.active || memcmp(p.mac, targetMac, 6) == 0) continue;
                            if (strcmp(p.name, newName) == 0) {
                                auto e = _makeErr("name already in use"); _sendJson(r, 409, e); return;
                            }
                        }
                    }
                    payload["deviceName"] = newName; any = true;
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
        // Shared fields (non-sensitive values arrive directly; sensitive ones via useLocal)
        addStr("wifiSsid");
        if (isUseLocal("wifiPassword")) { payload["wifiPassword"] = c.wifiPassword; any = true; }
        else addStr("wifiPassword", 1);
        if (isUseLocal("apPassword")) { if (strlen(c.apPassword) >= 8) { payload["apPassword"] = c.apPassword; any = true; } }
        else addStr("apPassword", 8);
        addStr("mqttHost");
        addNum("mqttPort");
        addStr("mqttUser");
        if (isUseLocal("mqttPassword")) { payload["mqttPassword"] = c.mqttPassword; any = true; }
        else addStr("mqttPassword", 1);
        addStr("githubRepo");
        if (isUseLocal("githubToken")) { payload["githubToken"] = c.githubToken; any = true; }
        else addStr("githubToken", 1);
        addBool("otaEnabled");
        addBool("sceneSyncEnabled");

        // Per-device: ledType arrives directly (value selected in UI)
        if (hasTarget) addNum("ledType");

        if (!any) {
            auto e = _makeErr("no fields to push"); _sendJson(r, 400, e); return;
        }

        String json;
        serializeJson(payload, json);
        if (_onPushConfig) _onPushConfig(targetMac, json.c_str(), json.length());
        auto ok = _makeOk(); _sendJson(r, 200, ok);
    }

    // Body: {mac, enabled}
    void _setRemoteSceneSync(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* macStr = doc["mac"] | "";
        bool enabled       = doc["enabled"] | true;

        if (WiFi.macAddress().equalsIgnoreCase(macStr)) {
            Config::get().sceneSyncEnabled = enabled;
            Config::save();
            auto ok = _makeOk(); _sendJson(r, 200, ok);
            return;
        }

        uint8_t mac[6];
        if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            auto e = _makeErr("bad mac"); _sendJson(r, 400, e); return;
        }
        if (_onSetRemoteSync) _onSetRemoteSync(mac, enabled);
        auto ok = _makeOk(); _sendJson(r, 200, ok);
    }

    // ── POST /api/peers/triggerupdate ─────────────────────────────────────────
    void _triggerPeerUpdate(AsyncWebServerRequest* r, uint8_t* data, size_t len) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            auto e = _makeErr("bad json"); _sendJson(r, 400, e); return;
        }
        const char* macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            auto e = _makeErr("bad mac"); _sendJson(r, 400, e); return;
        }
        Logger::i("[web] trigger-update for %s", macStr);
        if (_onTriggerPeerUpdate) _onTriggerPeerUpdate(mac);
        auto ok = _makeOk(); _sendJson(r, 200, ok);
    }

    static String _jsonStr(const char* s) {
        String o = "\"";
        for (; *s; ++s) {
            if (*s == '"')       o += "\\\"";
            else if (*s == '\\') o += "\\\\";
            else if (*s == '\n') o += "\\n";
            else                 o += *s;
        }
        return o + '"';
    }
};
