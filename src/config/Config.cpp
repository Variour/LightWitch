#include "Config.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "../logging/Logger.h"

DeviceConfig Config::_cfg;
const char* Config::_path = "/config.json";
WifiNetwork Config::_wifiNetworks[MAX_WIFI_NETWORKS];
uint8_t Config::_wifiCount = 0;
uint8_t Config::_wifiLast = 0;

static const char* _wifiPath = "/wifi.json";

static constexpr char NVS_NS[] = "bl";
static constexpr char NVS_KEY[] = "cfg";
static constexpr char NVS_WIFI_KEY[] = "wifi";

void serializeLightConfig(JsonObject o, const LightConfig& l) {
    o["mode"] = (uint8_t)l.mode;
    o["sceneId"] = l.sceneId;
    o["pattern"] = (uint8_t)l.pattern;
    o["r"] = l.color.r;
    o["g"] = l.color.g;
    o["b"] = l.color.b;
    o["brightness"] = l.brightness;
    o["speed"] = l.speed;
    o["seq"] = l.seq;
    o["proximityScale"] = l.proximityScale;
    o["morphEnabled"] = l.morphEnabled;
    o["gradientStopCount"] = l.gradientStopCount;
    o["text"] = l.text;
    o["textAnimation"] = (uint8_t)l.textAnimation;
    o["time24h"] = l.time24h;
}

LightConfig deserializeLightConfig(JsonVariantConst j, const LightConfig& def) {
    LightConfig l = def;
    l.mode = (GroupMode)(uint8_t)(j["mode"] | (uint8_t)def.mode);
    strlcpy(l.sceneId, j["sceneId"] | def.sceneId, sizeof(l.sceneId));
    l.pattern = (PatternId)(uint8_t)(j["pattern"] | (uint8_t)def.pattern);
    l.color.r = j["r"] | def.color.r;
    l.color.g = j["g"] | def.color.g;
    l.color.b = j["b"] | def.color.b;
    l.brightness = j["brightness"] | def.brightness;
    l.speed = j["speed"] | def.speed;
    l.seq = j["seq"] | def.seq;
    l.proximityScale = j["proximityScale"] | def.proximityScale;
    l.morphEnabled = j["morphEnabled"] | def.morphEnabled;
    l.gradientStopCount = j["gradientStopCount"] | def.gradientStopCount;
    strlcpy(l.text, j["text"] | def.text, sizeof(l.text));
    l.textAnimation = (TextAnimation)(uint8_t)(j["textAnimation"] | (uint8_t)def.textAnimation);
    l.time24h = j["time24h"] | def.time24h;
    return l;
}

void serializeGroup(JsonObject o, const GroupConfig& g) {
    o["id"] = g.id;
    o["name"] = g.name;
    o["exists"] = g.exists;
    o["syncEnabled"] = g.syncEnabled;
    serializeLightConfig(o, g.light);
}

void deserializeGroup(JsonVariantConst o, GroupConfig& g) {
    g.id = o["id"] | (uint8_t)0;
    g.exists = o["exists"] | false;
    g.syncEnabled = o["syncEnabled"] | true;
    strlcpy(g.name, o["name"] | "Default", sizeof(g.name));
    g.light = deserializeLightConfig(o, LightConfig{});
}

void serializeButtonAction(JsonObject o, const ButtonAction& a) {
    o["action"] = (uint8_t)a.action;
    o["groupId"] = a.groupId;
    o["lightIndex"] = a.lightIndex;
    o["numberValue"] = a.params.numberValue;
    o["stringValue"] = a.params.stringValue;
    o["r"] = a.params.colorValue.r;
    o["g"] = a.params.colorValue.g;
    o["b"] = a.params.colorValue.b;
}

ButtonAction deserializeButtonAction(JsonVariantConst j, const ButtonAction& def) {
    ButtonAction a = def;
    a.action = (ActionId)(uint8_t)(j["action"] | (uint8_t)def.action);
    a.groupId = j["groupId"] | def.groupId;
    a.lightIndex = j["lightIndex"] | def.lightIndex;
    a.params.numberValue = j["numberValue"] | def.params.numberValue;
    strlcpy(a.params.stringValue, j["stringValue"] | def.params.stringValue,
            sizeof(a.params.stringValue));
    a.params.colorValue.r = j["r"] | def.params.colorValue.r;
    a.params.colorValue.g = j["g"] | def.params.colorValue.g;
    a.params.colorValue.b = j["b"] | def.params.colorValue.b;
    return a;
}

void serializeButton(JsonObject o, const ButtonHardwareConfig& b) {
    o["name"] = b.name;
    o["pin"] = b.pin;
    o["activeLow"] = b.activeLow;
    o["viaExpander"] = b.viaExpander;
    o["exists"] = b.exists;
    serializeButtonAction(o["onShortPress"].to<JsonObject>(), b.onShortPress);
    serializeButtonAction(o["onLongPress"].to<JsonObject>(), b.onLongPress);
    serializeButtonAction(o["onDoubleClick"].to<JsonObject>(), b.onDoubleClick);
}

void deserializeButton(JsonVariantConst o, ButtonHardwareConfig& b) {
    strlcpy(b.name, o["name"] | "", sizeof(b.name));
    b.pin = o["pin"] | (uint8_t)0;
    b.activeLow = o["activeLow"] | true;
    b.viaExpander = o["viaExpander"] | false;
    b.exists = o["exists"] | false;
    b.onShortPress = deserializeButtonAction(o["onShortPress"], b.onShortPress);
    b.onLongPress = deserializeButtonAction(o["onLongPress"], b.onLongPress);
    b.onDoubleClick = deserializeButtonAction(o["onDoubleClick"], b.onDoubleClick);
}

void serializeSound(JsonObject o, const SoundHardwareConfig& s) {
    o["name"] = s.name;
    o["chip"] = (uint8_t)s.chip;
    o["i2cAddress"] = s.i2cAddress;
    o["i2sMclkPin"] = s.i2sMclkPin;
    o["i2sBclkPin"] = s.i2sBclkPin;
    o["i2sWsPin"] = s.i2sWsPin;
    o["i2sDoutPin"] = s.i2sDoutPin;
    o["paEnablePin"] = s.paEnablePin;
    o["paEnableActiveHigh"] = s.paEnableActiveHigh;
    o["paViaExpander"] = s.paViaExpander;
    o["exists"] = s.exists;
}

void deserializeSound(JsonVariantConst o, SoundHardwareConfig& s) {
    strlcpy(s.name, o["name"] | "", sizeof(s.name));
    s.chip = (SoundChip)(uint8_t)(o["chip"] | (uint8_t)SoundChip::ES8311);
    s.i2cAddress = o["i2cAddress"] | (uint8_t)0x18;
    s.i2sMclkPin = o["i2sMclkPin"] | PIN_UNUSED;
    s.i2sBclkPin = o["i2sBclkPin"] | PIN_UNUSED;
    s.i2sWsPin = o["i2sWsPin"] | PIN_UNUSED;
    s.i2sDoutPin = o["i2sDoutPin"] | PIN_UNUSED;
    s.paEnablePin = o["paEnablePin"] | PIN_UNUSED;
    s.paEnableActiveHigh = o["paEnableActiveHigh"] | true;
    s.paViaExpander = o["paViaExpander"] | false;
    s.exists = o["exists"] | false;
}

static bool migrateDoc(JsonDocument& doc) {
    uint8_t ver = doc["schemaVersion"] | (uint8_t)0;
    if (ver > CONFIG_SCHEMA_VERSION) {
        Logger::w("[cfg] schema version %u > firmware max %u — ignoring", ver,
                  CONFIG_SCHEMA_VERSION);
        return false;
    }
    if (ver < CONFIG_SCHEMA_VERSION) {
        Logger::i("[cfg] schema v%u < v%u — resetting to defaults", ver, CONFIG_SCHEMA_VERSION);
        return false;
    }
    return true;
}

static void applyDoc(JsonDocument& doc) {
    strlcpy(Config::get().deviceName, doc["deviceName"] | "batterylight",
            sizeof(Config::get().deviceName));
    strlcpy(Config::get().apPassword, doc["apPassword"] | "batterylight",
            sizeof(Config::get().apPassword));
    Config::get().otaPort = doc["otaPort"] | 3232;
    Config::get().otaEnabled = doc["otaEnabled"] | true;
    Config::get().sceneSyncEnabled = doc["sceneSyncEnabled"] | true;
    Config::get().checkUpdateOnStartup = doc["checkUpdateOnStartup"] | false;
    Config::get().wifiSingleClientMode = doc["wifiSingleClientMode"] | false;
    Config::get().batteryMonitoringEnabled = doc["batteryMonitoringEnabled"] | false;
    Config::get().prOtaEnabled = doc["prOtaEnabled"] | false;
    strlcpy(Config::get().prTrack, doc["prTrack"] | "", sizeof(Config::get().prTrack));
    Config::get().prTrackAssetId = doc["prTrackAssetId"] | (uint32_t)0;
    Config::get().i2cSdaPin = doc["i2cSdaPin"] | PIN_UNUSED;
    Config::get().i2cSclPin = doc["i2cSclPin"] | PIN_UNUSED;
    Config::get().expanderChip =
        (IoExpanderChip)(uint8_t)(doc["expanderChip"] | (uint8_t)IoExpanderChip::None);
    Config::get().expanderAddress = doc["expanderAddress"] | (uint8_t)0x20;
    Config::get().wifiPolicyRevision = doc["wifiPolicyRevision"] | (uint32_t)0;
    for (uint8_t i = 0; i < 6; i++)
        Config::get().wifiPolicyOriginMac[i] = doc["wifiPolicyOriginMac"][i] | (uint8_t)0;
    Config::get().logLevel = doc["logLevel"] | (uint8_t)0;
    strlcpy(Config::get().mqttHost, doc["mqttHost"] | "", sizeof(Config::get().mqttHost));
    Config::get().mqttPort = doc["mqttPort"] | (uint16_t)1883;
    strlcpy(Config::get().mqttUser, doc["mqttUser"] | "", sizeof(Config::get().mqttUser));
    strlcpy(Config::get().mqttPassword, doc["mqttPassword"] | "",
            sizeof(Config::get().mqttPassword));
    strlcpy(Config::get().githubToken, doc["githubToken"] | "", sizeof(Config::get().githubToken));
    strlcpy(Config::get().githubRepo, doc["githubRepo"] | "variour/batterylight",
            sizeof(Config::get().githubRepo));
    strlcpy(Config::get().timezone, doc["timezone"] | "UTC0", sizeof(Config::get().timezone));

    if (doc["lights"].is<JsonArray>()) {
        for (JsonVariant v : doc["lights"].as<JsonArray>()) {
            uint8_t idx = v["index"] | (uint8_t)0;
            if (idx >= MAX_LIGHTS) continue;
            auto& l = Config::get().lights[idx];
            l.exists = v["exists"] | false;
            l.ledType = (LedType)(uint8_t)(v["ledType"] | 0);
            l.colorOrder =
                (ColorOrder)(uint8_t)(v["colorOrder"] | (uint8_t)defaultColorOrder(l.ledType));
            l.dataPin = v["dataPin"] | (uint8_t)LED_DATA_PIN;
            l.clockPin = v["clockPin"] | (uint8_t)LED_CLOCK_PIN;
            l.width = v["width"] | (uint16_t)1;
            l.height = v["height"] | (uint16_t)1;
            l.matrixStart =
                (MatrixStart)(uint8_t)(v["matrixStart"] | (uint8_t)MatrixStart::TopLeft);
            l.matrixDir =
                (MatrixDirection)(uint8_t)(v["matrixDir"] | (uint8_t)MatrixDirection::Horizontal);
            l.matrixSerpentine = v["matrixSerpentine"] | false;
            l.wrapWidth = v["wrapWidth"] | false;
            l.wrapHeight = v["wrapHeight"] | false;
            l.groupId = v["groupId"] | (uint8_t)0;
            l.brightnessOverrideEnabled = v["brightnessOverrideEnabled"] | false;
            l.brightnessOverride = v["brightnessOverride"] | (uint8_t)255;
            if (!v["name"].isNull()) strlcpy(l.name, v["name"] | "", sizeof(l.name));
        }
    }

    if (doc["groups"].is<JsonArray>()) {
        for (JsonVariant v : doc["groups"].as<JsonArray>()) {
            uint8_t id = v["id"] | (uint8_t)0;
            if (id < MAX_GROUPS) deserializeGroup(v, Config::get().groups[id]);
        }
    }

    if (doc["groupRevisions"].is<JsonArray>()) {
        for (JsonVariant v : doc["groupRevisions"].as<JsonArray>()) {
            uint8_t id = v["id"] | (uint8_t)0;
            if (id >= MAX_GROUPS) continue;
            auto& g = Config::get().groups[id];
            g.revision = v["revision"] | (uint32_t)0;
            for (uint8_t b = 0; b < 6; b++) g.originMac[b] = v["originMac"][b] | (uint8_t)0;
        }
    }

    if (doc["buttons"].is<JsonArray>()) {
        for (JsonVariant v : doc["buttons"].as<JsonArray>()) {
            uint8_t idx = v["index"] | (uint8_t)0;
            if (idx < MAX_BUTTONS) deserializeButton(v, Config::get().buttons[idx]);
        }
    }

    if (doc["sounds"].is<JsonArray>()) {
        for (JsonVariant v : doc["sounds"].as<JsonArray>()) {
            uint8_t idx = v["index"] | (uint8_t)0;
            if (idx < MAX_SOUNDS) deserializeSound(v, Config::get().sounds[idx]);
        }
    }
}

bool Config::load() {
    if (LittleFS.exists(_path)) {
        File f = LittleFS.open(_path, "r");
        if (f) {
            JsonDocument doc;
            bool ok = !deserializeJson(doc, f);
            f.close();
            if (ok && migrateDoc(doc)) {
                applyDoc(doc);
                _ensureDefaultGroup();
                Logger::d("[cfg] loaded from LittleFS");
                return true;
            }
        }
    }

    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/true)) {
        String json = prefs.getString(NVS_KEY, "");
        prefs.end();
        if (json.length() > 0) {
            JsonDocument doc;
            if (!deserializeJson(doc, json) && migrateDoc(doc)) {
                applyDoc(doc);
                _ensureDefaultGroup();
                Logger::i("[cfg] no LittleFS config — restored from NVS");
                save();
                return true;
            }
        }
    }

    Logger::w("[cfg] no saved config — using defaults");
    _ensureDefaultGroup();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(_cfg.deviceName, sizeof(_cfg.deviceName), "light-%02x%02x%02x", mac[3], mac[4],
             mac[5]);
    return false;
}

bool Config::save() {
    JsonDocument doc;
    doc["schemaVersion"] = CONFIG_SCHEMA_VERSION;
    doc["deviceName"] = _cfg.deviceName;
    doc["apPassword"] = _cfg.apPassword;
    doc["otaPort"] = _cfg.otaPort;
    doc["otaEnabled"] = _cfg.otaEnabled;
    doc["sceneSyncEnabled"] = _cfg.sceneSyncEnabled;
    doc["checkUpdateOnStartup"] = _cfg.checkUpdateOnStartup;
    doc["wifiSingleClientMode"] = _cfg.wifiSingleClientMode;
    doc["batteryMonitoringEnabled"] = _cfg.batteryMonitoringEnabled;
    doc["prOtaEnabled"] = _cfg.prOtaEnabled;
    doc["prTrack"] = _cfg.prTrack;
    doc["prTrackAssetId"] = _cfg.prTrackAssetId;
    doc["i2cSdaPin"] = _cfg.i2cSdaPin;
    doc["i2cSclPin"] = _cfg.i2cSclPin;
    doc["expanderChip"] = (uint8_t)_cfg.expanderChip;
    doc["expanderAddress"] = _cfg.expanderAddress;
    doc["wifiPolicyRevision"] = _cfg.wifiPolicyRevision;
    {
        JsonArray origin = doc["wifiPolicyOriginMac"].to<JsonArray>();
        for (uint8_t i = 0; i < 6; i++) origin.add(_cfg.wifiPolicyOriginMac[i]);
    }
    doc["logLevel"] = _cfg.logLevel;
    doc["mqttHost"] = _cfg.mqttHost;
    doc["mqttPort"] = _cfg.mqttPort;
    doc["mqttUser"] = _cfg.mqttUser;
    doc["mqttPassword"] = _cfg.mqttPassword;
    doc["githubToken"] = _cfg.githubToken;
    doc["githubRepo"] = _cfg.githubRepo;
    doc["timezone"] = _cfg.timezone;

    JsonArray lightsArr = doc["lights"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
        if (!_cfg.lights[i].exists) continue;
        auto& l = _cfg.lights[i];
        JsonObject o = lightsArr.add<JsonObject>();
        o["index"] = i;
        o["exists"] = l.exists;
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

    JsonArray arr = doc["groups"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_GROUPS; i++)
        if (_cfg.groups[i].exists) serializeGroup(arr.add<JsonObject>(), _cfg.groups[i]);

    // Mesh-internal only (not part of the API-facing group shape, unlike the
    // "groups" array above) — every slot is saved regardless of exists, so a
    // group's revision counter survives both a delete and a reboot, staying
    // monotonic if the slot is later reused. Mirrors wifiPolicyRevision/
    // wifiPolicyOriginMac above, just indexed per group.
    JsonArray revArr = doc["groupRevisions"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_GROUPS; i++) {
        JsonObject o = revArr.add<JsonObject>();
        o["id"] = i;
        o["revision"] = _cfg.groups[i].revision;
        JsonArray origin = o["originMac"].to<JsonArray>();
        for (uint8_t b = 0; b < 6; b++) origin.add(_cfg.groups[i].originMac[b]);
    }

    JsonArray buttonsArr = doc["buttons"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
        if (!_cfg.buttons[i].exists) continue;
        JsonObject o = buttonsArr.add<JsonObject>();
        o["index"] = i;
        serializeButton(o, _cfg.buttons[i]);
    }

    JsonArray soundsArr = doc["sounds"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
        if (!_cfg.sounds[i].exists) continue;
        JsonObject o = soundsArr.add<JsonObject>();
        o["index"] = i;
        serializeSound(o, _cfg.sounds[i]);
    }

    bool fsOk = false;
    File f = LittleFS.open(_path, "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
        fsOk = true;
    }
    if (!fsOk) Logger::e("[cfg] LittleFS write failed");

    String json;
    serializeJson(doc, json);
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
        prefs.putString(NVS_KEY, json);
        prefs.end();
    }

    Logger::d("[cfg] saved (%u bytes)", json.length());
    return fsOk;
}

void Config::reset() {
    LittleFS.remove(_path);
    Preferences prefs;
    if (prefs.begin(NVS_NS, false)) {
        prefs.clear();
        prefs.end();
    }
}

uint8_t Config::createGroup(const char* name) {
    for (uint8_t i = 1; i < MAX_GROUPS; i++) {
        if (!_cfg.groups[i].exists) {
            // Preserve the slot's revision counter across delete/recreate cycles —
            // it must stay monotonic so a peer that cached the previous occupant's
            // (higher) revision can't reject this new group forever. bumpGroupRevision
            // below re-bumps past whatever we just preserved and stamps our MAC, so
            // the returned group is fully ready to broadcast without any further
            // caller action.
            uint32_t prevRevision = _cfg.groups[i].revision;
            _cfg.groups[i] = GroupConfig{};
            _cfg.groups[i].id = i;
            _cfg.groups[i].exists = true;
            _cfg.groups[i].revision = prevRevision;
            strlcpy(_cfg.groups[i].name, name, sizeof(_cfg.groups[i].name));
            bumpGroupRevision(_cfg.groups[i]);
            return i;
        }
    }
    return 0xFF;
}

void Config::bumpGroupRevision(GroupConfig& g) {
    g.revision++;
    WiFi.macAddress(g.originMac);
}

int Config::compareGroupRevision(const GroupConfig& a, const GroupConfig& b) {
    if (a.revision != b.revision) return a.revision > b.revision ? 1 : -1;
    int macCmp = memcmp(a.originMac, b.originMac, sizeof(a.originMac));
    return macCmp > 0 ? 1 : (macCmp < 0 ? -1 : 0);
}

bool Config::applyGroupSync(const GroupConfig& g) {
    if (g.id >= MAX_GROUPS) return false;
    GroupConfig& local = _cfg.groups[g.id];

    // A single revision+originMac governs the whole group now — name, exists,
    // syncEnabled, and light all move and reconcile together as one unit, so
    // a rename and a light edit made on the same device always converge as
    // one coherent update, and a freshly-created group (see createGroup,
    // which preserves+bumps revision across delete/recreate on a reused
    // slot) can never be out-ranked by a stale peer's cached data for
    // whatever used to occupy that slot — light included.
    if (compareGroupRevision(g, local) <= 0) return false;
    local = g;
    return true;
}

void Config::applyConfigSync(const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) {
        Logger::e("[cfg] applyConfigSync: bad json");
        return;
    }
    auto& c = _cfg;
    if (!doc["mqttHost"].isNull()) strlcpy(c.mqttHost, doc["mqttHost"], sizeof(c.mqttHost));
    if (!doc["mqttPort"].isNull()) c.mqttPort = (uint16_t)doc["mqttPort"];
    if (!doc["mqttUser"].isNull()) strlcpy(c.mqttUser, doc["mqttUser"], sizeof(c.mqttUser));
    if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0)
        strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
    if (!doc["githubRepo"].isNull()) strlcpy(c.githubRepo, doc["githubRepo"], sizeof(c.githubRepo));
    if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
        strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));
    if (!doc["otaEnabled"].isNull()) c.otaEnabled = (bool)doc["otaEnabled"];
    if (!doc["checkUpdateOnStartup"].isNull())
        c.checkUpdateOnStartup = (bool)doc["checkUpdateOnStartup"];
    if (!doc["deviceName"].isNull() && strlen(doc["deviceName"]) > 0)
        strlcpy(c.deviceName, doc["deviceName"], sizeof(c.deviceName));
    if (!doc["apPassword"].isNull() && strlen(doc["apPassword"]) >= 8)
        strlcpy(c.apPassword, doc["apPassword"], sizeof(c.apPassword));

    if (doc["addWifiNetworks"].is<JsonArray>()) {
        loadWifi();
        JsonArray nets = doc["addWifiNetworks"].as<JsonArray>();
        uint8_t newCount = _wifiCount;
        for (JsonVariant v : nets) {
            const char* ssid = v["ssid"] | "";
            if (strlen(ssid) == 0) continue;
            bool exists = false;
            for (uint8_t i = 0; i < _wifiCount; i++)
                if (strcmp(_wifiNetworks[i].ssid, ssid) == 0) {
                    exists = true;
                    break;
                }
            if (!exists) newCount++;
        }
        if (newCount > MAX_WIFI_NETWORKS) {
            Logger::w("[cfg] addWifiNetworks would exceed %u, skipping wifi merge",
                      MAX_WIFI_NETWORKS);
        } else {
            for (JsonVariant v : nets) {
                const char* ssid = v["ssid"] | "";
                const char* pass = v["password"] | "";
                if (strlen(ssid) == 0) continue;
                addWifiNetwork(ssid, pass);
            }
        }
    }

    save();
    Logger::i("[cfg] config sync applied, restarting");
    delay(200);
    ESP.restart();
}

bool Config::loadWifi() {
    _wifiCount = 0;
    _wifiLast = 0;

    auto parseDoc = [&](JsonDocument& doc) {
        _wifiLast = doc["last"] | (uint8_t)0;
        if (!doc["networks"].is<JsonArray>()) return;
        for (JsonVariant v : doc["networks"].as<JsonArray>()) {
            if (_wifiCount >= MAX_WIFI_NETWORKS) break;
            strlcpy(_wifiNetworks[_wifiCount].ssid, v["ssid"] | "", sizeof(_wifiNetworks[0].ssid));
            strlcpy(_wifiNetworks[_wifiCount].password, v["password"] | "",
                    sizeof(_wifiNetworks[0].password));
            _wifiCount++;
        }
    };

    if (LittleFS.exists(_wifiPath)) {
        File f = LittleFS.open(_wifiPath, "r");
        if (f) {
            JsonDocument doc;
            bool ok = !deserializeJson(doc, f);
            f.close();
            if (ok) {
                parseDoc(doc);
                return true;
            }
        }
    }

    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/true)) {
        String json = prefs.getString(NVS_WIFI_KEY, "");
        prefs.end();
        if (json.length() > 0) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
                parseDoc(doc);
                Logger::i("[cfg] no wifi.json — restored from NVS");
                saveWifi();
                return true;
            }
        }
    }

    return false;
}

bool Config::saveWifi() {
    JsonDocument doc;
    doc["last"] = _wifiLast;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (uint8_t i = 0; i < _wifiCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = _wifiNetworks[i].ssid;
        o["password"] = _wifiNetworks[i].password;
    }
    File f = LittleFS.open(_wifiPath, "w");
    if (!f) {
        Logger::e("[cfg] wifi.json write failed");
        return false;
    }
    serializeJson(doc, f);
    f.close();

    String json;
    serializeJson(doc, json);
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
        prefs.putString(NVS_WIFI_KEY, json);
        prefs.end();
    }

    return true;
}

bool Config::addWifiNetwork(const char* ssid, const char* password) {
    for (uint8_t i = 0; i < _wifiCount; i++) {
        if (strcmp(_wifiNetworks[i].ssid, ssid) == 0) {
            strlcpy(_wifiNetworks[i].password, password, sizeof(_wifiNetworks[0].password));
            return saveWifi();
        }
    }
    if (_wifiCount >= MAX_WIFI_NETWORKS) return false;
    strlcpy(_wifiNetworks[_wifiCount].ssid, ssid, sizeof(_wifiNetworks[0].ssid));
    strlcpy(_wifiNetworks[_wifiCount].password, password, sizeof(_wifiNetworks[0].password));
    _wifiCount++;
    return saveWifi();
}

bool Config::deleteWifiNetwork(const char* ssid) {
    for (uint8_t i = 0; i < _wifiCount; i++) {
        if (strcmp(_wifiNetworks[i].ssid, ssid) == 0) {
            for (uint8_t j = i; j < _wifiCount - 1; j++) _wifiNetworks[j] = _wifiNetworks[j + 1];
            _wifiCount--;
            if (_wifiLast == i)
                _wifiLast = 0;
            else if (_wifiLast > i)
                _wifiLast--;
            saveWifi();
            return true;
        }
    }
    return false;
}

// Swaps ssid with its immediate neighbor in the given direction (-1 = up /
// earlier, +1 = down / later). Connect order is list order (see setupWifi()
// and WifiConnectAttempt), so this is how priority is changed.
bool Config::moveWifiNetwork(const char* ssid, int8_t direction) {
    int8_t i = -1;
    for (uint8_t k = 0; k < _wifiCount; k++) {
        if (strcmp(_wifiNetworks[k].ssid, ssid) == 0) {
            i = (int8_t)k;
            break;
        }
    }
    if (i < 0) return false;

    int8_t j = i + (direction < 0 ? -1 : 1);
    if (j < 0 || j >= (int8_t)_wifiCount) return false;

    WifiNetwork tmp = _wifiNetworks[i];
    _wifiNetworks[i] = _wifiNetworks[j];
    _wifiNetworks[j] = tmp;

    if (_wifiLast == (uint8_t)i)
        _wifiLast = (uint8_t)j;
    else if (_wifiLast == (uint8_t)j)
        _wifiLast = (uint8_t)i;

    return saveWifi();
}

bool Config::isPinInUse(uint8_t pin, int8_t excludeButtonIndex, int8_t excludeSoundIndex,
                        int8_t excludeLightIndex, bool excludeI2cBus) {
    if (!excludeI2cBus) {
        if (_cfg.i2cSdaPin != PIN_UNUSED && _cfg.i2cSdaPin == pin) return true;
        if (_cfg.i2cSclPin != PIN_UNUSED && _cfg.i2cSclPin == pin) return true;
    }
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
        if ((int8_t)i == excludeLightIndex) continue;
        auto& l = _cfg.lights[i];
        if (!l.exists) continue;
        if (l.dataPin == pin) return true;
        if (l.ledType == LedType::WS2801 && l.clockPin == pin) return true;
    }
    for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
        if ((int8_t)i == excludeButtonIndex) continue;
        auto& b = _cfg.buttons[i];
        // A button's pin only occupies the ESP32 GPIO address space when it's
        // a direct pin — on the device expander it's a pin index in a
        // separate space (see IoExpanderChip) and must not be cross-checked
        // against real GPIOs.
        if (b.exists && !b.viaExpander && b.pin == pin) return true;
    }
    if (pin != PIN_UNUSED) {
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            if ((int8_t)i == excludeSoundIndex) continue;
            auto& s = _cfg.sounds[i];
            if (!s.exists) continue;
            if (s.i2sMclkPin == pin || s.i2sBclkPin == pin || s.i2sWsPin == pin ||
                s.i2sDoutPin == pin)
                return true;
            // paEnablePin only occupies the ESP32 GPIO address space when it's a
            // direct pin — on the device expander it's a pin index in a
            // separate space (see IoExpanderChip) and must not be
            // cross-checked against real GPIOs.
            if (!s.paViaExpander && s.paEnablePin == pin) return true;
        }
    }
    return false;
}

bool Config::isExpanderPinInUse(uint8_t pin, int8_t excludeButtonIndex, bool excludeSoundPa) {
    for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
        if ((int8_t)i == excludeButtonIndex) continue;
        auto& b = _cfg.buttons[i];
        if (b.exists && b.viaExpander && b.pin == pin) return true;
    }
    if (!excludeSoundPa) {
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            auto& s = _cfg.sounds[i];
            if (s.exists && s.paViaExpander && s.paEnablePin == pin) return true;
        }
    }
    return false;
}

bool Config::i2cBusInUse() {
    if (_cfg.expanderChip != IoExpanderChip::None) return true;
    for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
        if (_cfg.sounds[i].exists) return true;
    }
    return false;
}

bool Config::expanderInUse() {
    for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
        auto& s = _cfg.sounds[i];
        if (s.exists && s.paViaExpander) return true;
    }
    for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
        auto& b = _cfg.buttons[i];
        if (b.exists && b.viaExpander) return true;
    }
    return false;
}

void Config::_ensureDefaultGroup() {
    if (!_cfg.groups[0].exists) {
        _cfg.groups[0].id = 0;
        _cfg.groups[0].exists = true;
        strlcpy(_cfg.groups[0].name, "Default", sizeof(_cfg.groups[0].name));
    }
}
