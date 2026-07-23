#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdint.h>

#include "../config/Config.h"
#include "../io/Tca9555Expander.h"
#include "../storage/SdCardManager.h"
#include "SoundDriver.h"

// Own implementation of the ES8311 bring-up sequence, written from the public
// ES8311 register map (Everest Semiconductor datasheet) rather than vendoring
// an existing library — see src/sound/README.md for why. begin() brings the
// codec + I2S peripheral up; playTestMelody() and the scheduleFiles()/stop()
// playback pipeline both run on a single dedicated FreeRTOS task (see
// _playerTaskFn) spawned by begin() — every I2C/I2S register access after
// begin() happens from that one task, so a "Test speaker" click and an
// in-flight mesh playback trigger can never race on the shared I2C bus. All
// public playback calls just post a command to _cmdQueue and return.
//
// I2S sample rate is reconfigured per file to match its WAV header (see
// WavInfo) rather than fixed, unlike the original test-melody-only version
// of this driver — bit depth stays fixed at 16-bit (the only depth
// scheduleFiles() accepts) and channels are always driven as stereo, with
// mono sources duplicated to both channels in software.
//
// The speaker-amp enable pin (paEnablePin) may be a native ESP32 GPIO or a
// pin on the device's TCA9555 expander sharing this codec's bus — see
// paViaExpander in Config.h and src/io/Tca9555Expander.h.
//
// The codec's I2C control interface lives on the device-wide I2C bus
// (DeviceConfig::i2cSdaPin/i2cSclPin, not part of SoundHardwareConfig) —
// main.cpp brings that bus up once at startup, so begin() only needs the
// pins to validate/log with, not to call Wire.begin() itself. Likewise
// expanderAddress comes from DeviceConfig::expanderAddress, not this
// config — there's only ever one expander per device.
class Es8311Driver : public SoundDriver {
   public:
    void setup(const SoundHardwareConfig& cfg, uint8_t i2cSdaPin, uint8_t i2cSclPin,
               uint8_t expanderAddress) {
        _cfg = cfg;
        _i2cSdaPin = i2cSdaPin;
        _i2cSclPin = i2cSclPin;
        _expanderAddress = expanderAddress;
    }

    void begin() override;
    void playTestMelody() override;
    void setSdCard(SdCardManager* sdCard) override { _sdCard = sdCard; }
    void scheduleFiles(const String* filenames, uint8_t fileCount, bool loop,
                       uint32_t startDelayMs) override;
    void stop() override;
    void setVolume(uint8_t volume) override;

   private:
    static constexpr uint8_t MAX_PLAY_FILES =
        32;  // mirrors PlaylistManager::MAX_FILES_PER_PLAYLIST

    struct PlayerCommand {
        enum class Type : uint8_t { Play, Stop, SetVolume, TestMelody } type;
        String* files = nullptr;  // heap array, task takes ownership; Play only
        uint8_t fileCount = 0;
        bool loop = false;
        uint32_t startAtMs = 0;  // absolute millis() target; Play only
        uint8_t volume = 0;      // SetVolume only
    };

    struct WavInfo {
        uint16_t numChannels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        uint32_t dataSize = 0;  // bytes remaining to read from the current file position
    };

    bool _writeReg(uint8_t reg, uint8_t value);
    void _resetAndConfigureClocks();
    void _configureFormatAndPower();
    void _setPaEnabled(bool enabled);
    void _writeToneBlock(float freqHz, uint32_t durationMs, float gain);
    void _writeSilenceFrames(uint32_t frames);

    static void _playerTaskFn(void* arg);
    void _playerLoop();
    void _runTestMelody();
    // Runs (and owns cmd.files' lifetime for) one playback command, including
    // waiting out its scheduled start and any Play/Stop/TestMelody that
    // preempts it along the way.
    void _runPlayback(PlayerCommand cmd);
    // Returns false (and logs) if `name` can't be opened or isn't a supported
    // WAV (must be PCM, 16-bit mono/stereo) — caller treats that as "skip
    // this file", per the mesh design's "missing file = don't participate".
    bool _openWav(const String& name, File& outFile, WavInfo& outInfo);
    // Starting at *index (inclusive), opens the next playable file, advancing
    // past unplayable ones — wrapping to 0 only if cmd.loop. Returns false if
    // nothing playable is found (a full pass for loop, or the remaining tail
    // for non-loop).
    bool _openNextPlayable(const PlayerCommand& cmd, uint8_t& index, File& outFile,
                           WavInfo& outInfo);
    void _applyI2sFormat(const WavInfo& info);
    // Streams one file to completion. Returns true if a Stop/Play/TestMelody
    // command preempted it mid-stream (filled into outInterrupt); a
    // SetVolume seen along the way is applied internally and never reported
    // as an interrupt.
    bool _streamFile(File& f, const WavInfo& info, PlayerCommand& outInterrupt);
    // Non-blocking (or bounded-wait) peek at the command queue. Applies
    // SetVolume directly and keeps draining; returns true (with `out` filled)
    // on the first Stop/Play/TestMelody found.
    bool _checkForInterrupt(PlayerCommand& out, TickType_t wait);
    void _muteAndDisablePa();

    SoundHardwareConfig _cfg;
    uint8_t _i2cSdaPin = PIN_UNUSED;
    uint8_t _i2cSclPin = PIN_UNUSED;
    uint8_t _expanderAddress = 0x20;
    Tca9555Expander _paExpander;
    SdCardManager* _sdCard = nullptr;
    bool _i2sInstalled = false;

    QueueHandle_t _cmdQueue = nullptr;
    TaskHandle_t _playTask = nullptr;
    uint32_t _lastI2sSampleRate = 0;
    uint16_t _lastI2sBits = 0;
};
