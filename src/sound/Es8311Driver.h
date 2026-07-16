#pragma once
#include <stdint.h>

#include "../config/Config.h"
#include "SoundDriver.h"

// Own implementation of the ES8311 bring-up sequence, written from the public
// ES8311 register map (Everest Semiconductor datasheet) rather than vendoring
// an existing library — see src/sound/README.md for why. Hardware support
// only: begin() brings the codec + I2S peripheral up, playTestMelody() proves
// the wiring works with a short built-in tune. There is deliberately no
// generic "play arbitrary audio" API yet — that's the later, separate step.
//
// Fixed at a single internal format for now (master mode, 16-bit I2S normal,
// 16 kHz, mono content duplicated to both I2S slots) since that's all the
// test melody needs; a real playback pipeline will need this to become
// configurable.
class Es8311Driver : public SoundDriver {
   public:
    void setup(const SoundHardwareConfig& cfg) { _cfg = cfg; }

    void begin() override;
    void playTestMelody() override;

   private:
    void _writeReg(uint8_t reg, uint8_t value);
    void _resetAndConfigureClocks();
    void _configureFormatAndPower();
    void _setPaEnabled(bool enabled);
    void _writeToneBlock(float freqHz, uint32_t durationMs, float gain);

    SoundHardwareConfig _cfg;
    bool _i2sInstalled = false;
};
