#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

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
    Gradient  = 3,
    Text      = 4,  // matrix lights only
    Time      = 5,  // matrix lights only
};

enum class TextAnimation : uint8_t {
    Scroll = 0,
    Bounce = 1,
};

// Actions a button (or, perspectively, any other trigger source — MQTT/API/mesh)
// can invoke against a group's LightConfig. Every action targets a group id;
// GroupSyncToggle is the one exception that mutates GroupConfig.syncEnabled
// instead of LightConfig.
enum class ActionId : uint8_t {
    None = 0,
    BrightnessStep,
    BrightnessSet,
    ColorSet,
    SpeedStep,
    ModeSet,
    PatternNext,
    PatternPrev,
    PatternSet,
    SceneNext,
    ScenePrev,
    SceneRandom,
    SceneSet,
    FrameDurationStep,
    TransitionToggle,
    TransitionTimeSet,
    SceneUniformColorToggle,
    ProximityScaleStep,
    ProximityScaleSet,
    GradientPaletteNext,
    GradientPalettePrev,
    GradientPaletteRandom,
    GradientPaletteSet,
    MorphToggle,
    GradientStopCountSet,
    TextSet,
    TextAnimationSet,
    Time24hToggle,
    GroupSyncToggle,
};

struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    Color() = default;
    Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}

    static uint8_t lerp(uint8_t a, uint8_t b, float t) {
        return (uint8_t)((float)a + ((float)b - (float)a) * t);
    }
};

// Scene id buffer size, incl. null terminator (also used for wire structs in MeshTypes.h).
static constexpr uint8_t SCENE_ID_LEN = 33;

struct LightConfig {
    GroupMode mode              = GroupMode::Pattern;
    char      sceneId[SCENE_ID_LEN] = {};
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
    bool      morphEnabled      = false;  // gradient mode: stops continuously wander to new random palette colors
    uint8_t   gradientStopCount = 0;      // gradient mode: manual stop count override; 0 = auto (GradientCommon::targetStopCount)
    char          text[64]      = {};     // text mode: message to display
    TextAnimation textAnimation = TextAnimation::Scroll;  // text mode: how overflowing text moves
    bool          time24h       = true;   // time mode: 24h (HH:MM) vs 12h (hh:MM) display
};

static constexpr uint8_t MAX_GROUPS        = 8;
static constexpr uint8_t MAX_LIGHTS        = 4;
static constexpr uint8_t MAX_BUTTONS       = 4;
static constexpr uint8_t MAX_WIFI_NETWORKS = 5;
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 2;

// Parameters for a ButtonAction. Which member is meaningful depends on the
// ActionId — numberValue covers steps/fixed values/enum ordinals,
// stringValue covers sceneId or free text, colorValue covers ColorSet.
struct ActionParams {
    float   numberValue     = 0.0f;
    char    stringValue[64] = {};
    Color   colorValue;
};

// One action assignment: what to do (ActionId), which group to do it to,
// and any parameters the action needs. action == ActionId::None means the
// slot is unassigned.
struct ButtonAction {
    ActionId     action  = ActionId::None;
    uint8_t      groupId = 0;
    ActionParams params;
};

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
    // Mesh-internal revision + origin for name/exists/syncEnabled, independent
    // of light.seq — same convergence pattern as DeviceConfig::wifiPolicyRevision/
    // wifiPolicyOriginMac (see MeshManager::MeshPolicyState). Monotonic per
    // group id: survives delete/recreate cycles (persisted separately in
    // Config::save/load as "groupRevisions", not via serializeGroup) so a stale
    // cached peer can never out-rank a new group created in a reused slot.
    // Deliberately not part of serializeGroup/deserializeGroup — like the
    // wifiPolicy fields, this stays out of the REST/WebSocket API surface.
    uint32_t    revision     = 0;
    uint8_t     originMac[6] = {};
};

// Shared JSON (de)serialization for LightConfig/GroupConfig fields, used by both
// Config.cpp (full config load/save) and WebServer.h (REST API request/response),
// so the field list only needs to be maintained in one place.
void serializeLightConfig(JsonObject o, const LightConfig& l);
LightConfig deserializeLightConfig(JsonVariant j, const LightConfig& def = LightConfig{});
void serializeGroup(JsonObject o, const GroupConfig& g);
void deserializeGroup(JsonVariant o, GroupConfig& g);

// Physical GPIO button configuration, stored in DeviceConfig. Each button has
// up to three independently-assignable actions (short press, long press,
// double click); an unassigned slot has action == ActionId::None.
struct ButtonHardwareConfig {
    char         name[20]  = "";
    uint8_t      pin       = 0;
    bool         activeLow = true;  // true = pressed reads LOW (INPUT_PULLUP wiring)
    ButtonAction onShortPress;
    ButtonAction onLongPress;
    ButtonAction onDoubleClick;
    bool         exists    = false;
};

void serializeButtonAction(JsonObject o, const ButtonAction& a);
ButtonAction deserializeButtonAction(JsonVariant j, const ButtonAction& def = ButtonAction{});
void serializeButton(JsonObject o, const ButtonHardwareConfig& b);
void deserializeButton(JsonVariant o, ButtonHardwareConfig& b);

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
    bool            matrixSerpentine = false; // true = alternate rows/columns (per matrixDir) run in reverse, i.e. zig-zag/boustrophedon wiring
    bool            wrapWidth   = false; // true = last LED on a row/string connects back to the first (ring)
    bool            wrapHeight  = false; // true = last row wraps to the first (matrix only, height > 1)
    uint8_t         groupId     = 0;    // which group's LightConfig this light follows
    bool            exists      = false;
};

struct DeviceConfig {
    char        deviceName[32]   = "light";
    char        apPassword[64]   = "batterylight";
    uint16_t    otaPort          = 3232;
    bool        otaEnabled       = true;
    uint8_t     logLevel         = 0;
    char        mqttHost[64]     = "";
    uint16_t    mqttPort         = 1883;
    char        mqttUser[32]     = "";
    char        mqttPassword[64] = "";
    char        githubToken[128] = "";
    char        githubRepo[64]   = "variour/batterylight";
    char        timezone[64]     = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX TZ string; default is Europe/Berlin
    bool        sceneSyncEnabled        = true;
    bool        checkUpdateOnStartup    = false;
    // Mesh-wide policy (see WifiElection.h): when true, only one candidate device
    // actually joins the configured WiFi network at a time; the rest stay AP-only.
    // This is synchronized as mesh state, not a one-shot event: revision + origin
    // metadata are persisted so peers can replay, reconcile, and resolve conflicts.
    bool        wifiSingleClientMode    = false;
    uint32_t    wifiPolicyRevision      = 0;
    uint8_t     wifiPolicyOriginMac[6]  = {};
    LightHardwareConfig lights[MAX_LIGHTS];
    GroupConfig groups[MAX_GROUPS];
    ButtonHardwareConfig buttons[MAX_BUTTONS];
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

    // Bumps a group's metadata revision and stamps it as originating from this
    // device. Call before broadcasting any name/exists/syncEnabled change.
    static void bumpGroupRevision(GroupConfig& g);

    // Invoke fn(index, light) for every configured light where light.exists is true.
    static void forEachLight(const std::function<void(uint8_t, LightHardwareConfig&)>& fn) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = _cfg.lights[i];
            if (l.exists) fn(i, l);
        }
    }

    // Like forEachLight, but stops as soon as fn returns false.
    static void forEachLightUntil(const std::function<bool(uint8_t, LightHardwareConfig&)>& fn) {
        for (uint8_t i = 0; i < MAX_LIGHTS; i++) {
            auto& l = _cfg.lights[i];
            if (l.exists && !fn(i, l)) return;
        }
    }

    // Invoke fn(index, button) for every configured button where button.exists is true.
    static void forEachButton(const std::function<void(uint8_t, ButtonHardwareConfig&)>& fn) {
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            auto& b = _cfg.buttons[i];
            if (b.exists) fn(i, b);
        }
    }

    // Like forEachButton, but stops as soon as fn returns false.
    static void forEachButtonUntil(const std::function<bool(uint8_t, ButtonHardwareConfig&)>& fn) {
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            auto& b = _cfg.buttons[i];
            if (b.exists && !fn(i, b)) return;
        }
    }

    // Merges an incoming GroupSync against local state. name/exists/syncEnabled
    // are reconciled by revision, light by light.seq — independently of each
    // other. Returns true if light was applied (caller should re-apply it to
    // the runners).
    static bool applyGroupSync(const GroupConfig& g);

    // Compares only the metadata-revision fields (revision, then originMac as a
    // deterministic tie-break) of two GroupConfigs for the same group id.
    // >0 if a is ahead of b, <0 if behind, 0 if identical.
    static int compareGroupRevision(const GroupConfig& a, const GroupConfig& b);

    static void applyConfigSync(const char* json, size_t len);

    // True if `pin` is already used by a configured light (data/clock pin) or
    // by another configured button. Pass the button's own index as
    // excludeButtonIndex when validating an update to an existing button.
    static bool isPinInUse(uint8_t pin, int8_t excludeButtonIndex = -1);

private:
    static DeviceConfig _cfg;
    static const char*  _path;
    static WifiNetwork  _wifiNetworks[MAX_WIFI_NETWORKS];
    static uint8_t      _wifiCount;
    static uint8_t      _wifiLast;
    static void _ensureDefaultGroup();
    static void _ensureDefaultLight();
};
