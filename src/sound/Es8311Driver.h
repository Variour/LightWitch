#pragma once
#include <stdint.h>

#include "../config/Config.h"
#include "../io/Tca9555Expander.h"
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
//
// The speaker-amp enable pin (paEnablePin) may be a native ESP32 GPIO or a
// pin on a TCA9555 I2C expander sharing this codec's bus — see
// IoExpanderChip in Config.h and src/io/Tca9555Expander.h.
class Es8311Driver : public SoundDriver {
   public:
    void setup(const SoundHardwareConfig& cfg) { _cfg = cfg; }

    void begin() override;
    void playTestMelody() override;

    // TEMPORARY diagnostic aid for the silent/noisy-speaker bring-up issue —
    // remove once the correct BCLK/LRCK divider values are confirmed on real
    // hardware. Cycles REG_BCLK_DIV/REG_LRCK_DIV_HI/LO through a handful of
    // candidate values (see .cpp), playing a short steady tone after each so
    // the working combination (if any) can be identified by ear and matched
    // against the "[sound] SWEEP n/N: ..." log line active at that moment.
    void runDiagnosticSweep();

   private:
    bool _writeReg(uint8_t reg, uint8_t value);
    void _resetAndConfigureClocks();
    void _configureFormatAndPower();
    void _setPaEnabled(bool enabled);
    void _writeToneBlock(float freqHz, uint32_t durationMs, float gain);

    SoundHardwareConfig _cfg;
    Tca9555Expander _paExpander;
    bool _i2sInstalled = false;
};
