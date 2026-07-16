#pragma once
#include <stdint.h>

// Minimal driver for the TCA9555 16-bit I2C GPIO expander (NXP/TI, industry
// standard, public register map — an original implementation, no external
// library, mirrors this project's other small in-repo drivers). Only drives
// a single output pin for now (see Es8311Driver's PA-enable use), but is
// deliberately independent of the sound module so other expander-backed
// signals can reuse it later without any sound-specific baggage.
//
// Read-modify-write on both the direction and output registers: boards
// commonly multiplex many unrelated signals across the same expander's 16
// pins, so a blind full-byte write would clobber whatever else is wired to
// the other bits in that pin's port.
class Tca9555Expander {
   public:
    void setup(uint8_t i2cAddress) { _i2cAddress = i2cAddress; }

    // Configures `pin` (0-15) as an output, initialized low. Assumes the I2C
    // bus (Wire) is already begun by the caller — this expander is meant to
    // share a bus with another chip (e.g. an audio codec), not own it.
    void beginOutput(uint8_t pin);

    // Drives `pin` (0-15) high or low.
    void write(uint8_t pin, bool high);

   private:
    uint8_t _readReg(uint8_t reg);
    void _writeReg(uint8_t reg, uint8_t value);

    uint8_t _i2cAddress = 0x20;
};
