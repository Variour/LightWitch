#include "Tca9555Expander.h"

#include <Wire.h>

// TCA9555 register addresses (NXP/TI datasheet). Port 0 covers pins 0-7,
// port 1 covers pins 8-15; the port 1 register is always one address past
// its port 0 counterpart.
static constexpr uint8_t REG_OUTPUT_PORT0 = 0x02;
static constexpr uint8_t REG_CONFIG_PORT0 = 0x06;  // bit=1 -> input, bit=0 -> output

uint8_t Tca9555Expander::_readReg(uint8_t reg) {
    Wire.beginTransmission(_i2cAddress);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)_i2cAddress, 1);
    return Wire.available() ? Wire.read() : 0;
}

void Tca9555Expander::_writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(_i2cAddress);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

void Tca9555Expander::beginOutput(uint8_t pin) {
    if (pin > 15) return;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    uint8_t configReg = REG_CONFIG_PORT0 + port;
    uint8_t outputReg = REG_OUTPUT_PORT0 + port;

    uint8_t config = _readReg(configReg);
    _writeReg(configReg, config & ~(1 << bit));  // clear bit = output

    uint8_t output = _readReg(outputReg);
    _writeReg(outputReg, output & ~(1 << bit));  // init low
}

void Tca9555Expander::write(uint8_t pin, bool high) {
    if (pin > 15) return;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    uint8_t outputReg = REG_OUTPUT_PORT0 + port;

    uint8_t output = _readReg(outputReg);
    if (high)
        output |= (1 << bit);
    else
        output &= ~(1 << bit);
    _writeReg(outputReg, output);
}
