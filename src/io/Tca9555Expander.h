#pragma once
#include <esp_err.h>
#include <stdint.h>

// One pin of a TCA9555 16-bit I2C GPIO expander on the device I2C bus
// (src/io/I2cBus.h), by the registers of the TI datasheet SCPS200E ("Control
// Register and Command Byte"): input port 0x00/0x01, output port 0x02/0x03,
// configuration 0x06/0x07 (the first register of each pair holds pins 0-7,
// the second pins 8-15). The polarity inversion registers 0x04/0x05 stay at
// their power-on value.
// Independent of the sound module so other expander-backed signals can
// reuse it (see Es8311Driver's PA-enable use and ButtonManager).
class Tca9555Expander {
   public:
    void setup(uint8_t i2cAddress) { _i2cAddress = i2cAddress; }

    // Configures `pin` (0-15) as an output, initialized low. Assumes the
    // device I2C bus is already begun by the caller — this expander is meant
    // to share a bus with another chip (e.g. an audio codec), not own it.
    //
    // Changes this pin's bit alone, by a read-modify-write of its
    // configuration and output port registers: boards multiplex several
    // unrelated peripherals across the same expander, and every other pin
    // stays as it is.
    void beginOutput(uint8_t pin);

    // Drives `pin` (0-15) high or low.
    void write(uint8_t pin, bool high);

    // Configures `pin` (0-15) as an input, the same per-pin way as
    // beginOutput(). The TCA9555 has no internal pull resistors, unlike a
    // native ESP32 GPIO's INPUT_PULLUP/INPUT_PULLDOWN — a board wiring a
    // button through this pin must supply its own pull-up/pull-down.
    void beginInput(uint8_t pin);

    // Reads `pin` (0-15). Returns false (as if LOW) for an out-of-range pin,
    // before beginInput()/beginOutput() has run, or when the expander does
    // not answer.
    bool read(uint8_t pin);

   private:
    uint8_t _i2cAddress = 0x20;
    bool _begun = false;

    void _begin(uint8_t pin, bool input);
    esp_err_t _updateBit(uint8_t reg, uint8_t pin, bool set);
};
