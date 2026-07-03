#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#ifndef LED_DATA_PIN
#define LED_DATA_PIN 25
#endif
#ifndef LED_CLOCK_PIN
#define LED_CLOCK_PIN 26
#endif

enum class LedType : uint8_t {
    WS2812B = 0,  // single-wire NeoPixel
    WS2801  = 1,  // two-wire SPI
};

enum class MatrixStart : uint8_t {
    TopLeft     = 0,
    TopRight    = 1,
    BottomLeft  = 2,
    BottomRight = 3,
};

enum class MatrixDirection : uint8_t {
    Horizontal = 0,  // rows first
    Vertical   = 1,  // columns first
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
    GroupMode mode              = GroupMode::Pattern;
    char      sceneId[33]       = {};
    PatternId pattern           = PatternId::Static;
    Color     color             = Color{255, 255, 255};
    uint8_t   brightness        = 255;
    float     speed             = 1.0f;
    uint32_t  seq               = 0;
    bool      transitionEnabled = false;
    bool      sceneUniformColor = false;  // string lights: whole string shares one random scene color
    float     transitionTime    = 2.0f;
    float     frameDuration     = 1.0f;
    float     proximityScale    = 1.0f;
};

static constexpr uint8_t MAX_GROUPS        = 8;
static constexpr uint8_t MAX_LIGHTS        = 4;
static constexpr uint8_t MAX_WIFI_NETWORKS = 5;
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 2;

struct WifiNetwork {
    char ssid[64]     = {};
    char password[64] = {};
};

struct GroupConfig {
    uint8_t     id          = 0;
    char        name[24]    = {};
    LightConfig light;
    bool        exists      = false;
    bool        syncEnabled = true;
};

// Per-light physical hardware configuration, stored in DeviceConfig.
struct LightHardwareConfig {
    char     name[20]  = "";
    LedType  ledType   = LedType::WS2812B;
    uint8_t  dataPin   = LED_DATA_PIN;
    uint8_t  clockPin  = LED_CLOCK_PIN;
    uint16_t        width       = 1;    // string length, or matrix columns
    uint16_t        height      = 1;    // 1 = string, >1 = matrix
    MatrixStart     matrixStart = MatrixStart::TopLeft;
    MatrixDirection matrixDir   = MatrixDirection::Horizontal;
    bool            wrapWidth   = false; // true = last LED on a row/string connects back to the first (ring)
    bool            wrapHeight  = false; // true = last row wraps to the first (matrix only, height > 1)
    uint8_t         groupId     = 0;    // which group's LightConfig this light follows
    bool            exists      = false;
};

struct DeviceConfig {
    char        deviceName[32]   = "light";
    char        apPassword[64]   = "bl-9f4a2c81";
    uint16_t    otaPort          = 3232;
    bool        otaEnabled       = true;
    uint8_t     logLevel         = 0;
    char        mqttHost[64]     = "";
    uint16_t    mqttPort         = 1883;
    char        mqttUser[32]     = "";
    char        mqttPassword[64] = "";
    char        githubToken[128] = "";
    char        githubRepo[64]   = "variour/batterylight";
    bool        sceneSyncEnabled        = true;
    bool        checkUpdateOnStartup    = false;
    LightHardwareConfig lights[MAX_LIGHTS];
    GroupConfig groups[MAX_GROUPS];
};

class Config {
public:
    static bool    load();
    static bool    save();
    static void    reset();

    static bool    loadWifi();
    static bool    saveWifi();
    static bool    addWifiNetwork(const char* ssid, const char* password);
    static bool    deleteWifiNetwork(const char* ssid);

    static WifiNetwork* wifiNetworks() { return _wifiNetworks; }
    static uint8_t      wifiCount()    { return _wifiCount; }
    static uint8_t      wifiLast()     { return _wifiLast; }
    static void         setWifiLast(uint8_t i) { _wifiLast = i; }

    static DeviceConfig& get()   { return _cfg; }

    static GroupConfig* group(uint8_t id) {
        if (id < MAX_GROUPS && _cfg.groups[id].exists) return &_cfg.groups[id];
        return nullptr;
    }

    static uint8_t createGroup(const char* name);

    static bool applyGroupSync(const GroupConfig& g);

    static void applyConfigSync(const char* json, size_t len);

private:
    static DeviceConfig _cfg;
    static const char*  _path;
    static WifiNetwork  _wifiNetworks[MAX_WIFI_NETWORKS];
    static uint8_t      _wifiCount;
    static uint8_t      _wifiLast;
    static void _ensureDefaultGroup();
    static void _ensureDefaultLight();
};
