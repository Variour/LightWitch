#pragma once
#include <Arduino.h>
#include <stdint.h>

class SdCardManager;

// Abstract sound output interface. Swap this implementation when the codec
// chip changes (see LedDriver.h for the equivalent on the light side).
//
// playTestMelody() exists purely to let the user verify pin wiring from the
// web UI, the same way the LED side's showColorOrderTest() does.
//
// scheduleFiles()/stop()/setVolume() are the playback pipeline: filenames are
// read from the SD card (see setSdCard()) and streamed to the codec. Playback
// runs on a dedicated task, not the caller's — these calls only enqueue a
// command and return immediately. startDelayMs implements the mesh-wide
// sync-start contract (see PlayAudioMsg in MeshTypes.h): playback begins
// startDelayMs after scheduleFiles() is called, measured against this
// device's own clock, not a shared wall clock. If a file can't be
// opened/parsed in time, that device silently doesn't participate (fail
// closed, not fail late) — the caller isn't notified either way, mirroring
// the "no readiness handshake" mesh design.
class SoundDriver {
   public:
    virtual ~SoundDriver() = default;
    virtual void begin() = 0;
    virtual void playTestMelody() = 0;

    virtual void setSdCard(SdCardManager* sdCard) = 0;

    // Plays `fileCount` SD-card filenames in order, starting startDelayMs from
    // now. loop=true repeats the whole list (or the single file, if
    // fileCount==1) indefinitely until stop() is called.
    virtual void scheduleFiles(const String* filenames, uint8_t fileCount, bool loop,
                               uint32_t startDelayMs) = 0;

    // Stops playback immediately, regardless of what's currently playing (or
    // not) — idempotent.
    virtual void stop() = 0;

    // Live output level, 0 (silent) – 255 (max) — see
    // SoundHardwareConfig::volume. Takes effect immediately, no reboot.
    virtual void setVolume(uint8_t volume) = 0;
};
