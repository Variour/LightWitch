#include "BoardProfile.h"

#include "../buttons/ButtonRoles.h"

#ifdef BOARD_BUZZERBOARD

// Buzzerboard rev A0 (ESP32-C5-WROOM-1U-N8R8). Source of every value below:
// the board repo's hardware/gen/buzzer.py (nets) and hardware/README.md
// ("GPIO map (ESP32-C5-WROOM-1U)", "LED rail = PMID", charger table).
namespace {

struct KeyPin {
    const char* role;
    const char* name;
    uint8_t gpio;
};

// Every key switches its net to GND and has an external 10k pull-up to 3V3
// plus 100 nF to GND, so all of them are active low. POWER reaches GPIO4
// through one half of a BAT54C (the other half drives the charger's QON).
constexpr KeyPin kKeys[] = {
    {ButtonRole::BUZZ, "Buzzer", 2},   // KEY_BUZZ, module pin 4 (MTMS)
    {ButtonRole::UP, "Up", 3},         // KEY_UP, module pin 5 (MTDI strap)
    {ButtonRole::DOWN, "Down", 5},     // KEY_DOWN, module pin 16
    {ButtonRole::LEFT, "Left", 1},     // KEY_LEFT, module pin 7
    {ButtonRole::RIGHT, "Right", 12},  // KEY_RIGHT, module pin 24 (RXD0)
    {ButtonRole::ENTER, "Enter", 0},   // KEY_ENTER, module pin 6
    {ButtonRole::L1, "L1", 28},        // KEY_SIDE_L1, module pin 15 (BOOT strap)
    {ButtonRole::L2, "L2", 10},        // KEY_SIDE_L2, module pin 12
    {ButtonRole::R1, "R1", 23},        // KEY_SIDE_R1, module pin 21
    {ButtonRole::R2, "R2", 24},        // KEY_SIDE_R2, module pin 23
    {ButtonRole::POWER, "Power", 4},   // PWR_KEY, module pin 17 (via BAT54C)
};
static_assert(sizeof(kKeys) / sizeof(kKeys[0]) <= MAX_BUTTONS,
              "MAX_BUTTONS must cover every Buzzerboard key");

// I2C bus: BQ25628 (0x6A) and LSM6DS3TR-C (0x6B), 4.7k pull-ups.
constexpr uint8_t kI2cSdaPin = 26;
constexpr uint8_t kI2cSclPin = 27;

// 34 x SK6812MINI-E in one chain behind the SN74LV1T34 5 V buffer, on the
// WS2812B driver.
// UNVERIFIED: GRB channel order (SK6812 convention, no datasheet checked);
// confirm with the light's colour-order test.
constexpr uint8_t kLedDataPin = 8;
constexpr uint16_t kLedCount = 34;

// LED current cap for the whole chain. The LEDs hang on the charger's PMID
// node, which the charger cannot regulate: on a default USB source the input
// is held at the ILIM pin's 446 mA (5.6k, 398-496 mA with KILIM and 1 %
// tolerance), and PMID_GD cuts the LED rail once the ILIM pin passes
// VILIM_OCP (0.9 V = 112.5 % of the 0.8 V regulation point, BQ25628
// SLUSEG4C §8.3.3.4 / §8.3.10.2.7). Worst case 398 mA, minus the chain's
// idle current (~34 x 0.6 mA, board README) and ~75 mA for the C5 when the
// cell cannot supplement the system leaves ~300 mA for the pixels. On
// battery the boost could deliver more (board budget 1.22 A), but one cap
// has to hold on USB too.
// UNVERIFIED: the 0.6 mA idle and ~75 mA system figures are estimates;
// measure IBUS with the LEDs dark and at the cap on a 500 mA USB port.
constexpr uint16_t kLedCurrentLimitMa = 300;

}  // namespace

bool applyBoardProfile(DeviceConfig& cfg) {
    cfg.i2cSdaPin = kI2cSdaPin;
    cfg.i2cSclPin = kI2cSclPin;
    cfg.chargerEnabled = true;
    // The battery is read from the charger's ADC (no BATTERY_ADC_PIN on this
    // board), so turn the existing battery reporting on.
    cfg.batteryMonitoringEnabled = true;

    LightHardwareConfig& l = cfg.lights[0];
    l = LightHardwareConfig{};
    strlcpy(l.name, "Buzzer LEDs", sizeof(l.name));
    l.ledType = LedType::WS2812B;
    l.colorOrder = ColorOrder::GRB;
    l.dataPin = kLedDataPin;
    l.width = kLedCount;
    l.height = 1;
    l.groupId = 0;
    l.currentLimitMa = kLedCurrentLimitMa;
    l.exists = true;

    uint8_t i = 0;
    for (const KeyPin& k : kKeys) {
        ButtonHardwareConfig& b = cfg.buttons[i++];
        b = ButtonHardwareConfig{};
        strlcpy(b.name, k.name, sizeof(b.name));
        strlcpy(b.role, k.role, sizeof(b.role));
        b.pin = k.gpio;
        b.activeLow = true;
        b.viaExpander = false;
        b.exists = true;
    }
    return true;
}

#else

bool applyBoardProfile(DeviceConfig&) { return false; }

#endif
