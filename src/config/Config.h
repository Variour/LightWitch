#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

#include "../logging/Logger.h"

#ifndef LED_DATA_PIN
#define LED_DATA_PIN 25
#endif
#ifndef LED_CLOCK_PIN
#define LED_CLOCK_PIN 26
#endif

enum class LedType : uint8_t {
    WS2812B = 0,  // single-wire NeoPixel
    WS2801 = 1,   // two-wire SPI
};

// Physical wiring order of the three colour channels on the LED hardware.
// setColor(r,g,b) always takes true RGB values; the driver permutes them into
// this order before writing to the strip, so mixed-wiring lights on the same
// device all show correct colours without touching pattern code.
enum class ColorOrder : uint8_t {
    RGB = 0,
    RBG = 1,
    GRB = 2,
    GBR = 3,
    BRG = 4,
    BGR = 5,
};

// WS2812B strips are conventionally wired GRB, WS2801 modules RGB.
inline ColorOrder defaultColorOrder(LedType t) {
    return t == LedType::WS2812B ? ColorOrder::GRB : ColorOrder::RGB;
}

// Permutes true RGB values into the wire order `order` calls for.
inline void applyColorOrder(ColorOrder order, uint8_t r, uint8_t g, uint8_t b, uint8_t& o1,
                            uint8_t& o2, uint8_t& o3) {
    switch (order) {
        case ColorOrder::RBG:
            o1 = r;
            o2 = b;
            o3 = g;
            break;
        case ColorOrder::GRB:
            o1 = g;
            o2 = r;
            o3 = b;
            break;
        case ColorOrder::GBR:
            o1 = g;
            o2 = b;
            o3 = r;
            break;
        case ColorOrder::BRG:
            o1 = b;
            o2 = r;
            o3 = g;
            break;
        case ColorOrder::BGR:
            o1 = b;
            o2 = g;
            o3 = r;
            break;
        default:  // RGB
            o1 = r;
            o2 = g;
            o3 = b;
            break;
    }
}

enum class MatrixStart : uint8_t {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
};

enum class MatrixDirection : uint8_t {
    Horizontal = 0,  // rows first
    Vertical = 1,    // columns first
};

enum class PatternId : uint8_t {
    Static = 0,
    Breathing = 1,
    ColorCycle = 2,
    Strobe = 3,
    Candle = 4,
};

enum class GroupMode : uint8_t {
    Pattern = 0,
    Scene = 1,
    Proximity = 2,
    Gradient = 3,
    Text = 4,  // matrix lights only
    Time = 5,  // matrix lights only
};

enum class TextAnimation : uint8_t {
    Scroll = 0,
    Bounce = 1,
};

// Actions a button (or, perspectively, any other trigger source — MQTT/API/mesh)
// can invoke against a group's LightConfig. Every action targets a group id;
// GroupSyncToggle is the one exception that mutates GroupConfig.syncEnabled
// instead of LightConfig. The LightBrightnessOverride* actions are a second
// exception: they target a specific physical light (ButtonAction::lightIndex)
// and mutate LightHardwareConfig::brightnessOverride(Enabled) instead.
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
    LightBrightnessOverrideStep,
    LightBrightnessOverrideSet,
    LightBrightnessOverrideClear,
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
    GroupMode mode = GroupMode::Pattern;
    char sceneId[SCENE_ID_LEN] = {};
    PatternId pattern = PatternId::Static;
    Color color = Color{255, 255, 255};
    uint8_t brightness = 255;
    float speed = 1.0f;
    // Monotonic per group id; NOT used for GroupSync-level conflict resolution
    // (see GroupConfig::revision for that) — only for the standalone
    // LightConfigMsg mesh channel's staleness check and mesh self-echo
    // suppression (see MeshManager), and to order same-device local edits.
    uint32_t seq = 0;
    bool transitionEnabled = false;
    bool sceneUniformColor = false;  // string lights: whole string shares one random scene color
    float transitionTime = 2.0f;
    float frameDuration = 1.0f;
    float proximityScale = 1.0f;
    bool morphEnabled =
        false;  // gradient mode: stops continuously wander to new random palette colors
    uint8_t gradientStopCount =
        0;  // gradient mode: manual stop count override; 0 = auto (GradientCommon::targetStopCount)
    char text[64] = {};                                   // text mode: message to display
    TextAnimation textAnimation = TextAnimation::Scroll;  // text mode: how overflowing text moves
    bool time24h = true;  // time mode: 24h (HH:MM) vs 12h (hh:MM) display
};

static constexpr uint8_t MAX_GROUPS = 8;
static constexpr uint8_t MAX_LIGHTS = 4;
static constexpr uint8_t MAX_BUTTONS = 4;
static constexpr uint8_t MAX_WIFI_NETWORKS = 5;
static constexpr uint8_t CONFIG_SCHEMA_VERSION = 2;

// Parameters for a ButtonAction. Which member is meaningful depends on the
// ActionId — numberValue covers steps/fixed values/enum ordinals,
// stringValue covers sceneId or free text, colorValue covers ColorSet.
struct ActionParams {
    float numberValue = 0.0f;
    char stringValue[64] = {};
    Color colorValue;
};

// One action assignment: what to do (ActionId), which group to do it to,
// and any parameters the action needs. action == ActionId::None means the
// slot is unassigned. lightIndex is only meaningful for the light-targeted
// actions (see ActionId) — every other action targets groupId instead.
struct ButtonAction {
    ActionId action = ActionId::None;
    uint8_t groupId = 0;
    uint8_t lightIndex = 0;
    ActionParams params;
};

struct WifiNetwork {
    char ssid[64] = {};
    char password[64] = {};
};

struct GroupConfig {
    uint8_t id = 0;
    char name[24] = {};
    LightConfig light;
    bool exists = false;
    bool syncEnabled = true;
    // Mesh-internal revision + origin governing the *whole* group (name,
    // exists, syncEnabled, and light all move together as one unit — see
    // Config::applyGroupSync) — same convergence pattern as
    // DeviceConfig::wifiPolicyRevision/wifiPolicyOriginMac (see
    // MeshManager::MeshPolicyState). Monotonic per group id: survives
    // delete/recreate cycles (persisted separately in Config::save/load as
    // "groupRevisions", not via serializeGroup) so a stale cached peer can
    // never out-rank a new group created in a reused slot — including its
    // light, which shares this same counter.
    // Deliberately not part of serializeGroup/deserializeGroup — like the
    // wifiPolicy fields, this stays out of the REST/WebSocket API surface.
    uint32_t revision = 0;
    uint8_t originMac[6] = {};
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
    char name[20] = "";
    uint8_t pin = 0;
    bool activeLow = true;  // true = pressed reads LOW (INPUT_PULLUP wiring)
    ButtonAction onShortPress;
    ButtonAction onLongPress;
    ButtonAction onDoubleClick;
    bool exists = false;
};

void serializeButtonAction(JsonObject o, const ButtonAction& a);
ButtonAction deserializeButtonAction(JsonVariant j, const ButtonAction& def = ButtonAction{});
void serializeButton(JsonObject o, const ButtonHardwareConfig& b);
void deserializeButton(JsonVariant o, ButtonHardwareConfig& b);

// Per-light physical hardware configuration, stored in DeviceConfig.
struct LightHardwareConfig {
    char name[20] = "";
    LedType ledType = LedType::WS2812B;
    // WS2812B strips are conventionally wired GRB, WS2801 modules RGB — these
    // struct defaults only cover a fresh in-memory light; Config::load()/the
    // REST handlers apply the same ledType-dependent default when the field
    // is absent from a request or an older saved config.
    ColorOrder colorOrder = ColorOrder::GRB;
    uint8_t dataPin = LED_DATA_PIN;
    uint8_t clockPin = LED_CLOCK_PIN;
    uint16_t width = 1;   // string length, or matrix columns
    uint16_t height = 1;  // 1 = string, >1 = matrix
    MatrixStart matrixStart = MatrixStart::TopLeft;
    MatrixDirection matrixDir = MatrixDirection::Horizontal;
    bool matrixSerpentine = false;  // true = alternate rows/columns (per matrixDir) run in reverse,
                                    // i.e. zig-zag/boustrophedon wiring
    bool wrapWidth = false;   // true = last LED on a row/string connects back to the first (ring)
    bool wrapHeight = false;  // true = last row wraps to the first (matrix only, height > 1)
    uint8_t groupId = 0;      // which group's LightConfig this light follows
    // When enabled, this light renders at brightnessOverride instead of the
    // group's LightConfig::brightness — local to this device only, not
    // synced over mesh (unlike GroupConfig). Disabling reverts the light to
    // following the group's brightness.
    bool brightnessOverrideEnabled = false;
    uint8_t brightnessOverride = 255;
    bool exists = false;
};

// Sentinel for an unused/unconfigured GPIO pin field on a SoundHardwareConfig.
static constexpr uint8_t SOUND_PIN_UNUSED = 0xFF;

// Sound output chip. Only ES8311 (mono I2C-controlled I2S codec) is supported
// today; the enum exists so a second chip can be added without reshaping the
// config, REST API, or UI (see LedType/LightHardwareConfig for the precedent).
enum class SoundChip : uint8_t {
    ES8311 = 0,
};

// Generic I2C GPIO expander chip. Some boards don't wire a control signal
// (e.g. speaker-amp enable) to a native ESP32 GPIO at all — it sits behind an
// expander chip shared with other peripherals instead. This is a general,
// chip-level abstraction (not board-specific) so any future pin that needs
// this can reuse it; see src/io/Tca9555Expander.h.
enum class IoExpanderChip : uint8_t {
    None = 0,     // the pin field it applies to is a native ESP32 GPIO
    TCA9555 = 1,  // the pin field it applies to is a pin index (0-15) on a TCA9555
};

// Per-sound-output physical hardware configuration, stored in DeviceConfig.
// Hardware support only for now — nothing in Config/main.cpp/WebServer yet
// decides *what* plays; that's a separate, later step (see LightHardwareConfig
// vs. LightConfig/GroupConfig for how lights eventually split this).
struct SoundHardwareConfig {
    char name[20] = "";
    SoundChip chip = SoundChip::ES8311;
    uint8_t i2cSdaPin = SOUND_PIN_UNUSED;
    uint8_t i2cSclPin = SOUND_PIN_UNUSED;
    uint8_t i2cAddress =
        0x18;  // ES8311 default 7-bit address; 0x19 if the CE/AD pin is strapped high
    // ES8311 can derive its internal MCLK from SCLK via PLL when the board doesn't
    // wire a separate MCLK line — SOUND_PIN_UNUSED selects that mode.
    uint8_t i2sMclkPin = SOUND_PIN_UNUSED;
    uint8_t i2sBclkPin = SOUND_PIN_UNUSED;
    uint8_t i2sWsPin = SOUND_PIN_UNUSED;
    uint8_t i2sDoutPin = SOUND_PIN_UNUSED;
    // Some boards gate a separate speaker amp via a GPIO instead of relying on the
    // codec's own output stage; SOUND_PIN_UNUSED = no such pin. paExpander selects
    // whether paEnablePin is a native GPIO (None) or a pin index on the expander
    // (sharing the codec's I2C bus at paExpanderAddress) — see IoExpanderChip.
    uint8_t paEnablePin = SOUND_PIN_UNUSED;
    bool paEnableActiveHigh = true;
    IoExpanderChip paExpander = IoExpanderChip::None;
    uint8_t paExpanderAddress = 0x20;  // TCA9555 default 7-bit address (A0-A2 strapped low)
    bool exists = false;
};

void serializeSound(JsonObject o, const SoundHardwareConfig& s);
void deserializeSound(JsonVariant o, SoundHardwareConfig& s);

static constexpr uint8_t MAX_SOUNDS = 1;

struct DeviceConfig {
    char deviceName[32] = "light";
    char apPassword[64] = "batterylight";
    uint16_t otaPort = 3232;
    bool otaEnabled = true;
    uint8_t logLevel = (uint8_t)LogLevel::DEBUG;
    char mqttHost[64] = "";
    uint16_t mqttPort = 1883;
    char mqttUser[32] = "";
    char mqttPassword[64] = "";
    char githubToken[128] = "";
    char githubRepo[64] = "variour/batterylight";
    char timezone[64] = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX TZ string; default is Europe/Berlin
    bool sceneSyncEnabled = true;
    bool checkUpdateOnStartup = false;
    // Mesh-wide policy (see WifiElection.h): when true, only one candidate device
    // actually joins the configured WiFi network at a time; the rest stay AP-only.
    // This is synchronized as mesh state, not a one-shot event: revision + origin
    // metadata are persisted so peers can replay, reconcile, and resolve conflicts.
    bool wifiSingleClientMode = false;
    uint32_t wifiPolicyRevision = 0;
    uint8_t wifiPolicyOriginMac[6] = {};
    // Optional: BAT_ADC (battery voltage divider) solder-bridged onto GPIO1 —
    // only meaningful (and only ever offered by the web UI) on boards where
    // that pin is ADC-capable, see BatteryMonitor::kHwSupported.
    bool batteryMonitoringEnabled = false;
    // Optional: lets the device install/track an open PR's firmware build
    // via OTA (see Updater.h) instead of only tagged releases. Off by
    // default and never enabled implicitly — installing a PR build means
    // running unreviewed, untested firmware.
    bool prOtaEnabled = false;
    // "" = tracking tagged releases as usual; "pr-<n>" = the normal update
    // check/apply flow follows that PR's release instead of the latest
    // tagged release. Set only by Updater::applyPrAsync(), never accepted
    // from the API — this is device-local state, not a pushable setting.
    char prTrack[24] = "";
    // GitHub release asset id of the firmware currently installed from
    // prTrack, so Updater can tell "newer build of the same PR" apart from
    // "already on the latest build" — a pr-<n> tag stays the same across
    // pushes, so the tag alone can't signal that. Meaningless when prTrack
    // is empty. Also internal-only, like prTrack.
    uint32_t prTrackAssetId = 0;
    LightHardwareConfig lights[MAX_LIGHTS];
    GroupConfig groups[MAX_GROUPS];
    ButtonHardwareConfig buttons[MAX_BUTTONS];
    SoundHardwareConfig sounds[MAX_SOUNDS];
};

class Config {
   public:
    static bool load();
    static bool save();
    static void reset();

    static bool loadWifi();
    static bool saveWifi();
    static bool addWifiNetwork(const char* ssid, const char* password);
    static bool deleteWifiNetwork(const char* ssid);
    static bool moveWifiNetwork(const char* ssid, int8_t direction);

    static WifiNetwork* wifiNetworks() { return _wifiNetworks; }
    static uint8_t wifiCount() { return _wifiCount; }
    static uint8_t wifiLast() { return _wifiLast; }
    static void setWifiLast(uint8_t i) { _wifiLast = i; }

    static DeviceConfig& get() { return _cfg; }

    static GroupConfig* group(uint8_t id) {
        if (id < MAX_GROUPS && _cfg.groups[id].exists) return &_cfg.groups[id];
        return nullptr;
    }

    // Creates a group in the first free slot (never slot 0, the permanent
    // Default group). Internally calls bumpGroupRevision on it, so the
    // returned group's revision/originMac are already ready to broadcast —
    // callers must not call bumpGroupRevision again for the creation itself.
    static uint8_t createGroup(const char* name);

    // Bumps a group's revision and stamps it as originating from this device.
    // Call before broadcasting any name/exists/syncEnabled/light change —
    // except right after createGroup(), which already does this internally.
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

    // Invoke fn(index, sound) for every configured sound output where sound.exists is true.
    static void forEachSound(const std::function<void(uint8_t, SoundHardwareConfig&)>& fn) {
        for (uint8_t i = 0; i < MAX_SOUNDS; i++) {
            auto& s = _cfg.sounds[i];
            if (s.exists) fn(i, s);
        }
    }

    // Merges an incoming GroupSync against local state. A single revision +
    // originMac now governs the whole group (see GroupConfig::revision), so
    // this either adopts the incoming group wholesale or rejects it wholesale
    // — no per-field splitting. Returns true if it was adopted (caller should
    // re-apply light to the runners and persist).
    static bool applyGroupSync(const GroupConfig& g);

    // Compares only the revision fields (revision, then originMac as a
    // deterministic tie-break) of two GroupConfigs for the same group id.
    // >0 if a is ahead of b, <0 if behind, 0 if identical.
    static int compareGroupRevision(const GroupConfig& a, const GroupConfig& b);

    static void applyConfigSync(const char* json, size_t len);

    // True if `pin` is already used by a configured light (dataPin always;
    // clockPin only when that light is WS2801 — WS2812B is single-wire, so a
    // default/leftover clockPin value there is never actually driven), a
    // configured sound output (I2C/I2S pins, plus its PA-enable pin only when
    // that's a native GPIO — an expander-backed PA pin lives in a separate
    // address space, see IoExpanderChip), or by another configured button.
    // Pass the button's own index as excludeButtonIndex, a sound's own index
    // as excludeSoundIndex, or a light's own index as excludeLightIndex, when
    // validating an update to an existing button/sound/light. SOUND_PIN_UNUSED
    // never counts as "in use".
    static bool isPinInUse(uint8_t pin, int8_t excludeButtonIndex = -1,
                           int8_t excludeSoundIndex = -1, int8_t excludeLightIndex = -1);

   private:
    static DeviceConfig _cfg;
    static const char* _path;
    static WifiNetwork _wifiNetworks[MAX_WIFI_NETWORKS];
    static uint8_t _wifiCount;
    static uint8_t _wifiLast;
    static void _ensureDefaultGroup();
};
