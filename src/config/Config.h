#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum class LedType : uint8_t {
    WS2812B = 0,  // single-wire NeoPixel — data on LED_DATA_PIN (default GPIO 25, ESP32-C3 uses GPIO 20)
    WS2801  = 1,  // two-wire SPI — data on LED_DATA_PIN, clock on LED_CLOCK_PIN (default GPIO 25/26, ESP32-C3 uses GPIO 20/21)
};

enum class PatternId : uint8_t {
    Static     = 0,
    Breathing  = 1,
    ColorCycle = 2,
    Strobe     = 3,
    Candle     = 4,
};

enum class GroupMode : uint8_t {
    Pattern   = 0,
    Scene     = 1,
    Proximity = 2,
};

struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    Color() = default;
    Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
};

struct LightConfig {
    GroupMode mode               = GroupMode::Pattern;
    char      sceneId[33]       = {};
    PatternId pattern           = PatternId::Static;
    Color     color             = Color{255, 255, 255};
    uint8_t   brightness        = 255;
    float     speed             = 1.0f;
    uint32_t  seq               = 0;
    bool      transitionEnabled = false;
    float     transitionTime    = 10.0f;
    float     proximityScale    = 1.0f;
};

static constexpr uint8_t MAX_GROUPS        = 8;
static constexpr uint8_t MAX_WIFI_NETWORKS = 5;
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 1;

struct WifiNetwork {
    char ssid[64]     = {};
    char password[64] = {};
};

struct GroupConfig {
    uint8_t     id          = 0;
    char        name[24]    = {};
    LightConfig light;
    bool        exists      = false;
    bool        syncEnabled = true;  // sync animation phase across group members
};

struct DeviceConfig {
    char        deviceName[32]   = "light";
    char        apPassword[64]   = "bl-9f4a2c81";
    uint16_t    otaPort          = 3232;
    bool        otaEnabled       = true;
    uint8_t     groupId          = 0;
    LedType     ledType          = LedType::WS2812B;
    uint8_t     logLevel         = 0;  // LogLevel::DEBUG
    char        mqttHost[64]     = "";
    uint16_t    mqttPort         = 1883;
    char        mqttUser[32]     = "";
    char        mqttPassword[64] = "";
    char        githubToken[128] = "";
    char        githubRepo[64]   = "variour/batterylight";
    bool        sceneSyncEnabled        = true;
    bool        checkUpdateOnStartup    = false;
    GroupConfig groups[MAX_GROUPS];
};

class Config {
public:
    static bool    load();
    static bool    save();
    static void    reset();

    static bool    loadWifi();
    static bool    saveWifi();
    // Returns false if cap reached and SSID is not already in the list.
    // If SSID already exists, updates its password and returns true.
    static bool    addWifiNetwork(const char* ssid, const char* password);
    static bool    deleteWifiNetwork(const char* ssid);

    static WifiNetwork* wifiNetworks() { return _wifiNetworks; }
    static uint8_t      wifiCount()    { return _wifiCount; }
    static uint8_t      wifiLast()     { return _wifiLast; }
    static void         setWifiLast(uint8_t i) { _wifiLast = i; }

    static DeviceConfig& get()   { return _cfg; }

    // Light config for the group this device currently belongs to
    static LightConfig& light() {
        auto* g = group(_cfg.groupId);
        return g ? g->light : _cfg.groups[0].light;
    }

    static GroupConfig* group(uint8_t id) {
        if (id < MAX_GROUPS && _cfg.groups[id].exists) return &_cfg.groups[id];
        return nullptr;
    }

    // Create a new group; returns the new id or 0xFF if full
    static uint8_t createGroup(const char* name);

    // Apply a GroupConfig received from the mesh (create/update/delete).
    // Returns true if the light config was updated (incoming seq >= local seq).
    static bool applyGroupSync(const GroupConfig& g);

    // Apply syncable settings received via config push, save, and restart.
    static void applyConfigSync(const char* json, size_t len);

private:
    static DeviceConfig _cfg;
    static const char*  _path;
    static WifiNetwork  _wifiNetworks[MAX_WIFI_NETWORKS];
    static uint8_t      _wifiCount;
    static uint8_t      _wifiLast;
    static void _ensureDefaultGroup();
};
