#include "TimeSync.h"
#include <sys/time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "../logging/Logger.h"

TimeSync::Source      TimeSync::_source        = TimeSync::Source::None;
bool                   TimeSync::_ntpStarted    = false;
uint32_t               TimeSync::_lastBroadcast = 0;
TimeSync::BroadcastFn  TimeSync::_broadcastFn;
char                   TimeSync::_timezone[64]  = "UTC0";

static constexpr uint32_t BROADCAST_INTERVAL_MS = 10000;

void TimeSync::begin(const char* timezone) {
    strlcpy(_timezone, (timezone && timezone[0]) ? timezone : "UTC0", sizeof(_timezone));
    setenv("TZ", _timezone, 1);
    tzset();
}

void TimeSync::tick() {
    bool wifiUp = WiFi.status() == WL_CONNECTED;

    if (wifiUp && !_ntpStarted) {
        _ntpStarted = true;
        // configTzTime (not configTime) so SNTP's own TZ handling applies our
        // configured zone rather than clobbering it back to UTC — configTime's
        // gmtOffset_sec/daylightOffset_sec form always overwrites TZ, which
        // undid the timezone begin() set and made Time mode display UTC.
        configTzTime(_timezone, "pool.ntp.org", "time.nist.gov");
        Logger::i("[time] NTP sync started");
    }
    if (!wifiUp) _ntpStarted = false;  // retry NTP config on reconnect

    // Checked against the SNTP library's own completion status rather than
    // "is the system clock plausible", since a peer-adopted clock (settimeofday
    // in onPeerTime) already makes time(nullptr) look plausible before our own
    // NTP request has actually completed.
    if (wifiUp && _source != Source::Ntp && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        _source = Source::Ntp;
        Logger::i("[time] NTP synced");
    }

    if (_source == Source::Ntp && _broadcastFn && millis() - _lastBroadcast >= BROADCAST_INTERVAL_MS) {
        _lastBroadcast = millis();
        _broadcastFn((uint32_t)time(nullptr));
    }
}

void TimeSync::onPeerTime(uint32_t epoch) {
    if (_source == Source::Ntp) return;  // trust our own NTP over a relayed clock
    if ((time_t)epoch <= EPOCH_SANITY_THRESHOLD) return;
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    if (_source != Source::Peer) Logger::i("[time] adopted time from peer");
    _source = Source::Peer;
}

bool TimeSync::getLocalTime(struct tm& out) {
    out = {};
    if (_source == Source::None) return false;
    time_t t = time(nullptr);
    localtime_r(&t, &out);
    return true;
}
