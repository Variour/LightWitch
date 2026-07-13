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
// Mirrors the original retry shape: try each configured network in list
// order (first to last, no "last known good" stickiness — see #323), 3
// attempts of up to 10s each, with a short settle delay between attempts.
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
            if (_netIdx != Config::wifiLast()) {
                Config::setWifiLast(_netIdx);
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
    uint8_t  _count      = 0;
    uint8_t  _netIdx     = 0;
    uint8_t  _attemptNum = 0;
    DoneCb   _onDone;

    const char* _ssid() const { return Config::wifiNetworks()[_netIdx].ssid; }
    const char* _pass() const { return Config::wifiNetworks()[_netIdx].password; }

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
// connected. Whoever gets there first keeps it — stability wins, not MAC
// order: if another candidate connects while this device is still mid-
// attempt, it just aborts and stands by, regardless of either MAC (see the
// Connecting case in tick()). MAC only comes into play as a last-resort
// tiebreaker for the one case that's genuinely symmetric and would
// otherwise livelock: two candidates who both end up connected at once
// (e.g. both succeeded independently before finding each other in the
// mesh) — there the higher-MAC one yields (see the Connected case), since
// without some deterministic rule both sides would see "someone else is
// also connected" and both disconnect. There's no coordination beyond
// that: no staggered turns, no rank-based waiting. The trade-off is that
// every election event (fresh mesh boot with several candidates, or a
// failover once the current client drops) makes all remaining candidates'
// radios fire up at once instead of one at a time — acceptable for the
// handful of devices a mesh like this has, in exchange for a much
// simpler, easier-to-reason-about state machine.
//
// Once any candidate is confirmed connected (observed via PresenceMsg /
// local WiFi.status()), everyone else stands down to standby (AP-only, no
// STA attempts) until that peer drops off the mesh or loses its connection,
// at which point the same race runs again automatically.
//
// A failed attempt (every configured network exhausted, same one round as
// the original blocking setupWifi() always did) is terminal, not retried —
// this device just goes quiet (State::GaveUp) until something actually
// changes: the mode gets toggled, an OTA request needs the radio, or a peer
// is observed to connect (worth then watching in case *that* connection
// later drops). This mirrors the pre-single-client-mode behavior, where a
// device that couldn't reach any configured network simply stayed on AP
// until manually rebooted, rather than hammering it forever.
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
        if (!nowEnabled) {
            if (WiFi.status() != WL_CONNECTED && !_attempt.active())
                _attempt.start([](bool) {});
            return;
        }

        // If the mode is re-enabled while a non-OTA background connect from
        // the old "everyone connects" path is still in flight, abort it so we
        // don't finish connecting under the wrong policy and only notice later.
        if (_attempt.active() && !_otaHold) _attempt.abort();
        if (!_otaHold) _enterWaiting();
    }

    // Non-blocking: connects if needed, then invokes onReady once WL_CONNECTED
    // (or once all configured networks were tried and failed — the caller,
    // e.g. Updater, already handles "not connected to WiFi" gracefully).
    // Used for on-demand OTA checks/installs on a peer that is on standby.
    //
    // Important: this only guarantees the connection is up when onReady()
    // runs — it does NOT tear it down right after. Updater's checks/applies
    // run asynchronously and need the radio for their whole duration, so the
    // caller must call releaseTemporary() once it knows the real operation
    // has actually finished (see main.cpp's loop()).
    void requestTemporary(std::function<void()> onReady) {
        if (WiFi.status() == WL_CONNECTED) { onReady(); return; }
        if (!_otaHold) {
            _stateBeforeOta    = _state;
            _otaConnectedFired = false;
        }
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

    // Call once the operation that needed the temporary connection has
    // actually finished (e.g. Updater::status() left Checking/Downloading) —
    // hands the radio back if this device isn't the elected leader. A no-op
    // before onReady() has actually fired (nothing connected yet to release),
    // so it's safe to call speculatively/repeatedly from a status poll.
    void releaseTemporary() {
        if (!_otaHold || !_otaConnectedFired) return;
        bool wasLeader = _stateBeforeOta == State::Connected || _state == State::Connected;
        _otaHold           = false;
        _otaConnectedFired = false;
        if (wasLeader) {
            _state = State::Connected;
        } else if (_stateBeforeOta != State::Connecting) {
            if (Config::get().wifiSingleClientMode && WiFi.status() == WL_CONNECTED) {
                WiFi.disconnect(false);
                auto& c = Config::get();
                WiFi.softAP(c.deviceName, c.apPassword, 1);
                WiFi.setTxPower(WIFI_TX_POWER);
            }
            _state = _stateBeforeOta;
        }
        // else: _stateBeforeOta was Connecting and we didn't end up the
        // leader — the piggybacked attempt's own completion already left
        // _state exactly where it should be (GaveUp or Standby); leave it.
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
            if (WiFi.status() == WL_CONNECTED) {
                // Connected — notify the caller(s) exactly once and then
                // just keep holding; releaseTemporary() does the teardown
                // once the real operation is done, not right after this.
                if (!_otaConnectedFired) {
                    _otaConnectedFired = true;
                    std::function<void()> cb = _otaCallback;
                    _otaCallback = nullptr;
                    if (cb) cb();
                }
                return;
            }
            if (!_attempt.active()) {
                // Exhausted every configured network without connecting —
                // nothing to hold onto, so release immediately. Still fire
                // the callback so the caller's own guard (e.g. Updater)
                // reports its normal "not connected to WiFi" failure.
                std::function<void()> cb = _otaCallback;
                _otaCallback       = nullptr;
                _otaHold           = false;
                _otaConnectedFired = false;
                if (_stateBeforeOta != State::Connecting) _state = _stateBeforeOta;
                if (cb) cb();
                return;
            }
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
                Logger::i("[wifi-elect] nobody connected — attempting to connect");
                _state = State::Connecting;
                _attempt.start([this](bool ok) { _onAttemptDone(ok); });
                break;
            }
            case State::Connecting:
                if (_anyPeerConnected()) {
                    // Somebody else already made it while we were still
                    // mid-attempt — stability wins here regardless of MAC
                    // (no point finishing just to immediately have to yield):
                    // whoever got there first keeps it, we stand down. This
                    // is still race-safe for a genuine simultaneous start —
                    // neither side sees the other as connected until one of
                    // them actually succeeds, so there's no symmetric case
                    // to break a tie on here (see the Connected case for the
                    // one spot that still needs a MAC-based tiebreaker: both
                    // ending up connected at once).
                    Logger::i("[wifi-elect] another candidate connected while I was still trying — aborting");
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
            case State::GaveUp:
                // Passive from here — only reacts to a peer actually
                // succeeding (worth then watching in case it drops again).
                // No self-triggered retry; see class comment for why.
                if (anyConnected) _state = State::Standby;
                break;
        }
    }

    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }

    // True while a WifiConnectAttempt is actually in flight (WiFi.begin()
    // issued, waiting on the result) — either this device's own election
    // turn or a temporary OTA connect. Broadcast in PresenceMsg so the
    // device list can show a distinct "connecting…" state.
    bool isAttempting() const { return _attempt.active(); }

    // What this device should advertise as PresenceMsg.wifiConnected — true
    // only when the connection is (or just became, via a piggybacked
    // election attempt) this device's actual standing as the mesh's WiFi
    // client, never for a plain OTA-only hold that's going back to standby
    // once the request is done. Without this, a peer briefly connected only
    // to run an OTA check could make the real leader think a legitimate
    // lower-MAC candidate had shown up and wrongly yield to it (see
    // _lowerMacPeerConnected in tick()). Deliberately not used for this
    // device's own local WiFi.status() display (see WebServer.h) — the user
    // looking at this exact device's own page still wants to see "yes, I'm
    // online right now", even if the rest of the mesh shouldn't treat that
    // connection as anything more than a passing OTA errand.
    bool isAdvertisableConnected() const {
        if (WiFi.status() != WL_CONNECTED) return false;
        if (!_otaHold) return true;
        return _stateBeforeOta == State::Connected || _state == State::Connected;
    }

    // Manual nudge — e.g. a "retry WiFi" UI button — for a device stuck in
    // GaveUp (or just idling in Standby/Waiting) to take a fresh, single-
    // round shot right now instead of waiting for the mode to be toggled.
    // No-op while already Connecting or Connected: nothing to retry, and
    // resetting mid-attempt would desync _state from the still-running
    // WifiConnectAttempt.
    void retryNow() {
        if (_state == State::Connecting || _state == State::Connected) return;
        Logger::i("[wifi-elect] manual retry requested");
        _enterWaiting();
    }

private:
    enum class State { Waiting, Connecting, Connected, Standby, GaveUp };

    PeerRegistry*      _peers = nullptr;
    State              _state = State::Waiting;
    WifiConnectAttempt _attempt;

    bool                   _otaHold           = false;
    bool                   _otaConnectedFired = false;
    State                  _stateBeforeOta    = State::Waiting;
    std::function<void()> _otaCallback;
    std::function<void()> _onAttemptingChanged;

    void _enterWaiting() {
        _state = State::Waiting;
    }

    void _onAttemptDone(bool ok) {
        if (ok) {
            Logger::i("[wifi-elect] connected — acting as the mesh's WiFi client");
            _state = State::Connected;
            return;
        }
        Logger::w("[wifi-elect] failed to connect (all configured networks exhausted) — giving up until something changes");
        _state = State::GaveUp;
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
