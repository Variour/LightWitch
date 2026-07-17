#pragma once
#include <TCA9555.h>
#include <stdint.h>

#include <memory>

// Thin wrapper around RobTillaart's TCA9555 library (MIT-licensed — see
// platformio.ini's lib_deps and src/io/README.md) around the TCA9555 16-bit
// I2C GPIO expander. Only drives a single output pin for now (see
// Es8311Driver's PA-enable use), but is deliberately independent of the
// sound module so other expander-backed signals can reuse it later.
class Tca9555Expander {
   public:
    void setup(uint8_t i2cAddress) { _i2cAddress = i2cAddress; }

    // Configures `pin` (0-15) as an output, initialized low. Assumes the I2C
    // bus (Wire) is already begun by the caller — this expander is meant to
    // share a bus with another chip (e.g. an audio codec), not own it.
    //
    // Deliberately never calls the library's own begin() — it reconfigures
    // all 16 pins in one shot, which would clobber whatever else is wired to
    // the other bits on boards that multiplex several unrelated peripherals
    // across the same expander. pinMode1()/write1() do a proper per-pin
    // read-modify-write instead.
    void beginOutput(uint8_t pin);

    // Drives `pin` (0-15) high or low.
    void write(uint8_t pin, bool high);

   private:
    uint8_t _i2cAddress = 0x20;
    std::unique_ptr<TCA9555> _expander;
};
