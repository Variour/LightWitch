#include "Tca9555Expander.h"

#include "../logging/Logger.h"
#include "I2cBus.h"

namespace {

// Register of pins 0-7 of each pair; pins 8-15 are in the next one.
constexpr uint8_t REG_INPUT_PORT = 0x00;
constexpr uint8_t REG_OUTPUT_PORT = 0x02;
constexpr uint8_t REG_CONFIGURATION = 0x06;  // bit set: input

}  // namespace

// Sets or clears the bit of `pin` in the register pair starting at `reg`
// and writes the register back if that changed it.
esp_err_t Tca9555Expander::_updateBit(uint8_t reg, uint8_t pin, bool set) {
    reg += pin >> 3;
    uint8_t mask = 1 << (pin & 7);
    uint8_t value;
    esp_err_t err = i2cBus.read(_i2cAddress, reg, &value, 1);
    if (err != ESP_OK) return err;
    uint8_t updated = set ? (value | mask) : (value & ~mask);
    if (updated == value) return ESP_OK;
    return i2cBus.write(_i2cAddress, reg, &updated, 1);
}

void Tca9555Expander::_begin(uint8_t pin, bool input) {
    _begun = true;
    if (i2cBus.probe(_i2cAddress) != ESP_OK)
        Logger::w("[io] TCA9555 not responding at address 0x%02X", _i2cAddress);
    esp_err_t err = _updateBit(REG_CONFIGURATION, pin, input);
    if (err != ESP_OK) {
        Logger::w("[io] TCA9555 pin mode (pin=%u) failed at address 0x%02X (%s)", pin, _i2cAddress,
                  esp_err_to_name(err));
    }
}

void Tca9555Expander::beginOutput(uint8_t pin) {
    if (pin > 15) return;
    _begin(pin, false);
    write(pin, false);
}

void Tca9555Expander::write(uint8_t pin, bool high) {
    if (pin > 15 || !_begun) return;
    esp_err_t err = _updateBit(REG_OUTPUT_PORT, pin, high);
    if (err != ESP_OK) {
        Logger::w("[io] TCA9555 write (pin=%u) failed at address 0x%02X (%s)", pin, _i2cAddress,
                  esp_err_to_name(err));
    }
}

void Tca9555Expander::beginInput(uint8_t pin) {
    if (pin > 15) return;
    _begin(pin, true);
}

bool Tca9555Expander::read(uint8_t pin) {
    if (pin > 15 || !_begun) return false;
    uint8_t value;
    if (i2cBus.read(_i2cAddress, REG_INPUT_PORT + (pin >> 3), &value, 1) != ESP_OK) return false;
    return value & (1 << (pin & 7));
}
