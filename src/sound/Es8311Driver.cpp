#include "Es8311Driver.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_task_wdt.h>
#include <math.h>

#include "../logging/Logger.h"

// ES8311 register addresses (Everest Semiconductor datasheet register map).
static constexpr uint8_t REG_RESET = 0x00;         // soft reset / power-state control
static constexpr uint8_t REG_CLK_MANAGER1 = 0x01;  // MCLK source select, clock manager enable
static constexpr uint8_t REG_CLK_MANAGER2 = 0x02;  // MCLK pre-divider / pre-multiplier
static constexpr uint8_t REG_ADC_OSR = 0x03;
static constexpr uint8_t REG_DAC_OSR = 0x04;
static constexpr uint8_t REG_ADC_DAC_DIV = 0x05;
static constexpr uint8_t REG_BCLK_DIV = 0x06;
static constexpr uint8_t REG_LRCK_DIV_HI = 0x07;
static constexpr uint8_t REG_LRCK_DIV_LO = 0x08;
static constexpr uint8_t REG_DAC_FORMAT = 0x09;  // "SDPIN" — serial format for audio into the DAC
static constexpr uint8_t REG_SYSTEM_0B = 0x0B;
static constexpr uint8_t REG_SYSTEM_0C = 0x0C;
static constexpr uint8_t REG_SYSTEM_0D = 0x0D;
static constexpr uint8_t REG_SYSTEM_0E = 0x0E;
static constexpr uint8_t REG_SYSTEM_10 = 0x10;
static constexpr uint8_t REG_SYSTEM_11 = 0x11;
static constexpr uint8_t REG_SYSTEM_12 = 0x12;  // DAC path power/mute
static constexpr uint8_t REG_SYSTEM_13 = 0x13;
static constexpr uint8_t REG_SYSTEM_14 = 0x14;  // analog reference bias
static constexpr uint8_t REG_SYSTEM_15 = 0x15;
static constexpr uint8_t REG_ADC_16 = 0x16;
static constexpr uint8_t REG_ADC_17 = 0x17;
static constexpr uint8_t REG_ADC_FORMAT = 0x0A;  // "SDPOUT" — ADC's own serial format register
static constexpr uint8_t REG_ADC_1B = 0x1B;
static constexpr uint8_t REG_ADC_1C = 0x1C;
static constexpr uint8_t REG_DAC_MUTE = 0x31;
static constexpr uint8_t REG_DAC_VOLUME = 0x32;  // 0x00 = mute, 0xFF = max
static constexpr uint8_t REG_DAC_ANALOG = 0x37;
static constexpr uint8_t REG_GPIO_45 = 0x45;

static constexpr int I2S_SAMPLE_RATE_HZ = 16000;
static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

// Conservative default output level for the test melody — loud enough to be
// audible without assuming anything about the attached speaker's sensitivity.
static constexpr uint8_t DAC_VOLUME_TEST = 0xB0;

bool Es8311Driver::_writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(_cfg.i2cAddress);
    Wire.write(reg);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Logger::w("[sound] ES8311 write reg 0x%02X failed (I2C error %u, addr 0x%02X)", reg, err,
                  _cfg.i2cAddress);
    }
    return err == 0;
}

void Es8311Driver::_setPaEnabled(bool enabled) {
    if (_cfg.paEnablePin == SOUND_PIN_UNUSED) return;
    bool driveHigh = enabled == _cfg.paEnableActiveHigh;
    Logger::d("[sound] PA enable pin -> %s", driveHigh ? "HIGH" : "LOW");
    if (_cfg.paExpander == IoExpanderChip::TCA9555) {
        _paExpander.write(_cfg.paEnablePin, driveHigh);
    } else {
        digitalWrite(_cfg.paEnablePin, driveHigh ? HIGH : LOW);
    }
}

// Puts the codec into reset, then brings up the clock manager for
// I2S_SAMPLE_RATE_HZ/16-bit. The ESP32 I2S peripheral is the I2S clock master
// (see begin()'s I2S_MODE_MASTER); the codec stays I2S slave. When
// i2sMclkPin is unset, ES8311 derives its internal MCLK from SCLK (BCLK) via
// its own PLL instead of an external MCLK line — REG_CLK_MANAGER1's top bit
// selects that source.
//
// This whole sequence (register order and values) was directly captured via
// I2C bus logging from pschatzmann's arduino-audio-driver ES8311 codec class
// running on the exact same board/wiring and confirmed to produce clean
// audio — see src/sound/README.md's accuracy note. Three values here
// (REG_RESET's final 0x80, REG_CLK_MANAGER1's polarity, REG_BCLK_DIV) were
// previously "fixed" based on web-sourced register documentation that turned
// out to be wrong; this capture is ground truth from working hardware, not
// another citation, and takes precedence over those earlier fixes.
void Es8311Driver::_resetAndConfigureClocks() {
    _writeReg(REG_RESET, 0x80);  // full reset pulse
    delay(5);
    _writeReg(REG_RESET, 0x1F);  // release reset, keep analog blocks powered down until configured
    delay(5);

    bool mclkFromSclk = _cfg.i2sMclkPin == SOUND_PIN_UNUSED;
    _writeReg(REG_CLK_MANAGER1, mclkFromSclk ? 0xBF : 0x3F);
    _writeReg(REG_CLK_MANAGER2, 0x00);  // pre-divider=1, pre-multiplier=1
    _writeReg(REG_ADC_OSR, 0x10);
    _writeReg(REG_ADC_16, 0x24);
    _writeReg(REG_DAC_OSR, 0x10);
    _writeReg(REG_ADC_DAC_DIV, 0x00);
    _writeReg(REG_BCLK_DIV, 0x03);
    _writeReg(REG_LRCK_DIV_HI, 0x00);
    _writeReg(REG_LRCK_DIV_LO, 0xFF);

    _writeReg(REG_SYSTEM_0B, 0x00);
    _writeReg(REG_SYSTEM_0C, 0x00);
    _writeReg(REG_SYSTEM_10, 0x1F);
    _writeReg(REG_SYSTEM_11, 0x7F);

    // Final reset-register value stays 0x80 (not 0x00, as previously assumed
    // — bit7 apparently isn't "held in reset" here) — matches the captured
    // working sequence exactly.
    _writeReg(REG_RESET, 0x80);
}

void Es8311Driver::_configureFormatAndPower() {
    _writeReg(REG_SYSTEM_13, 0x10);
    _writeReg(REG_ADC_1B, 0x0A);
    _writeReg(REG_ADC_1C, 0x6A);

    _writeReg(REG_ADC_17, 0xBF);
    _writeReg(REG_SYSTEM_0E, 0x02);
    _writeReg(REG_SYSTEM_12, 0x00);  // DAC path powered on
    _writeReg(REG_SYSTEM_14, 0x1A);
    _writeReg(REG_SYSTEM_0D, 0x01);
    _writeReg(REG_SYSTEM_15, 0x40);
    _writeReg(REG_DAC_ANALOG, 0x48);
    _writeReg(REG_GPIO_45, 0x00);

    _writeReg(REG_DAC_FORMAT, 0x0C);   // 16-bit, I2S normal format
    _writeReg(REG_ADC_FORMAT, 0x4C);   // ADC unused, but the captured working sequence sets this too

    _writeReg(REG_DAC_MUTE, 0x00);    // unmute
    _writeReg(REG_DAC_VOLUME, 0x00);  // start silent; playTestMelody() raises this
}

void Es8311Driver::begin() {
    if (_cfg.i2cSdaPin == SOUND_PIN_UNUSED || _cfg.i2cSclPin == SOUND_PIN_UNUSED ||
        _cfg.i2sBclkPin == SOUND_PIN_UNUSED || _cfg.i2sWsPin == SOUND_PIN_UNUSED ||
        _cfg.i2sDoutPin == SOUND_PIN_UNUSED) {
        Logger::w("[sound] ES8311 pins incomplete — not initializing");
        return;
    }

    Wire.begin(_cfg.i2cSdaPin, _cfg.i2cSclPin);

    if (_cfg.paEnablePin != SOUND_PIN_UNUSED) {
        if (_cfg.paExpander == IoExpanderChip::TCA9555) {
            _paExpander.setup(_cfg.paExpanderAddress);
            _paExpander.beginOutput(_cfg.paEnablePin);
        } else {
            pinMode(_cfg.paEnablePin, OUTPUT);
        }
        _setPaEnabled(false);
    }

    _resetAndConfigureClocks();
    _configureFormatAndPower();

    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2sConfig.sample_rate = I2S_SAMPLE_RATE_HZ;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = 0;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 256;
    i2sConfig.use_apll = false;

    if (i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr) != ESP_OK) {
        Logger::e("[sound] I2S driver install failed");
        return;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num =
        _cfg.i2sMclkPin == SOUND_PIN_UNUSED ? I2S_PIN_NO_CHANGE : _cfg.i2sMclkPin;
    pinConfig.bck_io_num = _cfg.i2sBclkPin;
    pinConfig.ws_io_num = _cfg.i2sWsPin;
    pinConfig.data_out_num = _cfg.i2sDoutPin;
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_PORT, &pinConfig);

    _i2sInstalled = true;
    Logger::i("[sound] ES8311 initialized: sda=GPIO%d scl=GPIO%d bclk=GPIO%d ws=GPIO%d dout=GPIO%d",
              _cfg.i2cSdaPin, _cfg.i2cSclPin, _cfg.i2sBclkPin, _cfg.i2sWsPin, _cfg.i2sDoutPin);
}

// Writes durationMs of a sine tone at freqHz to both I2S slots (mono content
// duplicated to L+R), with a short linear fade-in/out to avoid clicks.
void Es8311Driver::_writeToneBlock(float freqHz, uint32_t durationMs, float gain) {
    constexpr uint32_t CHUNK_FRAMES = 128;
    int16_t chunk[CHUNK_FRAMES * 2];
    uint32_t totalFrames = I2S_SAMPLE_RATE_HZ * durationMs / 1000;
    uint32_t fadeFrames = min(totalFrames / 8, (uint32_t)(I2S_SAMPLE_RATE_HZ / 100));

    for (uint32_t frame = 0; frame < totalFrames;) {
        uint32_t framesThisChunk = min(CHUNK_FRAMES, totalFrames - frame);
        for (uint32_t i = 0; i < framesThisChunk; i++, frame++) {
            float envelope = 1.0f;
            if (frame < fadeFrames) envelope = (float)frame / (float)fadeFrames;
            if (frame > totalFrames - fadeFrames)
                envelope = (float)(totalFrames - frame) / (float)fadeFrames;
            float sample = sinf(2.0f * (float)M_PI * freqHz * (float)frame / I2S_SAMPLE_RATE_HZ);
            int16_t pcm = (int16_t)(sample * envelope * gain * 32000.0f);
            chunk[i * 2] = pcm;
            chunk[i * 2 + 1] = pcm;
        }
        size_t written = 0;
        i2s_write(I2S_PORT, chunk, framesThisChunk * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    }
}

// Short built-in jingle (ascending major arpeggio + resolving note) purely to
// let the user confirm the wiring/pins are correct from the web UI — no
// content/pattern system involved, see SoundDriver.h.
void Es8311Driver::playTestMelody() {
    if (!_i2sInstalled) {
        Logger::w("[sound] playTestMelody: I2S not installed, skipping");
        return;
    }

    Logger::i("[sound] playTestMelody: start");
    _writeReg(REG_DAC_VOLUME, DAC_VOLUME_TEST);
    _setPaEnabled(true);

    struct Note {
        float freqHz;
        uint32_t durationMs;
    };
    static const Note MELODY[] = {
        {523.25f, 140},   // C5
        {659.25f, 140},   // E5
        {783.99f, 140},   // G5
        {1046.50f, 220},  // C6
        {783.99f, 260},   // G5 (resolving note)
    };
    for (const auto& note : MELODY) _writeToneBlock(note.freqHz, note.durationMs, 0.6f);

    _setPaEnabled(false);
    _writeReg(REG_DAC_VOLUME, 0x00);
    Logger::i("[sound] playTestMelody: done");
}

// TEMPORARY — see Es8311Driver.h. Confirmed on real hardware so far: (1)
// silence and a loud tone sound identical (noise is content-independent);
// (2) REG_BCLK_DIV/REG_LRCK_DIV_HI/LO made no difference across a wide
// sweep (master-mode-only registers, irrelevant to this slave-mode setup);
// (3) REG_CLK_MANAGER2 (pre-div/pre-multi) DOES audibly change the noise —
// confirming it's live and relevant, though none of its 4 tested
// combinations produced a clean tone. Web-sourced register facts have
// proven unreliable mid-investigation (contradictory re-fetches of the same
// file), so this sweeps empirically instead of chasing more citations:
// REG_ADC_OSR/REG_DAC_OSR (0x03/0x04, oversampling ratio) are the one
// remaining clock-chain register pair never yet varied from their fixed
// 0x10. Sweeps both together across a few candidate OSR values.
void Es8311Driver::runDiagnosticSweep() {
    if (!_i2sInstalled) {
        Logger::w("[sound] SWEEP: I2S not installed, skipping");
        return;
    }

    struct Variant {
        const char* label;
        uint8_t osr;
    };
    static const Variant VARIANTS[] = {
        {"osr=0x10 (16, current)", 0x10},
        {"osr=0x08 (8)", 0x08},
        {"osr=0x20 (32)", 0x20},
        {"osr=0x40 (64)", 0x40},
    };
    constexpr size_t N = sizeof(VARIANTS) / sizeof(VARIANTS[0]);

    Logger::i("[sound] SWEEP: start, %u ADC/DAC OSR variants", (unsigned)N);
    _writeReg(REG_DAC_VOLUME, DAC_VOLUME_TEST);
    _setPaEnabled(true);

    for (size_t i = 0; i < N; i++) {
        esp_task_wdt_reset();
        const Variant& v = VARIANTS[i];
        Logger::i("[sound] SWEEP %u/%u: %s", (unsigned)(i + 1), (unsigned)N, v.label);
        _writeReg(REG_ADC_OSR, v.osr);
        _writeReg(REG_DAC_OSR, v.osr);
        _writeToneBlock(440.0f, 700, 0.6f);
        delay(300);
    }

    _setPaEnabled(false);
    _writeReg(REG_DAC_VOLUME, 0x00);
    _writeReg(REG_ADC_OSR, 0x10);  // restore current best-derived values
    _writeReg(REG_DAC_OSR, 0x10);
    Logger::i("[sound] SWEEP: done");
}
