#include "Config.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "../logging/Logger.h"

DeviceConfig Config::_cfg;
const char*  Config::_path = "/config.json";

static constexpr char NVS_NS[]  = "bl";
static constexpr char NVS_KEY[] = "cfg";

static void serializeGroup(JsonArray arr, const GroupConfig& g) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]                = g.id;
    o["name"]              = g.name;
    o["exists"]            = g.exists;
    o["syncEnabled"]       = g.syncEnabled;
    o["mode"]              = (uint8_t)g.light.mode;
    o["sceneId"]           = g.light.sceneId;
    o["pattern"]           = (uint8_t)g.light.pattern;
    o["r"]                 = g.light.color.r;
    o["g"]                 = g.light.color.g;
    o["b"]                 = g.light.color.b;
    o["brightness"]        = g.light.brightness;
    o["speed"]             = g.light.speed;
    o["seq"]               = g.light.seq;
    o["transitionEnabled"] = g.light.transitionEnabled;
    o["transitionTime"]    = g.light.transitionTime;
    o["proximityScale"]    = g.light.proximityScale;
}

static void deserializeGroup(JsonVariant o, GroupConfig& g) {
    g.id          = o["id"]          | (uint8_t)0;
    g.exists      = o["exists"]      | false;
    g.syncEnabled = o["syncEnabled"] | true;
    strlcpy(g.name, o["name"] | "Default", sizeof(g.name));
    g.light.mode       = (GroupMode)(uint8_t)(o["mode"] | (uint8_t)GroupMode::Pattern);
    strlcpy(g.light.sceneId, o["sceneId"] | "", sizeof(g.light.sceneId));
    g.light.pattern    = (PatternId)(uint8_t)(o["pattern"] | 0);
    g.light.color.r    = o["r"]          | 255;
    g.light.color.g    = o["g"]          | 255;
    g.light.color.b    = o["b"]          | 255;
    g.light.brightness = o["brightness"] | 255;
    g.light.speed              = o["speed"]             | 1.0f;
    g.light.seq                = o["seq"]               | (uint32_t)0;
    g.light.transitionEnabled  = o["transitionEnabled"] | false;
    g.light.transitionTime     = o["transitionTime"]    | 10.0f;
    g.light.proximityScale     = o["proximityScale"]    | 1.0f;
}

static void applyDoc(JsonDocument& doc) {
    strlcpy(Config::get().deviceName,   doc["deviceName"]   | "batterylight", sizeof(Config::get().deviceName));
    strlcpy(Config::get().wifiSsid,     doc["wifiSsid"]     | "",             sizeof(Config::get().wifiSsid));
    strlcpy(Config::get().wifiPassword, doc["wifiPassword"] | "",             sizeof(Config::get().wifiPassword));
    strlcpy(Config::get().apPassword,   doc["apPassword"]   | "bl-9f4a2c81", sizeof(Config::get().apPassword));
    Config::get().otaPort        = doc["otaPort"]        | 3232;
    Config::get().otaEnabled     = doc["otaEnabled"]     | true;
    Config::get().groupId        = doc["groupId"]        | (uint8_t)0;
    Config::get().sceneSyncEnabled = doc["sceneSyncEnabled"] | true;
    Config::get().ledType   = (LedType)(uint8_t)(doc["ledType"]  | 0);
    Config::get().logLevel  = doc["logLevel"]  | (uint8_t)0;
    strlcpy(Config::get().mqttHost,     doc["mqttHost"]     | "",    sizeof(Config::get().mqttHost));
    Config::get().mqttPort  = doc["mqttPort"]  | (uint16_t)1883;
    strlcpy(Config::get().mqttUser,     doc["mqttUser"]     | "",    sizeof(Config::get().mqttUser));
    strlcpy(Config::get().mqttPassword, doc["mqttPassword"] | "",    sizeof(Config::get().mqttPassword));
    strlcpy(Config::get().githubToken,  doc["githubToken"]  | "",    sizeof(Config::get().githubToken));
    strlcpy(Config::get().githubRepo,   doc["githubRepo"]   | "variour/batterylight", sizeof(Config::get().githubRepo));

    if (doc["groups"].is<JsonArray>()) {
        for (JsonVariant v : doc["groups"].as<JsonArray>()) {
            uint8_t id = v["id"] | (uint8_t)0;
            if (id < MAX_GROUPS) deserializeGroup(v, Config::get().groups[id]);
        }
    }
}

bool Config::load() {
    // Try LittleFS first
    if (LittleFS.exists(_path)) {
        File f = LittleFS.open(_path, "r");
        if (f) {
            JsonDocument doc;
            bool ok = !deserializeJson(doc, f);
            f.close();
            if (ok) {
                applyDoc(doc);
                _ensureDefaultGroup();
                if (!group(_cfg.groupId)) _cfg.groupId = 0;
                Logger::d("[cfg] loaded from LittleFS");
                return true;
            }
        }
    }

    // LittleFS config missing or corrupt — try NVS backup.
    // NVS lives in its own partition and survives both firmware and FS OTA updates,
    // so config is always recoverable even after a full filesystem erase.
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/true)) {
        String json = prefs.getString(NVS_KEY, "");
        prefs.end();
        if (json.length() > 0) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
                applyDoc(doc);
                _ensureDefaultGroup();
                if (!group(_cfg.groupId)) _cfg.groupId = 0;
                Logger::i("[cfg] no LittleFS config — restored from NVS");
                save();  // write back to LittleFS so future boots don't need NVS
                return true;
            }
        }
    }

    Logger::w("[cfg] no saved config — using defaults");
    _ensureDefaultGroup();
    // Derive a unique default name from the factory MAC so devices are distinguishable
    // out of the box without any configuration.
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(_cfg.deviceName, sizeof(_cfg.deviceName), "light-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return false;
}

bool Config::save() {
    JsonDocument doc;
    doc["deviceName"]   = _cfg.deviceName;
    doc["wifiSsid"]     = _cfg.wifiSsid;
    doc["wifiPassword"] = _cfg.wifiPassword;
    doc["apPassword"]   = _cfg.apPassword;
    doc["otaPort"]           = _cfg.otaPort;
    doc["otaEnabled"]        = _cfg.otaEnabled;
    doc["groupId"]           = _cfg.groupId;
    doc["sceneSyncEnabled"]  = _cfg.sceneSyncEnabled;
    doc["ledType"]      = (uint8_t)_cfg.ledType;
    doc["logLevel"]     = _cfg.logLevel;
    doc["mqttHost"]     = _cfg.mqttHost;
    doc["mqttPort"]     = _cfg.mqttPort;
    doc["mqttUser"]     = _cfg.mqttUser;
    doc["mqttPassword"] = _cfg.mqttPassword;
    doc["githubToken"]  = _cfg.githubToken;
    doc["githubRepo"]   = _cfg.githubRepo;

    JsonArray arr = doc["groups"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_GROUPS; i++)
        if (_cfg.groups[i].exists) serializeGroup(arr, _cfg.groups[i]);

    // Write to LittleFS
    bool fsOk = false;
    File f = LittleFS.open(_path, "w");
    if (f) { serializeJson(doc, f); f.close(); fsOk = true; }
    if (!fsOk) Logger::e("[cfg] LittleFS write failed");

    // Mirror to NVS — survives both firmware and FS OTA partition erasures.
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
    if (prefs.begin(NVS_NS, false)) { prefs.clear(); prefs.end(); }
}

uint8_t Config::createGroup(const char* name) {
    for (uint8_t i = 1; i < MAX_GROUPS; i++) {
        if (!_cfg.groups[i].exists) {
            _cfg.groups[i] = GroupConfig{};
            _cfg.groups[i].id     = i;
            _cfg.groups[i].exists = true;
            strlcpy(_cfg.groups[i].name, name, sizeof(_cfg.groups[i].name));
            return i;
        }
    }
    return 0xFF;
}

bool Config::applyGroupSync(const GroupConfig& g) {
    if (g.id >= MAX_GROUPS) return false;
    if (!g.exists) {
        _cfg.groups[g.id].exists = false;
        return false;
    }
    // A device that was offline may broadcast a stale GroupSync on rejoin.
    // Keep whichever light config has the higher seq number so the most
    // recently-changed settings always win, regardless of who sends first.
    bool hadGroup   = _cfg.groups[g.id].exists;
    uint32_t ourSeq = hadGroup ? _cfg.groups[g.id].light.seq : 0;
    bool lightWins  = !hadGroup || g.light.seq >= ourSeq;

    LightConfig savedLight = _cfg.groups[g.id].light;
    _cfg.groups[g.id] = g;
    if (!lightWins) _cfg.groups[g.id].light = savedLight;

    return lightWins;
}

void Config::applyConfigSync(const char* json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) {
        Logger::e("[cfg] applyConfigSync: bad json");
        return;
    }
    auto& c = _cfg;
    if (!doc["wifiSsid"].isNull())     strlcpy(c.wifiSsid,     doc["wifiSsid"],     sizeof(c.wifiSsid));
    if (!doc["wifiPassword"].isNull()) strlcpy(c.wifiPassword, doc["wifiPassword"], sizeof(c.wifiPassword));
    if (!doc["mqttHost"].isNull())     strlcpy(c.mqttHost,     doc["mqttHost"],     sizeof(c.mqttHost));
    if (!doc["mqttPort"].isNull())     c.mqttPort = (uint16_t)doc["mqttPort"];
    if (!doc["mqttUser"].isNull())     strlcpy(c.mqttUser,     doc["mqttUser"],     sizeof(c.mqttUser));
    if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0)
        strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
    if (!doc["githubRepo"].isNull())   strlcpy(c.githubRepo,   doc["githubRepo"],   sizeof(c.githubRepo));
    if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
        strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));
    if (!doc["otaEnabled"].isNull())       c.otaEnabled      = (bool)doc["otaEnabled"];
    if (!doc["sceneSyncEnabled"].isNull()) c.sceneSyncEnabled = (bool)doc["sceneSyncEnabled"];
    save();
    Logger::i("[cfg] config sync applied, restarting");
    delay(200);
    ESP.restart();
}

void Config::_ensureDefaultGroup() {
    if (!_cfg.groups[0].exists) {
        _cfg.groups[0].id     = 0;
        _cfg.groups[0].exists = true;
        strlcpy(_cfg.groups[0].name, "Default", sizeof(_cfg.groups[0].name));
    }
}
