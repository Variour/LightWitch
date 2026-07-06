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
// Election rule: among online peers that have ≥1 WiFi network configured
// ("candidates"), the lowest MAC address goes first. If it hasn't connected
// (or has disconnected) by its turn, the next-lowest-MAC candidate tries, and
// so on — computed independently by every device from its own view of
// PeerRegistry, using the shared moment "no candidate is connected" as a
// common reference point so ranks resolve to the same order everywhere.
// A candidate that exhausts its own attempt does another full round later
// (RANK_BUDGET_MS * candidateCount) rather than hammering retries, in case
// the network was briefly unavailable.
//
// Once any candidate is confirmed connected (observed via PresenceMsg /
// local WiFi.status()), everyone else stands down to standby (AP-only, no
// STA attempts) until that peer drops off the mesh or loses its connection,
// at which point the same ranked hand-off runs again automatically.
//
// Joining an existing, already-settled mesh: a freshly booted device hasn't
// heard any presence yet, so without care it would see an empty peer list
// and conclude "nobody's connected" even if a peer has been happily
// connected for weeks — see DISCOVERY_GRACE_MS below for the mitigation.
// This is best-effort, not a guarantee (no central coordinator): two
// candidates can still end up connected at once, e.g. both connecting
// independently before finding each other in the mesh. When that happens,
// the higher-MAC one yields (disconnects) as soon as it learns the lower-MAC
// one is also connected — see the Connected case in tick() — so the mesh
// still converges back to exactly one WiFi client, just not instantly.
class WifiElection {
public:
    void begin(PeerRegistry* peers) {
        _peers = peers;
        _enterFreshWaiting();
        // A freshly booted device knows nothing about the mesh yet — give it
        // time to hear presence from an already-connected peer (channel
        // search + at least one heartbeat cycle) before it's allowed to act
        // on "nobody seems connected". Only applies to this first boot-time
        // decision; later re-elections (failover) skip straight to the
        // normal rank-based wait, since the mesh view is already populated.
        _bootGraceStart = millis();
    }

    // Call whenever Config::get().wifiSingleClientMode transitions, so a
    // peer that had the mode turned off remotely (no reboot for it) resumes
    // "everyone connects" immediately instead of being stuck on standby.
    void onPolicyChanged(bool nowEnabled) {
        if (!nowEnabled && WiFi.status() != WL_CONNECTED && !_attempt.active())
            _attempt.start([](bool) {});
        if (nowEnabled) _enterFreshWaiting();
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
                uint32_t candidates = _countCandidates() + 1; // +self
                uint32_t rank       = _computeRank(ownMac);
                uint32_t threshold  = (uint32_t)(_myAttemptCount * candidates + rank) * RANK_BUDGET_MS;
                if (millis() - _waitSince >= threshold && millis() - _bootGraceStart >= DISCOVERY_GRACE_MS) {
                    Logger::i("[wifi-elect] my turn (rank %u/%u) — attempting to connect", rank, candidates);
                    _state = State::Connecting;
                    _attempt.start([this](bool ok) { _onAttemptDone(ok); });
                }
                break;
            }
            case State::Connecting:
                break; // handled by the DoneCb passed to _attempt.start()
            case State::Connected:
                if (!selfConnected) {
                    Logger::w("[wifi-elect] lost connection — re-electing");
                    _enterFreshWaiting();
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
                    _enterFreshWaiting();
                }
                break;
        }
    }

    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }

private:
    enum class State { Waiting, Connecting, Connected, Standby };

    // Covers one candidate's realistic turn: with the common case of 1-2
    // configured networks, exhausting them (3 attempts * 10s each, plus
    // settle delays) takes well under this. A candidate with many more
    // networks configured may still be mid-attempt when the next rank's
    // turn opens — in that rare case both may briefly hold the connection
    // until the next election cycle resolves it, which is an acceptable
    // trade-off for not making every failover wait several minutes.
    static constexpr uint32_t RANK_BUDGET_MS = 60000; // 1 min per rank step

    // Covers channel search (worst case a few dwell cycles) plus at least one
    // presence heartbeat (5s) after that, so a newly booted device has a real
    // chance to hear "someone's already connected" before deciding it's alone.
    // Best-effort, not a guarantee — see WifiElection class comment.
    static constexpr uint32_t DISCOVERY_GRACE_MS = 20000; // 20s, boot only

    PeerRegistry*      _peers = nullptr;
    State              _state = State::Waiting;
    uint32_t           _waitSince      = 0;
    uint32_t           _myAttemptCount = 0;
    uint32_t           _bootGraceStart = 0;
    WifiConnectAttempt _attempt;

    bool                   _otaHold        = false;
    State                  _stateBeforeOta = State::Waiting;
    std::function<void()> _otaCallback;

    void _enterFreshWaiting() {
        _state          = State::Waiting;
        _waitSince      = millis();
        _myAttemptCount = 0;
    }

    void _onAttemptDone(bool ok) {
        if (ok) {
            Logger::i("[wifi-elect] connected — acting as the mesh's WiFi client");
            _state = State::Connected;
            return;
        }
        Logger::w("[wifi-elect] failed to connect — waiting for another round");
        _myAttemptCount++;
        _state = State::Waiting; // same epoch — _waitSince stays put, next round's threshold is later
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
            _enterFreshWaiting();
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

    // Count of online candidates (peers with ≥1 WiFi network configured),
    // excluding self.
    uint32_t _countCandidates() const {
        uint32_t n = 0;
        if (!_peers) return n;
        for (auto& p : *_peers)
            if (p.active && p.online() && p.hasWifiNetworks) n++;
        return n;
    }

    // This device's position (0 = first) among online candidates including
    // self, ordered by ascending MAC address.
    uint32_t _computeRank(const uint8_t* ownMac) const {
        uint32_t rank = 0;
        if (!_peers) return rank;
        for (auto& p : *_peers)
            if (p.active && p.online() && p.hasWifiNetworks && memcmp(p.mac, ownMac, 6) < 0) rank++;
        return rank;
    }
};
