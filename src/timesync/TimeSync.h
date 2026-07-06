#pragma once
#include <Arduino.h>
#include <time.h>
#include <functional>

// Wall-clock time for the device. Time comes from NTP when this device has
// working WiFi; otherwise it can be adopted from a mesh peer that has NTP.
// Neither source is a hard requirement: until one succeeds, getLocalTime()
// returns false and callers (e.g. TimeMatrix) should show an error state
// instead of a clock.
class TimeSync {
public:
    enum class Source : uint8_t { None, Ntp, Peer };
    using BroadcastFn = std::function<void(uint32_t epoch)>;

    // timezone: POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3"), applied
    // immediately so localtime() is correct as soon as any clock source sets
    // the system time.
    static void begin(const char* timezone);

    // Called with the current epoch whenever this device has an NTP-synced
    // clock and it's time to share it with the mesh.
    static void setBroadcastFn(BroadcastFn fn) { _broadcastFn = fn; }

    // Poll WiFi/NTP status and (if synced) periodically broadcast to peers.
    static void tick();

    // A peer broadcast a synced epoch — adopt it unless we already have our
    // own NTP-sourced clock (which is trusted over a relayed one).
    static void onPeerTime(uint32_t epoch);

    // Fills `out` with local (timezone-adjusted) time. Returns false if no
    // time source has synced yet.
    static bool getLocalTime(struct tm& out);

private:
    static Source      _source;
    static bool        _ntpStarted;
    static uint32_t    _lastBroadcast;
    static BroadcastFn _broadcastFn;
    static char        _timezone[64];

    // Rejects implausible epochs received from a peer (e.g. a peer that never
    // synced itself and is still at its 1970 boot default). Corresponds to
    // 2023-11-14, comfortably before any real use of this firmware.
    static constexpr time_t EPOCH_SANITY_THRESHOLD = 1700000000;
};
