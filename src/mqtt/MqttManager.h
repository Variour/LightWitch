#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../scenes/SceneManager.h"
#include "../update/Updater.h"

// Publishes/subscribes one MQTT "light" per group (not just the first one),
// plus per-light brightness-override entities (switch + number), a Home
// Assistant "update" entity wired to the existing GitHub-release OTA flow,
// and a device-wide "scene sync" switch. Every entity is announced via HA
// MQTT discovery.
// Mirrors WebServer.h's group/light update handlers (_updateGroup/_updateLight)
// rather than ActionExecutor — MQTT commands are absolute "set" operations,
// not the relative step/toggle actions ActionExecutor models for buttons.
class MqttManager {
public:
    using GroupApplyFn      = std::function<void(uint8_t groupId, const LightConfig&)>;
    using GroupSyncToggleFn = std::function<void(const GroupConfig&)>;
    using LightOverrideFn   = std::function<void(uint8_t lightIndex)>;
    using SceneSyncToggleFn = std::function<void()>;

    // Applies + propagates a group's LightConfig change (e.g. main.cpp's
    // applyAndPropagateLightConfig).
    void setOnGroupLight(GroupApplyFn fn) { _onGroupLight = fn; }
    // A group's syncEnabled was toggled via MQTT (broadcast over mesh).
    void setOnGroupSyncToggle(GroupSyncToggleFn fn) { _onGroupSyncToggle = fn; }
    // A light's brightness override or group assignment changed (re-apply to its runner).
    void setOnLightOverride(LightOverrideFn fn) { _onLightOverride = fn; }
    // The device's own sceneSyncEnabled was just turned on via MQTT (was off before) —
    // mirrors WebServer's _postConfig/_setRemoteSceneSync: kick off a re-sync.
    void setOnSceneSyncEnabled(SceneSyncToggleFn fn) { _onSceneSyncEnabled = fn; }

    void begin(const DeviceConfig& cfg) {
        if (strlen(cfg.mqttHost) == 0) return;

        strlcpy(_deviceName, cfg.deviceName, sizeof(_deviceName));
        strlcpy(_host,       cfg.mqttHost,   sizeof(_host));
        _port = cfg.mqttPort;
        strlcpy(_user, cfg.mqttUser,     sizeof(_user));
        strlcpy(_pass, cfg.mqttPassword, sizeof(_pass));

        // Derive a stable unique ID from the MAC
        uint8_t mac[6]; WiFi.macAddress(mac);
        snprintf(_uniqueId, sizeof(_uniqueId), "bl_%02x%02x%02x", mac[3], mac[4], mac[5]);

        char base[96];
        snprintf(base, sizeof(base), "batterylight/%s", _deviceName);
        snprintf(_groupPrefix,     sizeof(_groupPrefix),     "%s/group/",       base);
        snprintf(_lightPrefix,     sizeof(_lightPrefix),     "%s/light/",       base);
        snprintf(_updateSetTopic,  sizeof(_updateSetTopic),  "%s/update/set",   base);
        snprintf(_updateStateTopic,sizeof(_updateStateTopic),"%s/update/state", base);
        snprintf(_sceneSyncSetTopic,  sizeof(_sceneSyncSetTopic),  "%s/scenesync/set",   base);
        snprintf(_sceneSyncStateTopic,sizeof(_sceneSyncStateTopic),"%s/scenesync/state", base);
        snprintf(_groupSubWildcard,sizeof(_groupSubWildcard),"%s+/set", _groupPrefix);
        snprintf(_lightSubWildcard,sizeof(_lightSubWildcard),"%s+/set", _lightPrefix);

        _client.setClient(_wifi);
        _client.setServer(_host, _port);
        _client.setKeepAlive(30);
        _client.setBufferSize(4096);
        _instance = this;
        _client.setCallback([](char* t, uint8_t* p, unsigned int l) {
            if (_instance) _instance->_handleMsg(t, p, l);
        });

        _enabled = true;
        Logger::i("[mqtt] enabled — broker %s:%u  client %s", _host, _port, _deviceName);
        _connect();
    }

    void loop() {
        if (!_enabled) return;
        if (!_client.connected()) {
            uint32_t now = millis();
            if (now - _lastAttempt > 10000) { _lastAttempt = now; _connect(); }
            return;
        }
        _client.loop();

        if (_discoveryDirty) { _discoveryDirty = false; _publishAllDiscovery(); }
        for (uint8_t i = 0; i < MAX_GROUPS; i++)
            if (_groupDirty[i]) { _groupDirty[i] = false; _doPublishGroup(i); }
        for (uint8_t i = 0; i < MAX_LIGHTS; i++)
            if (_lightDirty[i]) { _lightDirty[i] = false; _doPublishLight(i); }
        if (_updateDirty) { _updateDirty = false; _doPublishUpdate(); }
        if (_sceneSyncDirty) { _sceneSyncDirty = false; _doPublishSceneSync(); }
    }

    // All publish* calls are safe from any context (including from within
    // _handleMsg itself) — they only flip a dirty flag; the actual
    // _client.publish() happens from loop(), never reentrantly from inside
    // PubSubClient's own callback dispatch.
    void publishGroupState(uint8_t groupId)        { if (groupId    < MAX_GROUPS) _groupDirty[groupId]    = true; }
    void publishLightOverride(uint8_t lightIndex)  { if (lightIndex < MAX_LIGHTS) _lightDirty[lightIndex] = true; }
    void publishUpdateState()                      { _updateDirty = true; }
    // Call after Config::get().sceneSyncEnabled changes via any non-MQTT path
    // (web UI, peer-toggle-self) so the retained MQTT state doesn't go stale.
    void publishSceneSyncState()                    { _sceneSyncDirty = true; }
    // Republishes HA discovery (incl. the scene-derived effect list) for
    // every existing group — call after a group or scene is added/renamed/
    // removed/deleted.
    void resyncGroupDiscovery()                    { _discoveryDirty = true; }

    bool connected() { return _enabled && _client.connected(); }
    bool enabled() const { return _enabled; }

    // Clears every retained topic this device may have published — state,
    // update state, and HA discovery — then disconnects and disables MQTT
    // for the rest of this session. Clears every possible group/light slot
    // (not just the ones that currently exist), since a since-deleted
    // group's stale discovery entry is never otherwise cleaned up.
    // Connects using the still-configured broker first if not already
    // connected — the caller is expected to wipe the broker config in
    // Config *after* this returns. No-op if MQTT was never configured.
    void clearRetainedAndDisable() {
        if (strlen(_host) == 0) { _enabled = false; return; }
        if (!_client.connected()) {
            bool ok = (strlen(_user) > 0)
                ? _client.connect(_deviceName, _user, _pass)
                : _client.connect(_deviceName, nullptr, nullptr);
            if (!ok) {
                Logger::w("[mqtt] clear: connect failed rc=%d — retained messages left in place", _client.state());
                _enabled = false;
                return;
            }
        }

        _client.publish(_updateStateTopic, "", /*retain=*/true);
        char topic[128];
        snprintf(topic, sizeof(topic), "homeassistant/update/%s/fw/config", _uniqueId);
        _client.publish(topic, "", /*retain=*/true);

        _client.publish(_sceneSyncStateTopic, "", /*retain=*/true);
        _sceneSyncDiscoveryTopic(topic, sizeof(topic));
        _client.publish(topic, "", /*retain=*/true);

        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            char t[112];
            _groupTopic(i, "state", t, sizeof(t));
            _client.publish(t, "", /*retain=*/true);
            _groupDiscoveryTopic(i, topic, sizeof(topic));
            _client.publish(topic, "", /*retain=*/true);
            _groupTime24hDiscoveryTopic(i, topic, sizeof(topic));
            _client.publish(topic, "", /*retain=*/true);
        }
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            char t[112];
            _lightTopic(i, "state", t, sizeof(t));
            _client.publish(t, "", /*retain=*/true);
            _lightSwitchDiscoveryTopic(i, topic, sizeof(topic));
            _client.publish(topic, "", /*retain=*/true);
            _lightNumberDiscoveryTopic(i, topic, sizeof(topic));
            _client.publish(topic, "", /*retain=*/true);
            _lightGroupDiscoveryTopic(i, topic, sizeof(topic));
            _client.publish(topic, "", /*retain=*/true);
        }

        Logger::i("[mqtt] retained messages cleared, disabling");
        _client.disconnect();
        _enabled = false;
    }

    // Clears one group's retained state + HA discovery topics — call right
    // after a group is deleted (locally or via mesh sync), since Config no
    // longer has it by then, so the normal publishGroupState()/loop() flush
    // path silently no-ops for it (_doPublishGroup bails when Config::group()
    // returns null) and would otherwise leave the group's entities stuck in
    // Home Assistant / retained on the broker forever. Unlike the other
    // publish* setters, this publishes immediately rather than deferring to
    // loop() — callers must not invoke it from within _handleMsg (no group
    // command currently deletes a group, so this isn't reachable from there).
    void clearGroupRetained(uint8_t groupId) {
        if (!_enabled || !_client.connected() || groupId >= MAX_GROUPS) return;
        char t[112];
        _groupTopic(groupId, "state", t, sizeof(t));
        _client.publish(t, "", /*retain=*/true);
        char topic[128];
        _groupDiscoveryTopic(groupId, topic, sizeof(topic));
        _client.publish(topic, "", /*retain=*/true);
        _groupTime24hDiscoveryTopic(groupId, topic, sizeof(topic));
        _client.publish(topic, "", /*retain=*/true);
        Logger::i("[mqtt] cleared retained topics for deleted group %u", groupId);
    }

private:
    inline static MqttManager* _instance = nullptr;

    static constexpr const char* kPatternNames[5] = {
        "Static", "Breathing", "Color Cycle", "Strobe", "Candle"
    };

    WiFiClient   _wifi;
    PubSubClient _client{_wifi};

    GroupApplyFn      _onGroupLight;
    GroupSyncToggleFn _onGroupSyncToggle;
    LightOverrideFn   _onLightOverride;
    SceneSyncToggleFn _onSceneSyncEnabled;

    bool     _enabled     = false;
    uint32_t _lastAttempt = 0;

    bool _discoveryDirty            = false;
    bool _groupDirty[MAX_GROUPS]    = {};
    bool _lightDirty[MAX_LIGHTS]    = {};
    // Last non-zero brightness observed per group (from any source, via
    // _doPublishGroup) — restores HA's on/off toggle, which sends a bare
    // {"state":"ON"} with no brightness and would otherwise leave the group
    // at brightness 0 forever. 0 means "never observed"; falls back to 255.
    uint8_t _lastOnBrightness[MAX_GROUPS] = {};
    bool _updateDirty               = false;
    bool _sceneSyncDirty             = false;

    char     _deviceName[32] = {};
    char     _uniqueId[24]   = {};
    char     _host[64]       = {};
    uint16_t _port           = 1883;
    char     _user[32]       = {};
    char     _pass[64]       = {};

    char _groupPrefix[80]       = {};   // "batterylight/<dev>/group/"
    char _lightPrefix[80]       = {};   // "batterylight/<dev>/light/"
    char _groupSubWildcard[96]  = {};   // "batterylight/<dev>/group/+/set"
    char _lightSubWildcard[96]  = {};   // "batterylight/<dev>/light/+/set"
    char _updateSetTopic[112]   = {};
    char _updateStateTopic[112] = {};
    char _sceneSyncSetTopic[112]   = {};
    char _sceneSyncStateTopic[112] = {};

    void _groupTopic(uint8_t id, const char* suffix, char* out, size_t outLen) {
        snprintf(out, outLen, "%s%u/%s", _groupPrefix, id, suffix);
    }
    void _lightTopic(uint8_t idx, const char* suffix, char* out, size_t outLen) {
        snprintf(out, outLen, "%s%u/%s", _lightPrefix, idx, suffix);
    }
    void _groupDiscoveryTopic(uint8_t id, char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/light/%s/g%u/config", _uniqueId, id);
    }
    void _groupTime24hDiscoveryTopic(uint8_t id, char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/switch/%s/g%u_time24h/config", _uniqueId, id);
    }
    void _lightSwitchDiscoveryTopic(uint8_t idx, char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/switch/%s/l%u_briEn/config", _uniqueId, idx);
    }
    void _lightNumberDiscoveryTopic(uint8_t idx, char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/number/%s/l%u_bri/config", _uniqueId, idx);
    }
    void _lightGroupDiscoveryTopic(uint8_t idx, char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/select/%s/l%u_group/config", _uniqueId, idx);
    }
    void _sceneSyncDiscoveryTopic(char* out, size_t outLen) {
        snprintf(out, outLen, "homeassistant/switch/%s/scenesync/config", _uniqueId);
    }

    void _connect() {
        Logger::i("[mqtt] connecting to %s:%u ...", _host, _port);
        bool ok = (strlen(_user) > 0)
            ? _client.connect(_deviceName, _user, _pass)
            : _client.connect(_deviceName, nullptr, nullptr);
        if (!ok) {
            Logger::w("[mqtt] connect failed rc=%d — retry in 10s", _client.state());
            return;
        }
        Logger::i("[mqtt] connected");
        _client.subscribe(_groupSubWildcard);
        _client.subscribe(_lightSubWildcard);
        _client.subscribe(_updateSetTopic);
        _client.subscribe(_sceneSyncSetTopic);
        _publishAllDiscovery();
        for (uint8_t i = 0; i < MAX_GROUPS; i++) if (Config::group(i)) _doPublishGroup(i);
        Config::forEachLight([this](uint8_t i, LightHardwareConfig&) { _doPublishLight(i); });
        _doPublishUpdate();
        _doPublishSceneSync();
    }

    // ── Discovery ────────────────────────────────────────────────────────────

    void _publishAllDiscovery() {
        for (uint8_t i = 0; i < MAX_GROUPS; i++) {
            GroupConfig* g = Config::group(i);
            if (g) _publishGroupDiscovery(i, *g);
        }
        Config::forEachLight([this](uint8_t i, LightHardwareConfig&) { _publishLightDiscovery(i); });
        _publishUpdateDiscovery();
        _publishSceneSyncDiscovery();
        Logger::i("[mqtt] discovery published");
    }

    // Static pattern names + fixed non-scene modes, plus two entries per
    // existing scene ("Scene: X" / "Gradient: X" — scenes and gradients
    // share the same underlying files, see LightConfig::sceneId). Rebuilt
    // from LittleFS on every discovery publish, so it always reflects the
    // current scene list; callers must resyncGroupDiscovery() after any
    // scene create/rename/delete.
    void _buildEffectList(JsonArray fx) {
        for (auto name : kPatternNames) fx.add(name);
        fx.add("Proximity"); fx.add("Text"); fx.add("Time");
        JsonDocument sc;
        SceneManager::buildList(sc);
        for (JsonObject o : sc["scenes"].as<JsonArray>()) {
            const char* name = o["name"] | "";
            if (!name[0]) continue;
            // ArduinoJson doesn't copy char*/const char* by default (only String/
            // std::string) — wrap in String so each entry keeps its own text.
            fx.add(String("Scene: ")    + name);
            fx.add(String("Gradient: ") + name);
        }
    }

    void _publishGroupDiscovery(uint8_t id, const GroupConfig& g) {
        char discTopic[128];
        _groupDiscoveryTopic(id, discTopic, sizeof(discTopic));
        char stateTopic[112], setTopic[112];
        _groupTopic(id, "state", stateTopic, sizeof(stateTopic));
        _groupTopic(id, "set",   setTopic,   sizeof(setTopic));

        JsonDocument doc;
        doc["name"] = g.name;
        char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_g%u", _uniqueId, id);
        doc["unique_id"]  = String(uniq);
        doc["schema"]     = "json";
        doc["state_topic"]           = String(stateTopic);
        doc["command_topic"]         = String(setTopic);
        doc["brightness"] = true;
        doc["color_mode"] = true;
        doc["supported_color_modes"].to<JsonArray>().add("rgb");
        doc["effect"] = true;
        _buildEffectList(doc["effect_list"].to<JsonArray>());
        auto dev = doc["device"].to<JsonObject>();
        dev["name"]         = _deviceName;
        dev["model"]        = "Battery Light";
        dev["manufacturer"] = "DIY";
        dev["identifiers"].to<JsonArray>().add(_uniqueId);

        String s; serializeJson(doc, s);
        _client.publish(discTopic, s.c_str(), /*retain=*/true);

        _publishGroupTime24hDiscovery(id, g, stateTopic, setTopic);
    }

    // The group's Time-mode 12h/24h display setting, exposed as a switch
    // sharing the group's state/set topic. "time24h" is already a plain
    // field on the group's JSON light config (see serializeLightConfig /
    // deserializeLightConfig in Config.cpp), so no extra command handling
    // is needed — _handleGroupSet already merges it in via
    // deserializeLightConfig(doc, g->light), leaving every other field
    // untouched.
    void _publishGroupTime24hDiscovery(uint8_t id, const GroupConfig& g, const char* stateTopic, const char* setTopic) {
        char discTopic[128];
        _groupTime24hDiscoveryTopic(id, discTopic, sizeof(discTopic));

        JsonDocument doc;
        doc["name"] = String(g.name) + ": Use 24h Time";
        char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_g%u_time24h", _uniqueId, id);
        doc["unique_id"]        = String(uniq);
        doc["state_topic"]      = String(stateTopic);
        doc["command_topic"]    = String(setTopic);
        doc["value_template"]   = "{{ 'ON' if value_json.time24h else 'OFF' }}";
        doc["command_template"] = "{\"time24h\": {{ (value == \"ON\") | lower }}}";
        auto dev = doc["device"].to<JsonObject>();
        dev["name"]         = _deviceName;
        dev["model"]        = "Battery Light";
        dev["manufacturer"] = "DIY";
        dev["identifiers"].to<JsonArray>().add(_uniqueId);

        String s; serializeJson(doc, s);
        _client.publish(discTopic, s.c_str(), /*retain=*/true);
    }

    // A physical light's brightness override and group assignment, exposed
    // as three entities that share its state/set topic: a switch for
    // brightnessOverrideEnabled, a 0-255 number for brightnessOverride, and
    // a select for which group it belongs to. command_template wraps each
    // entity's raw HA payload into the JSON _handleLightSet expects;
    // value_template picks the matching field back out of the shared state.
    void _publishLightDiscovery(uint8_t idx) {
        char stateTopic[112], setTopic[112];
        _lightTopic(idx, "state", stateTopic, sizeof(stateTopic));
        _lightTopic(idx, "set",   setTopic,   sizeof(setTopic));
        auto dev = [&](JsonDocument& doc) {
            auto d = doc["device"].to<JsonObject>();
            d["name"]         = _deviceName;
            d["model"]        = "Battery Light";
            d["manufacturer"] = "DIY";
            d["identifiers"].to<JsonArray>().add(_uniqueId);
        };
        // Falls back to "Light <idx>" — the light's own name (set in the web
        // UI) is optional and empty by default.
        const char* lightName = Config::get().lights[idx].name;
        char label[32];
        if (lightName[0]) strlcpy(label, lightName, sizeof(label));
        else              snprintf(label, sizeof(label), "Light %u", idx);

        {
            char discTopic[128];
            _lightSwitchDiscoveryTopic(idx, discTopic, sizeof(discTopic));
            JsonDocument doc;
            char name[64]; snprintf(name, sizeof(name), "%s: Brightness Override Enabled", label);
            doc["name"] = String(name);
            char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_l%u_briEn", _uniqueId, idx);
            doc["unique_id"]       = String(uniq);
            doc["state_topic"]     = String(stateTopic);
            doc["command_topic"]   = String(setTopic);
            doc["value_template"]  = "{{ 'ON' if value_json.brightnessOverrideEnabled else 'OFF' }}";
            doc["command_template"] = "{\"brightnessOverrideEnabled\": {{ (value == \"ON\") | lower }}}";
            dev(doc);
            String s; serializeJson(doc, s);
            _client.publish(discTopic, s.c_str(), /*retain=*/true);
        }
        {
            char discTopic[128];
            _lightNumberDiscoveryTopic(idx, discTopic, sizeof(discTopic));
            JsonDocument doc;
            char name[64]; snprintf(name, sizeof(name), "%s: Brightness Override", label);
            doc["name"] = String(name);
            char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_l%u_bri", _uniqueId, idx);
            doc["unique_id"]       = String(uniq);
            doc["state_topic"]     = String(stateTopic);
            doc["command_topic"]   = String(setTopic);
            doc["value_template"]  = "{{ value_json.brightnessOverride }}";
            doc["command_template"] = "{\"brightnessOverride\": {{ value }}}";
            doc["min"]  = 0;
            doc["max"]  = 255;
            doc["step"] = 1;
            dev(doc);
            String s; serializeJson(doc, s);
            _client.publish(discTopic, s.c_str(), /*retain=*/true);
        }
        {
            // Options/state are "<id>: <name>" strings so the command_template
            // can recover the numeric groupId with a plain split — avoids
            // having to build a name<->id Jinja lookup (and escape group
            // names, which are free-text) inside the template itself.
            char discTopic[128];
            _lightGroupDiscoveryTopic(idx, discTopic, sizeof(discTopic));
            JsonDocument doc;
            char name[64]; snprintf(name, sizeof(name), "%s: Group", label);
            doc["name"] = String(name);
            char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_l%u_group", _uniqueId, idx);
            doc["unique_id"]       = String(uniq);
            doc["state_topic"]     = String(stateTopic);
            doc["command_topic"]   = String(setTopic);
            doc["value_template"]  = "{{ value_json.group }}";
            doc["command_template"] = "{\"groupId\": {{ value.split(':')[0] | int }}}";
            JsonArray opts = doc["options"].to<JsonArray>();
            for (uint8_t g = 0; g < MAX_GROUPS; g++) {
                GroupConfig* gc = Config::group(g);
                if (!gc) continue;
                char opt[48]; snprintf(opt, sizeof(opt), "%u: %s", g, gc->name);
                opts.add(String(opt));
            }
            dev(doc);
            String s; serializeJson(doc, s);
            _client.publish(discTopic, s.c_str(), /*retain=*/true);
        }
    }

    void _publishUpdateDiscovery() {
        char discTopic[128];
        snprintf(discTopic, sizeof(discTopic), "homeassistant/update/%s/fw/config", _uniqueId);

        JsonDocument doc;
        doc["name"] = "Firmware";
        char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_fw", _uniqueId);
        doc["unique_id"]     = String(uniq);
        doc["state_topic"]   = _updateStateTopic;
        doc["command_topic"] = _updateSetTopic;
        doc["payload_install"] = "INSTALL";
        doc["device_class"]  = "firmware";
        doc["entity_category"] = "config";
        auto dev = doc["device"].to<JsonObject>();
        dev["name"]         = _deviceName;
        dev["model"]        = "Battery Light";
        dev["manufacturer"] = "DIY";
        dev["identifiers"].to<JsonArray>().add(_uniqueId);

        String s; serializeJson(doc, s);
        _client.publish(discTopic, s.c_str(), /*retain=*/true);
    }

    // The device's own sceneSyncEnabled toggle — a plain on/off switch with
    // its own dedicated topic (unlike time24h, it has no group to piggyback
    // a shared state/set topic on).
    void _publishSceneSyncDiscovery() {
        char discTopic[128];
        _sceneSyncDiscoveryTopic(discTopic, sizeof(discTopic));

        JsonDocument doc;
        doc["name"] = "Scene Sync";
        char uniq[48]; snprintf(uniq, sizeof(uniq), "%s_scenesync", _uniqueId);
        doc["unique_id"]       = String(uniq);
        doc["state_topic"]     = _sceneSyncStateTopic;
        doc["command_topic"]   = _sceneSyncSetTopic;
        doc["entity_category"] = "config";
        auto dev = doc["device"].to<JsonObject>();
        dev["name"]         = _deviceName;
        dev["model"]        = "Battery Light";
        dev["manufacturer"] = "DIY";
        dev["identifiers"].to<JsonArray>().add(_uniqueId);

        String s; serializeJson(doc, s);
        _client.publish(discTopic, s.c_str(), /*retain=*/true);
    }

    // ── State publish ────────────────────────────────────────────────────────

    // Resolves the current mode/pattern/sceneId to the matching effect_list
    // display string, so HA's effect dropdown shows the right selection.
    String _effectDisplayName(const LightConfig& l) {
        if (l.mode == GroupMode::Pattern)   return kPatternNames[(uint8_t)l.pattern];
        if (l.mode == GroupMode::Proximity) return "Proximity";
        if (l.mode == GroupMode::Text)      return "Text";
        if (l.mode == GroupMode::Time)      return "Time";
        JsonDocument sc;
        SceneManager::buildList(sc);
        for (JsonObject o : sc["scenes"].as<JsonArray>()) {
            if (strcmp(o["id"] | "", l.sceneId) == 0) {
                const char* prefix = l.mode == GroupMode::Scene ? "Scene: " : "Gradient: ";
                return String(prefix) + (const char*)(o["name"] | "");
            }
        }
        return l.mode == GroupMode::Scene ? "Scene" : "Gradient";
    }

    void _doPublishGroup(uint8_t id) {
        GroupConfig* g = Config::group(id);
        if (!g) return;
        if (g->light.brightness > 0) _lastOnBrightness[id] = g->light.brightness;
        JsonDocument doc;
        JsonObject o = doc.to<JsonObject>();
        serializeGroup(o, *g);
        // HA json-light-schema convenience keys, layered on top of the raw fields.
        o["state"]      = g->light.brightness > 0 ? "ON" : "OFF";
        o["color_mode"] = "rgb";
        auto c = o["color"].to<JsonObject>();
        c["r"] = g->light.color.r; c["g"] = g->light.color.g; c["b"] = g->light.color.b;
        o["effect"] = _effectDisplayName(g->light);

        char topic[112]; _groupTopic(id, "state", topic, sizeof(topic));
        String s; serializeJson(doc, s);
        _client.publish(topic, s.c_str(), /*retain=*/true);
    }

    void _doPublishLight(uint8_t idx) {
        if (idx >= MAX_LIGHTS) return;
        auto& l = Config::get().lights[idx];
        if (!l.exists) return;
        JsonDocument doc;
        doc["brightnessOverrideEnabled"] = l.brightnessOverrideEnabled;
        doc["brightnessOverride"]        = l.brightnessOverride;
        GroupConfig* g = Config::group(l.groupId);
        char group[48]; snprintf(group, sizeof(group), "%u: %s", l.groupId, g ? g->name : "");
        doc["group"] = String(group);
        char topic[112]; _lightTopic(idx, "state", topic, sizeof(topic));
        String s; serializeJson(doc, s);
        _client.publish(topic, s.c_str(), /*retain=*/true);
    }

    void _doPublishUpdate() {
        auto& us = Updater::status();
        JsonDocument doc;
        doc["installed_version"] = us.currentVersion;
        doc["latest_version"]    = us.hasUpdate ? us.latestVersion : us.currentVersion;
        bool inProgress = (us.state == Updater::State::Downloading);
        doc["in_progress"] = inProgress;
        if (inProgress) doc["update_percentage"] = us.progress;
        else            doc["update_percentage"] = nullptr;
        String s; serializeJson(doc, s);
        _client.publish(_updateStateTopic, s.c_str(), /*retain=*/true);
    }

    void _doPublishSceneSync() {
        _client.publish(_sceneSyncStateTopic, Config::get().sceneSyncEnabled ? "ON" : "OFF", /*retain=*/true);
    }

    // ── Command handling ─────────────────────────────────────────────────────

    // Matches topic against "<prefix><id>/set" exactly; fills outId on match.
    static bool _parseSuffixId(const char* topic, const char* prefix, uint8_t maxId, uint8_t& outId) {
        size_t plen = strlen(prefix);
        if (strncmp(topic, prefix, plen) != 0) return false;
        const char* p = topic + plen;
        char* end = nullptr;
        long id = strtol(p, &end, 10);
        if (end == p || id < 0 || id >= maxId) return false;
        if (strcmp(end, "/set") != 0) return false;
        outId = (uint8_t)id;
        return true;
    }

    void _handleMsg(char* topic, uint8_t* payload, unsigned int len) {
        uint8_t id;
        if (_parseSuffixId(topic, _groupPrefix, MAX_GROUPS, id)) { _handleGroupSet(id, payload, len); return; }
        if (_parseSuffixId(topic, _lightPrefix, MAX_LIGHTS, id)) { _handleLightSet(id, payload, len); return; }
        if (strcmp(topic, _updateSetTopic) == 0)                 { _handleUpdateSet(payload, len);    return; }
        if (strcmp(topic, _sceneSyncSetTopic) == 0)              { _handleSceneSyncSet(payload, len); return; }
    }

    // Maps an effect_list display string back to mode/pattern/sceneId.
    // Returns true if it matched something recognizable.
    bool _applyEffectName(const char* effect, LightConfig& cfg) {
        for (uint8_t i = 0; i < 5; i++) {
            if (strcmp(effect, kPatternNames[i]) == 0) {
                cfg.mode    = GroupMode::Pattern;
                cfg.pattern = (PatternId)i;
                return true;
            }
        }
        if (strcmp(effect, "Proximity") == 0) { cfg.mode = GroupMode::Proximity; return true; }
        if (strcmp(effect, "Text")      == 0) { cfg.mode = GroupMode::Text;      return true; }
        if (strcmp(effect, "Time")      == 0) { cfg.mode = GroupMode::Time;      return true; }

        const char* rest = nullptr;
        GroupMode   targetMode;
        if      (strncmp(effect, "Scene: ", 7)    == 0) { rest = effect + 7;  targetMode = GroupMode::Scene; }
        else if (strncmp(effect, "Gradient: ", 10) == 0) { rest = effect + 10; targetMode = GroupMode::Gradient; }
        if (!rest) return false;

        JsonDocument sc;
        SceneManager::buildList(sc);
        for (JsonObject o : sc["scenes"].as<JsonArray>()) {
            if (strcmp(o["name"] | "", rest) == 0) {
                cfg.mode = targetMode;
                strlcpy(cfg.sceneId, o["id"] | "", sizeof(cfg.sceneId));
                return true;
            }
        }
        return false;
    }

    void _handleGroupSet(uint8_t id, uint8_t* payload, unsigned int len) {
        GroupConfig* g = Config::group(id);
        if (!g) { Logger::w("[mqtt] group %u set — not found", id); return; }
        JsonDocument doc;
        if (deserializeJson(doc, payload, len)) { Logger::w("[mqtt] group %u: bad JSON", id); return; }

        if (!doc["syncEnabled"].isNull()) {
            g->syncEnabled = (bool)doc["syncEnabled"];
            Config::bumpGroupRevision(*g);
            Config::save();
            if (_onGroupSyncToggle) _onGroupSyncToggle(*g);
            return;
        }

        // HA's json light schema nests color under "color": normalize to the
        // flat r/g/b keys deserializeLightConfig expects (see Config.cpp).
        if (!doc["color"].isNull()) {
            doc["r"] = doc["color"]["r"] | (int)g->light.color.r;
            doc["g"] = doc["color"]["g"] | (int)g->light.color.g;
            doc["b"] = doc["color"]["b"] | (int)g->light.color.b;
        }

        LightConfig cfg = deserializeLightConfig(doc, g->light);

        const char* state = doc["state"] | "";
        if (strcmp(state, "OFF") == 0) {
            cfg.brightness = 0;
        } else if (strcmp(state, "ON") == 0 && doc["brightness"].isNull() && cfg.brightness == 0) {
            // HA's on/off toggle sends a bare {"state":"ON"} with no brightness —
            // deserializeLightConfig above defaulted the missing field to the
            // group's *current* (0, since it's off) brightness, which would
            // otherwise leave the light dark. Restore whatever it was last on at.
            cfg.brightness = _lastOnBrightness[id] > 0 ? _lastOnBrightness[id] : 255;
        }

        if (!doc["effect"].isNull()) _applyEffectName(doc["effect"], cfg);

        cfg.seq = g->light.seq + 1;
        Logger::d("[mqtt] group %u command applied", id);
        if (_onGroupLight) _onGroupLight(id, cfg);
    }

    void _handleLightSet(uint8_t idx, uint8_t* payload, unsigned int len) {
        if (idx >= MAX_LIGHTS || !Config::get().lights[idx].exists) {
            Logger::w("[mqtt] light %u set — not found", idx); return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, payload, len)) { Logger::w("[mqtt] light %u: bad JSON", idx); return; }

        auto& l = Config::get().lights[idx];
        bool changed = false;
        if (!doc["brightnessOverrideEnabled"].isNull()) {
            l.brightnessOverrideEnabled = (bool)doc["brightnessOverrideEnabled"];
            changed = true;
        }
        if (!doc["brightnessOverride"].isNull()) {
            l.brightnessOverride = (uint8_t)constrain((int)doc["brightnessOverride"], 0, 255);
            changed = true;
        }
        if (!doc["groupId"].isNull()) {
            uint8_t gid = doc["groupId"];
            if (Config::group(gid)) { l.groupId = gid; changed = true; }
            else Logger::w("[mqtt] light %u: groupId %u not found", idx, gid);
        }
        if (!changed) return;
        Config::save();
        if (_onLightOverride) _onLightOverride(idx);
    }

    void _handleUpdateSet(uint8_t* payload, unsigned int len) {
        String cmd; cmd.reserve(len);
        for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
        if (cmd == "INSTALL") {
            Logger::i("[mqtt] update install requested");
            Updater::triggerAsync();
        }
    }

    // Mirrors WebServer's _postConfig/_setRemoteSceneSync own-mac branch.
    void _handleSceneSyncSet(uint8_t* payload, unsigned int len) {
        String cmd; cmd.reserve(len);
        for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
        if (cmd != "ON" && cmd != "OFF") { Logger::w("[mqtt] sceneSync: bad payload"); return; }

        bool prev    = Config::get().sceneSyncEnabled;
        bool enabled = (cmd == "ON");
        Config::get().sceneSyncEnabled = enabled;
        Config::save();
        if (enabled && !prev && _onSceneSyncEnabled) _onSceneSyncEnabled();
        publishSceneSyncState();
    }
};
