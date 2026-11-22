// Brightness: the per-light clamp (src/led/LedDriver.h), the lamp brightness
// model (src/lamp/Lamp.cpp, docs/lamp/README.md), LampDriver in front of a
// real driver (src/led/LampDriver.h) and the game LED levels
// (src/game/GameRender.h).
#include <LittleFS.h>
#include <unity.h>

#include <vector>

#include "game/GameRender.h"
#include "lamp/Lamp.cpp"
#include "led/LampDriver.h"

// Config.cpp needs the ESP32 core; the tests only use the config struct.
DeviceConfig Config::_cfg;

using namespace lamp;

// Records what reaches the hardware.
struct RecordingDriver : LedDriver {
    struct Px {
        uint8_t r, g, b;
    };
    std::vector<Px> px = std::vector<Px>(16, Px{0, 0, 0});
    Px color{0, 0, 0};
    int shows = 0;
    void begin() override {}
    void show() override { shows++; }
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override { color = {r, g, b}; }
    void writePixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b) override {
        if (i < px.size()) px[i] = {r, g, b};
    }
};

// Two RGB modules of 100 mm² on light 0: "weak" (r 1 lm, g 2 lm, b 0.5 lm)
// and "strong" (four times that), so densities are 100/200/50 and
// 400/800/200 lm/dm².
static const char* kTwoModules = R"({
  "format": "lightwitch-lamp", "version": 1, "name": "Test",
  "types": {
    "weak":   {"area": 100, "pixels": [{"ch": [
      {"k": "r", "lm": 1,   "at": [0, 0]},
      {"k": "g", "lm": 2,   "at": [0, 0]},
      {"k": "b", "lm": 0.5, "at": [0, 0]}]}]},
    "strong": {"area": 100, "pixels": [{"ch": [
      {"k": "r", "lm": 4, "at": [0, 0]},
      {"k": "g", "lm": 8, "at": [0, 0]},
      {"k": "b", "lm": 2, "at": [0, 0]}]}]}
  },
  "outputs": [{"id": "a", "light": 0, "modules": [
    {"t": "weak",   "p": [0, 0, 0],  "n": [0, 0, 1], "u": [0, 1, 0]},
    {"t": "strong", "p": [10, 0, 5], "n": [0, 0, 1], "u": [0, 1, 0]}]}]
})";

static void loadLamp(const char* json, Mode mode = Mode::Uniform) {
    Config::get() = DeviceConfig();
    Config::get().lights[0].exists = true;
    Config::get().lampMode = (uint8_t)mode;
    LittleFS.clear();
    LittleFS.put(FILE_PATH, json);
    Lamp::load();
}

static const Channel* emitter(uint16_t pixel) {
    return Lamp::outputForLight(0)->ch.data() + (size_t)pixel * 3;
}

static void duty(uint16_t pixel, uint8_t r, uint8_t g, uint8_t b, float M, uint8_t* out) {
    Lamp::emitterDuty(emitter(pixel), 3, r, g, b, M, out);
}

void setUp() {}
void tearDown() {}

// ── LedDriver clamp ──────────────────────────────────────────────────────────

static void test_clamp_defaults_to_pass_through() {
    RecordingDriver d;
    d.setColor(255, 128, 1);
    TEST_ASSERT_EQUAL(255, d.color.r);
    TEST_ASSERT_EQUAL(128, d.color.g);
    TEST_ASSERT_EQUAL(1, d.color.b);
}

static void test_clamp_scale_damps_proportionally() {
    RecordingDriver d;
    d.setBrightnessClamp(255, 128);
    d.setColor(255, 100, 1);
    TEST_ASSERT_EQUAL(128, d.color.r);
    TEST_ASSERT_EQUAL(50, d.color.g);
    TEST_ASSERT_EQUAL(0, d.color.b);
}

static void test_clamp_limit_caps_after_scale() {
    RecordingDriver d;
    d.setBrightnessClamp(100, 255);
    d.setPixel(3, 255, 100, 50);
    TEST_ASSERT_EQUAL(100, d.px[3].r);
    TEST_ASSERT_EQUAL(100, d.px[3].g);
    TEST_ASSERT_EQUAL(50, d.px[3].b);
    d.setBrightnessClamp(100, 128);
    d.setPixel(3, 255, 100, 50);
    TEST_ASSERT_EQUAL(100, d.px[3].r);  // 128 capped to 100
    TEST_ASSERT_EQUAL(50, d.px[3].g);
    TEST_ASSERT_EQUAL(25, d.px[3].b);
}

static void test_clamp_zero_blanks() {
    RecordingDriver d;
    d.setBrightnessClamp(0, 255);
    d.setColor(255, 255, 255);
    TEST_ASSERT_EQUAL(0, d.color.r);
    TEST_ASSERT_EQUAL(0, d.color.g);
    TEST_ASSERT_EQUAL(0, d.color.b);
}

// ── Brightness scale ─────────────────────────────────────────────────────────

static void test_level_scale_end_points() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, levelToDensity(0));
    TEST_ASSERT_EQUAL_FLOAT(SCALE_MIN, levelToDensity(1));
    TEST_ASSERT_FLOAT_WITHIN(SCALE_MAX * 0.01f, SCALE_MAX, levelToDensity(255));
}

static void test_level_scale_is_logarithmic_and_monotonic() {
    // M(128) = 0.5 * 4000^(127/254) = 0.5 * sqrt(4000)
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 0.5f * sqrtf(4000.0f), levelToDensity(128));
    for (int b = 2; b <= 255; b++)
        TEST_ASSERT_TRUE(levelToDensity((uint8_t)b) > levelToDensity((uint8_t)(b - 1)));
}

// ── Colour families ──────────────────────────────────────────────────────────

static void test_family_weights() {
    float w[K_COUNT];
    familyWeights(F_RGB, 1.0f, 0.5f, 0.25f, w);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, w[K_R]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, w[K_G]);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, w[K_B]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w[K_WW]);

    familyWeights(F_WHITE, 1.0f, 0.6f, 0.2f, w);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, w[K_A]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, w[K_WW]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2f, w[K_CW]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w[K_R]);

    familyWeights(F_AMBER, 0.8f, 0.1f, 0.0f, w);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, w[K_A]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w[K_WW]);
}

static void test_render_family_choice() {
    const uint8_t rgbWhite = (1 << F_RGB) | (1 << F_WHITE);
    const uint8_t whiteAmber = (1 << F_WHITE) | (1 << F_AMBER);
    TEST_ASSERT_EQUAL(F_RGB, Lamp::renderFamily(rgbWhite, 1.0f, 0.0f, 0.0f));  // saturated
    TEST_ASSERT_EQUAL(F_RGB, Lamp::renderFamily(rgbWhite, 0.2f, 1.0f, 0.2f));  // not warm
    TEST_ASSERT_EQUAL(F_WHITE, Lamp::renderFamily(rgbWhite, 1.0f, 0.9f, 0.8f));
    TEST_ASSERT_EQUAL(F_AMBER, Lamp::renderFamily(whiteAmber, 1.0f, 0.3f, 0.1f));  // glow
    TEST_ASSERT_EQUAL(F_WHITE, Lamp::renderFamily(whiteAmber, 1.0f, 0.6f, 0.2f));
    TEST_ASSERT_EQUAL(F_WHITE, Lamp::renderFamily(whiteAmber, 0.2f, 1.0f, 0.2f));  // no RGB
}

// ── Lamp file ────────────────────────────────────────────────────────────────

static void test_lamp_loads_outputs_and_emitters() {
    loadLamp(kTwoModules);
    TEST_ASSERT_TRUE(Lamp::loaded());
    TEST_ASSERT_EQUAL_STRING("Test", Lamp::name());
    TEST_ASSERT_EQUAL(1, Lamp::outputCount());
    const Output* o = Lamp::outputForLight(0);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_NULL(Lamp::outputForLight(1));
    TEST_ASSERT_EQUAL(2, o->modules);
    TEST_ASSERT_EQUAL(2, o->pixels);
    // r, g and b of a pixel share one position: one emitter.
    TEST_ASSERT_TRUE(o->ch[0].firstOfEmitter);
    TEST_ASSERT_FALSE(o->ch[1].firstOfEmitter);
    TEST_ASSERT_FALSE(o->ch[2].firstOfEmitter);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.01f, o->ch[0].area);  // 100 mm² = 0.01 dm²
    TEST_ASSERT_EQUAL(100, o->ch[3].pos[0]);                // 10 mm in 0.1 mm
    TEST_ASSERT_EQUAL(50, o->ch[3].pos[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02f, Lamp::familyArea(F_RGB));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, Lamp::zMin());
    TEST_ASSERT_EQUAL_FLOAT(5.0f, Lamp::zMax());
}

static void test_lamp_ceilings_follow_the_weakest_module() {
    loadLamp(kTwoModules);
    // All channels at full on the weak module: 1 + 2 + 0.5 lm on 0.01 dm².
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, Lamp::ceiling(F_RGB));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, Lamp::ceiling(F_WHITE));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, Lamp::colourCeiling(1.0f, 0.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, Lamp::colourCeiling(0.0f, 0.0f, 1.0f));
}

static void test_lamp_file_problems_are_rejected() {
    struct {
        const char* json;
        const char* err;
    } cases[] = {
        {R"({"format": "other", "version": 1})", "not a lightwitch-lamp v1 file"},
        {R"({"format": "lightwitch-lamp", "version": 1, "types": {}, "outputs": []})",
         "types or outputs missing"},
        {R"({"format": "lightwitch-lamp", "version": 1, "types": {"t": {}},
             "outputs": [{"light": 3, "modules": []}]})",
         "light does not exist"},
        {R"({"format": "lightwitch-lamp", "version": 1, "types": {"t": {}},
             "outputs": [{"light": 0, "modules": []}, {"light": 0, "modules": []}]})",
         "two outputs on one light"},
        {R"({"format": "lightwitch-lamp", "version": 1, "types": {"t": {}},
             "outputs": [{"light": 0, "modules": [{"t": "x"}]}]})",
         "unknown module type"},
        {R"({"format": "lightwitch-lamp", "version": 1,
             "types": {"t": {"area": 10, "pixels": [{"ch": [{"k": "uv"}, {}, {}]}]}},
             "outputs": [{"light": 0, "modules": [
               {"t": "t", "p": [0, 0, 0], "n": [0, 0, 1], "u": [0, 1, 0]}]}]})",
         "unknown channel kind"},
        {R"({"format": "lightwitch-lamp", "version": 1,
             "types": {"t": {"area": 10, "pixels": [{"ch": [{"k": "r", "at": [0, 0]}]}]}},
             "outputs": [{"light": 0, "modules": [
               {"t": "t", "p": [0, 0, 0], "n": [0, 0, 1], "u": [0, 1, 0]}]}]})",
         "pixel needs 3 channels"},
        {R"({"format": "lightwitch-lamp", "version": 1,
             "types": {"t": {"area": 10, "pixels": []}},
             "outputs": [{"light": 0, "modules": [
               {"t": "t", "p": [0, 0, 0], "n": [0, 0, 1], "u": [0, 0, 2]}]}]})",
         "module u parallel to n"},
    };
    Config::get() = DeviceConfig();
    Config::get().lights[0].exists = true;
    for (auto& c : cases) {
        JsonDocument doc;
        TEST_ASSERT_FALSE(deserializeJson(doc, c.json));
        Output outs[MAX_LIGHTS];
        uint8_t count;
        char name[32];
        const char* err;
        TEST_ASSERT_FALSE(Lamp::parse(doc, outs, count, name, sizeof(name), err));
        TEST_ASSERT_EQUAL_STRING(c.err, err);
    }
}

static void test_missing_or_broken_lamp_file_leaves_lamp_unloaded() {
    loadLamp(kTwoModules);
    TEST_ASSERT_TRUE(Lamp::loaded());
    LittleFS.clear();
    Lamp::load();
    TEST_ASSERT_FALSE(Lamp::loaded());
    TEST_ASSERT_NULL(Lamp::outputForLight(0));
    loadLamp("{ not json");
    TEST_ASSERT_FALSE(Lamp::loaded());
}

// ── Emitter duties ───────────────────────────────────────────────────────────

static void test_duty_is_linear_in_area_brightness() {
    loadLamp(kTwoModules);
    uint8_t out[3];
    duty(0, 255, 0, 0, 50.0f, out);  // half the weak red ceiling
    TEST_ASSERT_EQUAL(128, out[0]);
    TEST_ASSERT_EQUAL(0, out[1]);
    TEST_ASSERT_EQUAL(0, out[2]);
    duty(0, 128, 0, 0, 50.0f, out);  // colour value scales too
    TEST_ASSERT_EQUAL(64, out[0]);
    duty(1, 255, 0, 0, 50.0f, out);  // strong module needs a quarter
    TEST_ASSERT_EQUAL(32, out[0]);
}

static void test_duty_off_for_black_or_zero_level() {
    loadLamp(kTwoModules);
    uint8_t out[3] = {9, 9, 9};
    duty(0, 0, 0, 0, 50.0f, out);
    TEST_ASSERT_EQUAL(0, out[0]);
    TEST_ASSERT_EQUAL(0, out[1]);
    TEST_ASSERT_EQUAL(0, out[2]);
    duty(0, 255, 255, 255, 0.0f, out);
    TEST_ASSERT_EQUAL(0, out[0]);
}

static void test_uniform_mode_holds_strong_modules_at_the_ceiling() {
    loadLamp(kTwoModules, Mode::Uniform);
    uint8_t weak[3], strong[3];
    duty(0, 255, 0, 0, 200.0f, weak);
    duty(1, 255, 0, 0, 200.0f, strong);
    TEST_ASSERT_EQUAL(255, weak[0]);
    TEST_ASSERT_EQUAL(64, strong[0]);  // held to 100 lm/dm²
}

static void test_peaks_mode_lets_strong_modules_rise() {
    loadLamp(kTwoModules, Mode::WithPeaks);
    uint8_t weak[3], strong[3];
    duty(0, 255, 0, 0, 200.0f, weak);
    duty(1, 255, 0, 0, 200.0f, strong);
    TEST_ASSERT_EQUAL(255, weak[0]);  // clips at its own maximum
    TEST_ASSERT_EQUAL(128, strong[0]);
}

static void test_clipping_keeps_the_hue() {
    loadLamp(kTwoModules, Mode::WithPeaks);
    uint8_t out[3];
    duty(0, 255, 128, 0, 2000.0f, out);
    TEST_ASSERT_EQUAL(255, out[0]);
    TEST_ASSERT_INT_WITHIN(1, 128, out[1]);
    TEST_ASSERT_EQUAL(0, out[2]);
}

// ── LampDriver ───────────────────────────────────────────────────────────────

static void test_lamp_driver_raw_mode_passes_colours() {
    loadLamp(kTwoModules, Mode::Raw);
    RecordingDriver inner;
    LampDriver d;
    d.setup(&inner, Lamp::outputForLight(0));
    TEST_ASSERT_FALSE(d.physical());
    d.setColor(10, 20, 30);
    TEST_ASSERT_EQUAL(10, inner.px[1].r);
    TEST_ASSERT_EQUAL(20, inner.px[1].g);
    TEST_ASSERT_EQUAL(30, inner.px[1].b);
    TEST_ASSERT_EQUAL(1, inner.shows);
}

static void test_lamp_driver_applies_level_and_clamp() {
    loadLamp(kTwoModules, Mode::Uniform);
    RecordingDriver inner;
    LampDriver d;
    d.setup(&inner, Lamp::outputForLight(0));
    TEST_ASSERT_TRUE(d.physical());
    d.setLevel(128);
    d.setPixel(0, 255, 0, 0);
    uint8_t want[3];
    duty(0, 255, 0, 0, levelToDensity(128), want);
    TEST_ASSERT_EQUAL(want[0], inner.px[0].r);
    TEST_ASSERT_TRUE(want[0] > 0 && want[0] < 255);

    // The clamp applies to the colour before the lamp model.
    d.setBrightnessClamp(128, 255);
    d.setPixel(0, 255, 0, 0);
    duty(0, 128, 0, 0, levelToDensity(128), want);
    TEST_ASSERT_EQUAL(want[0], inner.px[0].r);

    d.setLevel(0);
    d.setPixel(0, 255, 0, 0);
    TEST_ASSERT_EQUAL(0, inner.px[0].r);
}

// ── Game LED levels ──────────────────────────────────────────────────────────

static void test_pulse_runs_between_dimmed_and_bright() {
    using namespace game;
    TEST_ASSERT_EQUAL(kLevelDimmed, pulseLevel(0));
    TEST_ASSERT_EQUAL(kLevelBright, pulseLevel(kPulsePeriodMs / 2));
    TEST_ASSERT_EQUAL((kLevelDimmed + kLevelBright) / 2, pulseLevel(kPulsePeriodMs / 4));
    TEST_ASSERT_EQUAL(kLevelDimmed, pulseLevel(kPulsePeriodMs));
    for (uint32_t t = 0; t < 2 * kPulsePeriodMs; t += 7) {
        uint8_t l = pulseLevel(t);
        TEST_ASSERT_TRUE(l >= kLevelDimmed && l <= kLevelBright);
    }
}

static void test_scale_rgb_levels() {
    using namespace game;
    Rgb c = scaleRgb(colorRgb(game::Color::Orange), kLevelDimmed);
    TEST_ASSERT_EQUAL(51, c.r);
    TEST_ASSERT_EQUAL(14, c.g);
    TEST_ASSERT_EQUAL(0, c.b);
    c = scaleRgb(colorRgb(game::Color::White), kLevelFaint);
    TEST_ASSERT_EQUAL(13, c.r);
    c = scaleRgb(colorRgb(game::Color::White), kLevelBright);
    TEST_ASSERT_EQUAL(255, c.b);
    c = scaleRgb(colorRgb(game::Color::Off), kLevelBright);
    TEST_ASSERT_EQUAL(0, c.r + c.g + c.b);
}

static void test_blink_is_half_on() {
    using namespace game;
    TEST_ASSERT_TRUE(blinkOn(0));
    TEST_ASSERT_TRUE(blinkOn(kBlinkPeriodMs / 2 - 1));
    TEST_ASSERT_FALSE(blinkOn(kBlinkPeriodMs / 2));
    TEST_ASSERT_TRUE(blinkOn(kBlinkPeriodMs));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_clamp_defaults_to_pass_through);
    RUN_TEST(test_clamp_scale_damps_proportionally);
    RUN_TEST(test_clamp_limit_caps_after_scale);
    RUN_TEST(test_clamp_zero_blanks);
    RUN_TEST(test_level_scale_end_points);
    RUN_TEST(test_level_scale_is_logarithmic_and_monotonic);
    RUN_TEST(test_family_weights);
    RUN_TEST(test_render_family_choice);
    RUN_TEST(test_lamp_loads_outputs_and_emitters);
    RUN_TEST(test_lamp_ceilings_follow_the_weakest_module);
    RUN_TEST(test_lamp_file_problems_are_rejected);
    RUN_TEST(test_missing_or_broken_lamp_file_leaves_lamp_unloaded);
    RUN_TEST(test_duty_is_linear_in_area_brightness);
    RUN_TEST(test_duty_off_for_black_or_zero_level);
    RUN_TEST(test_uniform_mode_holds_strong_modules_at_the_ceiling);
    RUN_TEST(test_peaks_mode_lets_strong_modules_rise);
    RUN_TEST(test_clipping_keeps_the_hue);
    RUN_TEST(test_lamp_driver_raw_mode_passes_colours);
    RUN_TEST(test_lamp_driver_applies_level_and_clamp);
    RUN_TEST(test_pulse_runs_between_dimmed_and_bright);
    RUN_TEST(test_scale_rgb_levels);
    RUN_TEST(test_blink_is_half_on);
    return UNITY_END();
}
