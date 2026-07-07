#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "PeerRegistry.h"

// Non-blocking, tick-based replacement for the STA connect loop in
// main.cpp's setupWifi(), so a (possibly long) connection attempt never
// stalls loop() — needed because, unlike at boot, WifiElection drives this
// while the mesh, patterns and web UI are all live.
//
// Mirrors the original retry shape: try each configured network (last-known-
// good first), 3 attempts of up to 10s each, with a short settle delay
// between attempts.
class WifiConnectAttempt {
public:
    using DoneCb = std::function<void(bool success)>;

    bool active() const { return _active; }

    // No-op if an attempt is already in flight — callers race to "claim" one.
    void start(DoneCb onDone) {
        if (_active) return;
        _onDone = onDone;
        _count  = Config::wifiCount();
        if (_count == 0) { _active = false; if (_onDone) _onDone(false); return; }
        _buildOrder();
        _netIdx     = 0;
        _attemptNum = 0;
        _active     = true;
        WiFi.disconnect(false);
        _beginPreDelay(100);
    }

    // Cancels an in-flight attempt without invoking the DoneCb — used when
    // the caller already knows why (e.g. a lower-MAC peer beat us to it) and
    // doesn't want the normal completion handling to also run.
    void abort() {
        if (!_active) return;
        _active = false;
        _onDone = nullptr;
        WiFi.disconnect(false);
        auto& c = Config::get();
        WiFi.softAP(c.deviceName, c.apPassword, 1);
        WiFi.setTxPower(WIFI_TX_POWER);
    }

    void tick() {
        if (!_active) return;

        if (_phase == Phase::PreDelay) {
            if (millis() - _phaseStart < _preDelayMs) return;
            WiFi.begin(_ssid(), _pass());
            WiFi.setTxPower(WIFI_TX_POWER);
            _phase      = Phase::Connecting;
            _phaseStart = millis();
            return;
        }

        // Phase::Connecting
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.softAPdisconnect(false);
            uint8_t ni = _tryOrder[_netIdx];
            if (ni != Config::wifiLast()) {
                Config::setWifiLast(ni);
                Config::saveWifi();
            }
            _finish(true);
            return;
        }
        if (millis() - _phaseStart < 10000) return;

        _attemptNum++;
        if (_attemptNum < 3) { WiFi.disconnect(false); _beginPreDelay(2000); return; }

        _attemptNum = 0;
        _netIdx++;
        if (_netIdx >= _count) { _finish(false); return; }
        WiFi.disconnect(false);
        _beginPreDelay(100);
    }

private:
    enum class Phase { PreDelay, Connecting };

    bool     _active     = false;
    Phase    _phase      = Phase::PreDelay;
    uint32_t _phaseStart = 0;
    uint32_t _preDelayMs = 0;
    uint8_t  _tryOrder[MAX_WIFI_NETWORKS] = {};
    uint8_t  _count      = 0;
    uint8_t  _netIdx     = 0;
    uint8_t  _attemptNum = 0;
    DoneCb   _onDone;

    const char* _ssid() const { return Config::wifiNetworks()[_tryOrder[_netIdx]].ssid; }
    const char* _pass() const { return Config::wifiNetworks()[_tryOrder[_netIdx]].password; }

    void _buildOrder() {
        uint8_t last = Config::wifiLast();
        uint8_t idx  = 0;
        _tryOrder[idx++] = last;
        for (uint8_t i = 0; i < _count; i++)
            if (i != last) _tryOrder[idx++] = i;
    }

    void _beginPreDelay(uint32_t ms) {
        _preDelayMs = ms;
        _phase      = Phase::PreDelay;
        _phaseStart = millis();
    }

    void _finish(bool ok) {
        _active = false;
        if (!ok) {
            auto& c = Config::get();
            WiFi.softAP(c.deviceName, c.apPassword, 1);
            WiFi.setTxPower(WIFI_TX_POWER);
        }
        DoneCb cb = _onDone;
        _onDone   = nullptr;
        if (cb) cb(ok);
    }
};

// Elects a single mesh peer to hold the actual WiFi STA connection when
// Config::get().wifiSingleClientMode is enabled, so battery-powered peers can
// skip joining the router entirely (see CLAUDE task: mesh single-client WiFi).
//
// Rule, deliberately simple: every candidate (a device with ≥1 WiFi network
// configured) tries to connect whenever it doesn't know of anyone else being
// connected. If it turns out a lower-MAC peer is also connected — or beats it
// to it while it's still mid-attempt — the higher-MAC one yields (aborts its
// attempt, or disconnects if it already succeeded) and stands by instead.
// There's no coordination beyond that: no staggered turns, no rank-based
// waiting. The trade-off is that every election event (fresh mesh boot with
// several candidates, or a failover once the current client drops) makes all
// remaining candidates' radios fire up at once instead of one at a time —
// acceptable for the handful of devices a mesh like this has, in exchange for
// a much simpler, easier-to-reason-about state machine.
//
// Once any candidate is confirmed connected (observed via PresenceMsg /
// local WiFi.status()), everyone else stands down to standby (AP-only, no
// STA attempts) until that peer drops off the mesh or loses its connection,
// at which point the same race runs again automatically.
class WifiElection {
public:
    void begin(PeerRegistry* peers) {
        _peers = peers;
        _enterWaiting();
    }

    // Fires whenever isAttempting() flips, so the caller can push an
    // immediate update (e.g. over the websocket) instead of waiting for the
    // next presence-driven refresh, which only covers peers, not self.
    void setOnAttemptingChanged(std::function<void()> cb) { _onAttemptingChanged = cb; }

    // Call whenever Config::get().wifiSingleClientMode transitions, so a
    // peer that had the mode turned off remotely (no reboot for it) resumes
    // "everyone connects" immediately instead of being stuck on standby.
    void onPolicyChanged(bool nowEnabled) {
        if (!nowEnabled && WiFi.status() != WL_CONNECTED && !_attempt.active())
            _attempt.start([](bool) {});
        if (nowEnabled) _enterWaiting();
    }

    // Non-blocking: connects if needed, then invokes onReady once WL_CONNECTED
    // (or once all configured networks were tried and failed — the caller,
    // e.g. Updater, already handles "not connected to WiFi" gracefully).
    // Used for on-demand OTA checks/installs on a peer that is on standby.
    void requestTemporary(std::function<void()> onReady) {
        if (WiFi.status() == WL_CONNECTED) { onReady(); return; }
        if (!_otaHold) _stateBeforeOta = _state;
        _otaHold = true;
        // Chain rather than overwrite — a second caller (e.g. a mesh-triggered
        // check arriving alongside a local web request) must still get its
        // callback invoked once the connection settles.
        if (_otaCallback) {
            auto prev    = _otaCallback;
            _otaCallback = [prev, onReady]() { prev(); onReady(); };
        } else {
            _otaCallback = onReady;
        }
        if (!_attempt.active()) _attempt.start([](bool) {});
    }

    void tick() {
        // Runs on every exit path (including early returns below) so a
        // start/stop of the connect attempt is always reported exactly once.
        struct AttemptingGuard {
            WifiElection* self;
            bool          was;
            ~AttemptingGuard() {
                if (self->_attempt.active() != was && self->_onAttemptingChanged) self->_onAttemptingChanged();
            }
        } guard{this, _attempt.active()};

        _attempt.tick();

        if (_otaHold) {
            if (WiFi.status() == WL_CONNECTED)      { _finishOtaHold(); return; }
            if (!_attempt.active())                 { _finishOtaHold(); return; }
            return; // let the in-flight attempt finish before anything else runs
        }

        if (!Config::get().wifiSingleClientMode) return; // default path: untouched
        if (Config::wifiCount() == 0) return;             // not a candidate — nothing to elect

        uint8_t ownMac[6];
        WiFi.macAddress(ownMac);
        bool selfConnected = WiFi.status() == WL_CONNECTED;
        bool anyConnected  = selfConnected || _anyPeerConnected();

        switch (_state) {
            case State::Waiting: {
                if (selfConnected) {
                    // Already connected — e.g. the mode was just turned on
                    // while this device had a live connection from before, or
                    // it independently connected before discovering a peer.
                    // Adopt it; the Connected case below yields it to a
                    // lower-MAC peer if one turns out to also be connected.
                    Logger::i("[wifi-elect] already connected — adopting as this mesh's WiFi client");
                    _state = State::Connected;
                    return;
                }
                if (anyConnected) {
                    Logger::i("[wifi-elect] another candidate connected — standing by");
                    _state = State::Standby;
                    return;
                }
                if (_coolingDown) {
                    if (millis() - _cooldownStart < RETRY_COOLDOWN_MS) break;
                    _coolingDown = false;
                }
                Logger::i("[wifi-elect] nobody connected — attempting to connect");
                _state = State::Connecting;
                _attempt.start([this](bool ok) { _onAttemptDone(ok); });
                break;
            }
            case State::Connecting:
                if (_lowerMacPeerConnected(ownMac)) {
                    // A lower-MAC peer beat us to it while we were still
                    // mid-attempt — no point finishing (or worse, connecting
                    // and immediately having to yield), so stop right now.
                    Logger::i("[wifi-elect] a lower-MAC peer connected while I was still trying — aborting");
                    _attempt.abort();
                    _state = State::Standby;
                }
                break; // otherwise handled by the DoneCb passed to _attempt.start()
            case State::Connected:
                if (!selfConnected) {
                    Logger::w("[wifi-elect] lost connection — re-electing");
                    _enterWaiting();
                } else if (_lowerMacPeerConnected(ownMac)) {
                    // Two candidates ended up connected at once (e.g. both
                    // connected independently before finding each other in
                    // the mesh) — yield to the lower MAC so the mesh settles
                    // back to exactly one WiFi client.
                    Logger::w("[wifi-elect] a lower-MAC peer is also connected — yielding");
                    WiFi.disconnect(false);
                    auto& c = Config::get();
                    WiFi.softAP(c.deviceName, c.apPassword, 1);
                    WiFi.setTxPower(WIFI_TX_POWER);
                    _state = State::Standby;
                }
                break;
            case State::Standby:
                if (!anyConnected) {
                    Logger::i("[wifi-elect] elected peer went offline — re-electing");
                    _enterWaiting();
                }
                break;
        }
    }

    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }

    // True while a WifiConnectAttempt is actually in flight (WiFi.begin()
    // issued, waiting on the result) — either this device's own election
    // turn or a temporary OTA connect. Broadcast in PresenceMsg so the
    // device list can show a distinct "connecting…" state.
    bool isAttempting() const { return _attempt.active(); }

private:
    enum class State { Waiting, Connecting, Connected, Standby };

    // After this device's own attempt fails, don't immediately retry (the
    // network is likely still unreachable) — wait this long first so a
    // chronically-unreachable network doesn't get hammered in a tight loop.
    // Unrelated to coordinating with peers; purely local hygiene.
    static constexpr uint32_t RETRY_COOLDOWN_MS = 30000; // 30s

    PeerRegistry*      _peers = nullptr;
    State              _state = State::Waiting;
    bool               _coolingDown  = false;
    uint32_t           _cooldownStart = 0;
    WifiConnectAttempt _attempt;

    bool                   _otaHold        = false;
    State                  _stateBeforeOta = State::Waiting;
    std::function<void()> _otaCallback;
    std::function<void()> _onAttemptingChanged;

    void _enterWaiting() {
        _state       = State::Waiting;
        _coolingDown = false;
    }

    void _onAttemptDone(bool ok) {
        if (ok) {
            Logger::i("[wifi-elect] connected — acting as the mesh's WiFi client");
            _state = State::Connected;
            return;
        }
        Logger::w("[wifi-elect] failed to connect — cooling down before retrying");
        _coolingDown   = true;
        _cooldownStart = millis();
        _state         = State::Waiting;
    }

    void _finishOtaHold() {
        _otaHold = false;
        bool wasLeader = _stateBeforeOta == State::Connected;
        if (wasLeader) {
            // Already the elected leader — OTA just used the existing
            // connection, nothing to undo, keep acting as the WiFi client.
            _state = State::Connected;
        } else {
            if (Config::get().wifiSingleClientMode && WiFi.status() == WL_CONNECTED) {
                // We connected only for this OTA request and aren't the
                // elected leader — hand the radio back and let election
                // re-settle instead of staying connected indefinitely.
                WiFi.disconnect(false);
                auto& c = Config::get();
                WiFi.softAP(c.deviceName, c.apPassword, 1);
                WiFi.setTxPower(WIFI_TX_POWER);
            }
            _enterWaiting();
        }
        std::function<void()> cb = _otaCallback;
        _otaCallback = nullptr;
        if (cb) cb();
    }

    // Excludes self on purpose — callers combine this with their own
    // WiFi.status() check so they can tell "I'm the one connected" apart
    // from "a peer is", which need different reactions (adopt vs. stand by).
    bool _anyPeerConnected() const {
        if (!_peers) return false;
        for (auto& p : *_peers)
            if (p.active && p.online() && p.wifiConnected) return true;
        return false;
    }

    bool _lowerMacPeerConnected(const uint8_t* ownMac) const {
        if (!_peers) return false;
        for (auto& p : *_peers)
            if (p.active && p.online() && p.wifiConnected && memcmp(p.mac, ownMac, 6) < 0) return true;
        return false;
    }
};
