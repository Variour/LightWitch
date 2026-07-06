#include "Config.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "../logging/Logger.h"

DeviceConfig    Config::_cfg;
const char*     Config::_path = "/config.json";
WifiNetwork     Config::_wifiNetworks[MAX_WIFI_NETWORKS];
uint8_t         Config::_wifiCount = 0;
uint8_t         Config::_wifiLast  = 0;

static const char* _wifiPath = "/wifi.json";

static constexpr char NVS_NS[]      = "bl";
static constexpr char NVS_KEY[]     = "cfg";
static constexpr char NVS_WIFI_KEY[] = "wifi";

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
    o["sceneUniformColor"] = g.light.sceneUniformColor;
    o["transitionTime"]    = g.light.transitionTime;
    o["frameDuration"]     = g.light.frameDuration;
    o["proximityScale"]    = g.light.proximityScale;
    o["morphEnabled"]     = g.light.morphEnabled;
    o["gradientStopCount"] = g.light.gradientStopCount;
    o["text"]             = g.light.text;
    o["textAnimation"]    = (uint8_t)g.light.textAnimation;
    o["time24h"]          = g.light.time24h;
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
    g.light.sceneUniformColor  = o["sceneUniformColor"] | false;
    g.light.transitionTime     = o["transitionTime"]    | 2.0f;
    g.light.frameDuration      = o["frameDuration"]     | 1.0f;
    g.light.proximityScale     = o["proximityScale"]    | 1.0f;
    g.light.morphEnabled      = o["morphEnabled"]     | false;
    g.light.gradientStopCount = o["gradientStopCount"] | (uint8_t)0;
    strlcpy(g.light.text, o["text"] | "", sizeof(g.light.text));
    g.light.textAnimation = (TextAnimation)(uint8_t)(o["textAnimation"] | (uint8_t)TextAnimation::Scroll);
    g.light.time24h       = o["time24h"] | true;
}

static bool migrateDoc(JsonDocument& doc) {
    uint8_t ver = doc["schemaVersion"] | (uint8_t)0;
    if (ver > CONFIG_SCHEMA_VERSION) {
        Logger::w("[cfg] schema version %u > firmware max %u — ignoring", ver, CONFIG_SCHEMA_VERSION);
        return false;
    }
    if (ver < CONFIG_SCHEMA_VERSION) {
        Logger::i("[cfg] schema v%u < v%u — resetting to defaults", ver, CONFIG_SCHEMA_VERSION);
        return false;
    }
    return true;
}

static void applyDoc(JsonDocument& doc) {
    strlcpy(Config::get().deviceName,   doc["deviceName"]   | "batterylight", sizeof(Config::get().deviceName));
    strlcpy(Config::get().apPassword,   doc["apPassword"]   | "batterylight", sizeof(Config::get().apPassword));
    Config::get().otaPort        = doc["otaPort"]        | 3232;
    Config::get().otaEnabled     = doc["otaEnabled"]     | true;
    Config::get().sceneSyncEnabled     = doc["sceneSyncEnabled"]     | true;
    Config::get().checkUpdateOnStartup = doc["checkUpdateOnStartup"] | false;
    Config::get().logLevel  = doc["logLevel"]  | (uint8_t)0;
    strlcpy(Config::get().mqttHost,     doc["mqttHost"]     | "",    sizeof(Config::get().mqttHost));
    Config::get().mqttPort  = doc["mqttPort"]  | (uint16_t)1883;
    strlcpy(Config::get().mqttUser,     doc["mqttUser"]     | "",    sizeof(Config::get().mqttUser));
    strlcpy(Config::get().mqttPassword, doc["mqttPassword"] | "",    sizeof(Config::get().mqttPassword));
    strlcpy(Config::get().githubToken,  doc["githubToken"]  | "",    sizeof(Config::get().githubToken));
    strlcpy(Config::get().githubRepo,   doc["githubRepo"]   | "variour/batterylight", sizeof(Config::get().githubRepo));
    strlcpy(Config::get().timezone,     doc["timezone"]     | "UTC0", sizeof(Config::get().timezone));

    if (doc["lights"].is<JsonArray>()) {
        for (JsonVariant v : doc["lights"].as<JsonArray>()) {
            uint8_t idx = v["index"] | (uint8_t)0;
            if (idx >= MAX_LIGHTS) continue;
            auto& l = Config::get().lights[idx];
            l.exists   = v["exists"]   | false;
            l.ledType  = (LedType)(uint8_t)(v["ledType"] | 0);
            l.dataPin  = v["dataPin"]  | (uint8_t)LED_DATA_PIN;
            l.clockPin = v["clockPin"] | (uint8_t)LED_CLOCK_PIN;
            l.width    = v["width"]    | (uint16_t)1;
            l.height   = v["height"]   | (uint16_t)1;
            l.matrixStart = (MatrixStart)(uint8_t)(v["matrixStart"] | (uint8_t)MatrixStart::TopLeft);
            l.matrixDir   = (MatrixDirection)(uint8_t)(v["matrixDir"] | (uint8_t)MatrixDirection::Horizontal);
            l.matrixSerpentine = v["matrixSerpentine"] | false;
            l.wrapWidth  = v["wrapWidth"]  | false;
            l.wrapHeight = v["wrapHeight"] | false;
            l.groupId  = v["groupId"]  | (uint8_t)0;
            if (!v["name"].isNull()) strlcpy(l.name, v["name"] | "", sizeof(l.name));
        }
    }

    if (doc["groups"].is<JsonArray>()) {
        for (JsonVariant v : doc["groups"].as<JsonArray>()) {
            uint8_t id = v["id"] | (uint8_t)0;
            if (id < MAX_GROUPS) deserializeGroup(v, Config::get().groups[id]);
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
                _ensureDefaultLight();
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
                _ensureDefaultLight();
                Logger::i("[cfg] no LittleFS config — restored from NVS");
                save();
                return true;
            }
        }
    }

    Logger::w("[cfg] no saved config — using defaults");
    _ensureDefaultGroup();
    _ensureDefaultLight();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(_cfg.deviceName, sizeof(_cfg.deviceName), "light-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return false;
}

bool Config::save() {
    JsonDocument doc;
    doc["schemaVersion"] = CONFIG_SCHEMA_VERSION;
    doc["deviceName"]   = _cfg.deviceName;
    doc["apPassword"]   = _cfg.apPassword;
    doc["otaPort"]           = _cfg.otaPort;
    doc["otaEnabled"]        = _cfg.otaEnabled;
    doc["sceneSyncEnabled"]     = _cfg.sceneSyncEnabled;
    doc["checkUpdateOnStartup"] = _cfg.checkUpdateOnStartup;
    doc["logLevel"]     = _cfg.logLevel;
    doc["mqttHost"]     = _cfg.mqttHost;
    doc["mqttPort"]     = _cfg.mqttPort;
    doc["mqttUser"]     = _cfg.mqttUser;
    doc["mqttPassword"] = _cfg.mqttPassword;
    doc["githubToken"]  = _cfg.githubToken;
    doc["githubRepo"]   = _cfg.githubRepo;
    doc["timezone"]     = _cfg.timezone;

    JsonArray lightsArr = doc["lights"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
        if (!_cfg.lights[i].exists) continue;
        auto& l = _cfg.lights[i];
        JsonObject o = lightsArr.add<JsonObject>();
        o["index"]   = i;
        o["exists"]  = l.exists;
        o["name"]    = l.name;
        o["ledType"] = (uint8_t)l.ledType;
        o["dataPin"] = l.dataPin;
        o["clockPin"]= l.clockPin;
        o["width"]      = l.width;
        o["height"]     = l.height;
        o["matrixStart"]= (uint8_t)l.matrixStart;
        o["matrixDir"]  = (uint8_t)l.matrixDir;
        o["matrixSerpentine"] = l.matrixSerpentine;
        o["wrapWidth"]  = l.wrapWidth;
        o["wrapHeight"] = l.wrapHeight;
        o["groupId"]    = l.groupId;
    }

    JsonArray arr = doc["groups"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_GROUPS; i++)
        if (_cfg.groups[i].exists) serializeGroup(arr, _cfg.groups[i]);

    bool fsOk = false;
    File f = LittleFS.open(_path, "w");
    if (f) { serializeJson(doc, f); f.close(); fsOk = true; }
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
    if (!doc["mqttHost"].isNull())     strlcpy(c.mqttHost,     doc["mqttHost"],     sizeof(c.mqttHost));
    if (!doc["mqttPort"].isNull())     c.mqttPort = (uint16_t)doc["mqttPort"];
    if (!doc["mqttUser"].isNull())     strlcpy(c.mqttUser,     doc["mqttUser"],     sizeof(c.mqttUser));
    if (!doc["mqttPassword"].isNull() && strlen(doc["mqttPassword"]) > 0)
        strlcpy(c.mqttPassword, doc["mqttPassword"], sizeof(c.mqttPassword));
    if (!doc["githubRepo"].isNull())   strlcpy(c.githubRepo,   doc["githubRepo"],   sizeof(c.githubRepo));
    if (!doc["githubToken"].isNull() && strlen(doc["githubToken"]) > 0)
        strlcpy(c.githubToken, doc["githubToken"], sizeof(c.githubToken));
    if (!doc["otaEnabled"].isNull())            c.otaEnabled           = (bool)doc["otaEnabled"];
    if (!doc["sceneSyncEnabled"].isNull())      c.sceneSyncEnabled      = (bool)doc["sceneSyncEnabled"];
    if (!doc["checkUpdateOnStartup"].isNull())  c.checkUpdateOnStartup  = (bool)doc["checkUpdateOnStartup"];
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
                if (strcmp(_wifiNetworks[i].ssid, ssid) == 0) { exists = true; break; }
            if (!exists) newCount++;
        }
        if (newCount > MAX_WIFI_NETWORKS) {
            Logger::w("[cfg] addWifiNetworks would exceed %u, skipping wifi merge", MAX_WIFI_NETWORKS);
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
    _wifiLast  = 0;

    auto parseDoc = [&](JsonDocument& doc) {
        _wifiLast = doc["last"] | (uint8_t)0;
        if (!doc["networks"].is<JsonArray>()) return;
        for (JsonVariant v : doc["networks"].as<JsonArray>()) {
            if (_wifiCount >= MAX_WIFI_NETWORKS) break;
            strlcpy(_wifiNetworks[_wifiCount].ssid,     v["ssid"]     | "", sizeof(_wifiNetworks[0].ssid));
            strlcpy(_wifiNetworks[_wifiCount].password, v["password"] | "", sizeof(_wifiNetworks[0].password));
            _wifiCount++;
        }
    };

    if (LittleFS.exists(_wifiPath)) {
        File f = LittleFS.open(_wifiPath, "r");
        if (f) {
            JsonDocument doc;
            bool ok = !deserializeJson(doc, f);
            f.close();
            if (ok) { parseDoc(doc); return true; }
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
        o["ssid"]     = _wifiNetworks[i].ssid;
        o["password"] = _wifiNetworks[i].password;
    }
    File f = LittleFS.open(_wifiPath, "w");
    if (!f) { Logger::e("[cfg] wifi.json write failed"); return false; }
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
    strlcpy(_wifiNetworks[_wifiCount].ssid,     ssid,     sizeof(_wifiNetworks[0].ssid));
    strlcpy(_wifiNetworks[_wifiCount].password, password, sizeof(_wifiNetworks[0].password));
    _wifiCount++;
    return saveWifi();
}

bool Config::deleteWifiNetwork(const char* ssid) {
    for (uint8_t i = 0; i < _wifiCount; i++) {
        if (strcmp(_wifiNetworks[i].ssid, ssid) == 0) {
            for (uint8_t j = i; j < _wifiCount - 1; j++)
                _wifiNetworks[j] = _wifiNetworks[j + 1];
            _wifiCount--;
            if (_wifiLast == i) _wifiLast = 0;
            else if (_wifiLast > i) _wifiLast--;
            saveWifi();
            return true;
        }
    }
    return false;
}

void Config::_ensureDefaultGroup() {
    if (!_cfg.groups[0].exists) {
        _cfg.groups[0].id     = 0;
        _cfg.groups[0].exists = true;
        strlcpy(_cfg.groups[0].name, "Default", sizeof(_cfg.groups[0].name));
    }
}

void Config::_ensureDefaultLight() {
    // If no lights are configured, create one with hardware defaults.
    bool any = false;
    for (uint8_t i = 0; i < MAX_LIGHTS; i++) if (_cfg.lights[i].exists) { any = true; break; }
    if (!any) {
        _cfg.lights[0].exists   = true;
        _cfg.lights[0].ledType  = LedType::WS2812B;
        _cfg.lights[0].dataPin  = LED_DATA_PIN;
        _cfg.lights[0].clockPin = LED_CLOCK_PIN;
        _cfg.lights[0].width    = 1;
        _cfg.lights[0].height   = 1;
        _cfg.lights[0].groupId  = 0;
    }
}
