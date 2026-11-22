#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "../config/Config.h"

// Lamp layout and physical brightness model, see docs/lamp/README.md.
//
// A lamp file (/lamp.json) describes which modules (LED triangles) hang on
// which output, where they sit and what every LED channel emits. Lamp loads
// it once at boot into a flat per-channel table per output; LampDriver turns
// pattern colours into channel duties with it.
namespace lamp {

enum Kind : uint8_t { K_NONE = 0, K_R, K_G, K_B, K_WW, K_CW, K_A, K_COUNT };
enum Family : uint8_t { F_RGB = 0, F_WHITE = 1, F_AMBER = 2, F_COUNT = 3 };

enum class Mode : uint8_t {
    Raw = 0,        // colours go to the wire unchanged, brightness linear
    Uniform = 1,    // area brightness capped at the lamp's uniform ceiling
    WithPeaks = 2,  // uniform up to the ceiling, stronger modules rise beyond it
};

static constexpr const char* FILE_PATH = "/lamp.json";
static constexpr const char* TMP_PATH = "/lamp.tmp";
static constexpr uint16_t MAX_PIXELS = 1024;
#if CONFIG_IDF_TARGET_ESP32C3
static constexpr size_t MAX_FILE_BYTES = 48 * 1024;
#else
static constexpr size_t MAX_FILE_BYTES = 160 * 1024;
#endif

// Fixed brightness scale: b (1-255) -> area brightness in lm/dm², 0 = off.
static constexpr float SCALE_MIN = 0.5f;
static constexpr float SCALE_MAX = 2000.0f;
float levelToDensity(uint8_t b);

// One LED channel. Channels of one pixel that share a position form one
// emitter; firstOfEmitter marks its first channel.
struct Channel {
    float lm = 0;                // flux at 100 % duty
    float area = 0;              // emitter's share of module area, dm² (set on the first channel)
    int16_t pos[3] = {0, 0, 0};  // lamp coordinates, 0.1 mm
    uint8_t kind = K_NONE;
    uint8_t family = F_RGB;
    uint8_t modMask = 0;  // families present on this channel's module (bit per Family)
    bool firstOfEmitter = true;
};

struct Output {
    char id[8] = {};
    uint8_t light = 0xFF;
    uint16_t modules = 0;
    uint16_t pixels = 0;
    std::vector<Channel> ch;  // pixels * 3, wire order
};

class Lamp {
   public:
    // Loads /lamp.json, if present. Call once after LittleFS is mounted.
    static void load();
    static bool loaded() { return _loaded; }
    static const char* name() { return _name; }
    static uint8_t outputCount() { return _outputCount; }
    static const Output& output(uint8_t i) { return _outputs[i]; }
    // Output driving this light slot, or nullptr.
    static const Output* outputForLight(uint8_t light);

    static Mode mode() { return (Mode)Config::get().lampMode; }

    // Uniform ceiling of a family with all its channels at full, lm/dm² (0 = family absent).
    static float ceiling(uint8_t family);
    // Lamp-wide uniform ceiling for a normalised colour: the lowest ceiling
    // of the families that render it on any module, lm/dm².
    static float colourCeiling(float r, float g, float b);
    // Which family of a module with family mask `mask` renders a normalised colour.
    static uint8_t renderFamily(uint8_t mask, float r, float g, float b);
    // Grid area of all emitters of a family, dm².
    static float familyArea(uint8_t family) { return _famArea[family]; }
    static float zMin() { return _zMin; }
    static float zMax() { return _zMax; }

    // Duties (0-255) for the channels [ch, ch + n) of one emitter showing
    // colour (r, g, b) at area brightness M.
    static void emitterDuty(const Channel* ch, uint8_t n, uint8_t r, uint8_t g, uint8_t b, float M,
                            uint8_t* out);

    // Validates a lamp file and fills the summary. On success the outputs
    // (with their "light") are returned in outs. err names the first problem.
    static bool parse(JsonDocument& doc, Output* outs, uint8_t& outCount, char* nameOut,
                      size_t nameLen, const char*& err);

   private:
    static void _computeCeilings();
    static float _ceilingFor(uint8_t family, const float* w);

    static bool _loaded;
    static char _name[32];
    static Output _outputs[MAX_LIGHTS];
    static uint8_t _outputCount;
    // _dens[f][k]: smallest lm/dm² of kind k over the emitters of family f (0 = none).
    static float _dens[F_COUNT][K_COUNT];
    static float _famArea[F_COUNT];
    static uint8_t _masks[8];
    static uint8_t _maskCount;
    static float _zMin, _zMax;
};

// Family weights for a normalised colour (max component 1).
void familyWeights(uint8_t family, float r, float g, float b, float* w /* K_COUNT */);

}  // namespace lamp
