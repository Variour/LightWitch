#include "Tca9555Expander.h"

#include <Arduino.h>

#include "../logging/Logger.h"

void Tca9555Expander::beginOutput(uint8_t pin) {
    if (pin > 15) return;
    _expander = std::make_unique<TCA9555>(_i2cAddress);
    if (!_expander->isConnected())
        Logger::w("[io] TCA9555 not responding at address 0x%02X", _i2cAddress);
    if (!_expander->pinMode1(pin, OUTPUT)) {
        Logger::w("[io] TCA9555 pinMode1(pin=%u) failed at address 0x%02X (error %d)", pin,
                  _i2cAddress, _expander->lastError());
    }
    write(pin, false);
}

void Tca9555Expander::write(uint8_t pin, bool high) {
    if (pin > 15 || !_expander) return;
    if (!_expander->write1(pin, high ? HIGH : LOW)) {
        Logger::w("[io] TCA9555 write1(pin=%u) failed at address 0x%02X (error %d)", pin,
                  _i2cAddress, _expander->lastError());
    }
}
