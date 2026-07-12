#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "../logging/Logger.h"
#include "PeerRegistry.h"

// Coordinates the ESP-NOW radio channel so all mesh peers stay on the same channel.
//
// Call begin(peers) after setupWifi() and before MeshManager::begin().
// Call tick() from loop().
// Call beginSearch() to trigger a re-search: from the local "Search devices"
// web UI click, and again on every device that receives the resulting
// mesh-wide MeshSearchMsg broadcast (see MeshManager::broadcastMeshSearch),
// so a click on any one device reconciles the whole mesh, not just itself.
//
// State machine:
//   WiFi connected    → LOCKED (channel saved to NVS)
//   WiFi failed       → SEARCH (stored channel first, then 1/6/11)
//   SEARCH + new peer heard  → LOCKED (channel saved to NVS)
//   SEARCH + already-known peer heard → ignored, keep searching (#321)
//   SEARCH + 2 full cycles, no new peer → LOCKED (NVS unchanged, fall back to common ch 1)
//   SEARCH + WiFi reconnects → LOCKED (channel saved to NVS)
//   LOCKED + beginSearch()   → SEARCH (starting on the channel after the current one)

class ChannelManager {
public:
    // Called by MeshManager when a heartbeat is received, with the sender's
    // MAC. During SEARCH, signals that the current channel has a live peer —
    // unless that peer is one we already knew about before this search
    // started (see _snapshotKnownPeers), in which case it's not new
    // information and we keep looking. Without this, two devices that were
    // already locked together can wander onto some other free channel,
    // still hear each other there, and immediately re-lock to each other
    // without ever reaching whatever bigger/different group this search was
    // actually meant to find (#321).
    void onPeerHeard(const uint8_t* mac) {
        if (_state != State::Searching) return;
        if (_isAlreadyKnown(mac)) return;
        uint8_t ch = _currentChannel();
        _saveChannel(ch);
        Logger::i("[ch] peer heard on ch %u — locked", ch);
        _lock(ch);
    }

    void begin(PeerRegistry* peers) {
        _peers = peers;
        _loadChannel();
        _lastWifiState = WiFi.status();

        if (WiFi.status() == WL_CONNECTED) {
            uint8_t ch = (uint8_t)WiFi.channel();
            _saveChannel(ch);
            Logger::i("[ch] wifi connected, locked to ch %u", ch);
            _lock(ch);
            return;
        }

        Logger::i("[ch] wifi not connected, starting channel search");
        _startSearch();
    }

    void tick() {
        if (_state != State::Searching) {
            // Check for WiFi connecting while locked (or idle)
            if (WiFi.status() == WL_CONNECTED && _lastWifiState != WL_CONNECTED) {
                uint8_t ch = (uint8_t)WiFi.channel();
                _saveChannel(ch);
                Logger::i("[ch] wifi reconnected, locked to ch %u", ch);
                _lock(ch);
            }
            _lastWifiState = WiFi.status();
            return;
        }

        // WiFi reconnected mid-search
        if (WiFi.status() == WL_CONNECTED) {
            uint8_t ch = (uint8_t)WiFi.channel();
            _saveChannel(ch);
            Logger::i("[ch] wifi reconnected during search, locked to ch %u", ch);
            _lock(ch);
            _lastWifiState = WL_CONNECTED;
            return;
        }
        _lastWifiState = WiFi.status();

        uint32_t now = millis();
        if (now - _dwellStart < _dwellMs) return;

        // Advance to next channel in sequence
        _searchIdx++;
        if (_searchIdx >= _searchLen) {
            _round++;
            if (_round < SEARCH_ROUNDS) {
                // Full pass, no peer heard — go again before giving up. A second
                // pass gives a device on a different phase (staggered boot) or a
                // different stored-channel order another chance to overlap (#321).
                _searchIdx = 0;
                _applyChannel(_searchSeq[0]);
                _dwellStart = now;
                _dwellMs    = _randomDwell();
                Logger::i("[ch] search round %u/%u complete, no peers — retrying: ch %u (dwell %u ms)",
                          _round, SEARCH_ROUNDS, _searchSeq[0], _dwellMs);
                return;
            }
            // Both rounds exhausted, no peer heard — lock to a common fallback
            // channel, not this device's own stored channel. Devices with
            // different stored-channel history that never overlapped during
            // search still converge on one shared channel this way instead of
            // a silent, permanent split (#321). NVS unchanged.
            Logger::i("[ch] search exhausted (%u rounds), no peers — locking to common ch %u",
                      SEARCH_ROUNDS, COMMON_FALLBACK_CHANNEL);
            _lock(COMMON_FALLBACK_CHANNEL);
            return;
        }

        _applyChannel(_searchSeq[_searchIdx]);
        _dwellStart = now;
        _dwellMs    = _randomDwell();
        Logger::i("[ch] search: ch %u (dwell %u ms)", _searchSeq[_searchIdx], _dwellMs);
    }

    // Trigger a manual, mesh-wide re-search (called from web UI endpoint and
    // from every device that receives the resulting MeshSearchMsg broadcast —
    // see MeshManager::broadcastMeshSearch). Bypasses the SoftAP client guard
    // — user explicitly confirmed disruption. Starts on the channel *after*
    // the one this device is currently on (see _buildSearchSeq) so a mesh
    // that's already split doesn't just immediately re-form the same islands.
    void beginSearch() {
        uint8_t leaveFrom = _currentChannel();
        Logger::i("[ch] manual re-search triggered, leaving ch %u", leaveFrom);
        _startSearch(leaveFrom);
    }

    uint8_t lockedChannel() const { return _lockedChannel; }
    bool    isSearching()   const { return _state == State::Searching; }

private:
    static constexpr const char* NVS_NS  = "bl";
    static constexpr const char* NVS_KEY = "ch";

    enum class State { Locked, Searching };

    State    _state         = State::Locked;
    uint8_t  _lockedChannel = 1;
    uint8_t  _storedChannel = 0;   // 0 = nothing stored
    wl_status_t _lastWifiState = WL_IDLE_STATUS;

    PeerRegistry* _peers = nullptr;

    // Snapshot of peers already known when the current search began (#321) —
    // see onPeerHeard. Sized to PeerRegistry's own cap since it can never
    // hold more entries than that.
    static constexpr uint8_t MAX_KNOWN = PeerRegistry::MAX_PEERS;
    uint8_t _knownMacs[MAX_KNOWN][6];
    uint8_t _knownCount = 0;

    // Search sequence: [stored, 1, 6, 11] deduplicated
    static constexpr uint8_t MAX_SEQ = 4;
    uint8_t _searchSeq[MAX_SEQ];
    uint8_t _searchLen  = 0;
    uint8_t _searchIdx  = 0;

    // Number of full passes through _searchSeq before giving up (#321).
    static constexpr uint8_t SEARCH_ROUNDS = 2;
    uint8_t _round = 0;

    // Shared channel every device falls back to once search is exhausted with
    // no peer heard — deliberately not each device's own stored channel, so a
    // mesh with mixed stored-channel history still has one common rendezvous
    // point instead of each device silently parking on a different channel.
    static constexpr uint8_t COMMON_FALLBACK_CHANNEL = 1;

    uint32_t _dwellStart = 0;
    uint32_t _dwellMs    = 0;

    void _loadChannel() {
        Preferences prefs;
        if (prefs.begin(NVS_NS, true)) {
            uint8_t v = prefs.getUChar(NVS_KEY, 0);
            prefs.end();
            if (v >= 1 && v <= 13) {
                _storedChannel = v;
                Logger::i("[ch] loaded stored channel %u from NVS", v);
            }
        }
    }

    void _saveChannel(uint8_t ch) {
        Preferences prefs;
        if (prefs.begin(NVS_NS, false)) {
            prefs.putUChar(NVS_KEY, ch);
            prefs.end();
        }
        _storedChannel = ch;
    }

    // leaveFrom == 0: boot-time search — stored channel first (most likely to
    // reconnect fast if nothing has actually changed).
    // leaveFrom != 0: reconciliation search (beginSearch(), #321) — start on
    // the *next* channel after the one we're leaving, standard triple rotated
    // accordingly. A device that's Locked and gets told to re-search is, by
    // definition, already parked with whoever it currently hears — searching
    // that same channel first would just re-lock the same group immediately
    // without ever looking elsewhere, which defeats the point of a mesh-wide
    // reconciliation search (see MeshManager::broadcastMeshSearch).
    void _buildSearchSeq(uint8_t leaveFrom = 0) {
        _searchLen = 0;
        static const uint8_t standard[] = {1, 6, 11};
        if (leaveFrom == 0) {
            if (_storedChannel > 0) _searchSeq[_searchLen++] = _storedChannel;
            for (uint8_t ch : standard) {
                if (ch != _storedChannel) _searchSeq[_searchLen++] = ch;
            }
            return;
        }
        uint8_t startIdx = 0;
        for (uint8_t i = 0; i < 3; i++) {
            if (standard[i] == leaveFrom) { startIdx = (i + 1) % 3; break; }
        }
        for (uint8_t i = 0; i < 3; i++) _searchSeq[_searchLen++] = standard[(startIdx + i) % 3];
    }

    void _startSearch(uint8_t leaveFrom = 0) {
        _state     = State::Searching;
        _searchIdx = 0;
        _round     = 0;
        _snapshotKnownPeers();
        _buildSearchSeq(leaveFrom);
        _applyChannel(_searchSeq[0]);
        _dwellStart = millis();
        _dwellMs    = _randomDwell();
        Logger::i("[ch] search start: ch %u (dwell %u ms)", _searchSeq[0], _dwellMs);
    }

    // Records who we already knew about right before this search started, so
    // onPeerHeard can tell "new peer" apart from "someone I already had".
    // At boot this runs before MeshManager has received anything, so it's
    // naturally empty and every peer heard during a boot search is new —
    // this only changes behavior for a reconciliation search (#321).
    void _snapshotKnownPeers() {
        _knownCount = 0;
        if (!_peers) return;
        for (auto& p : *_peers) {
            if (!p.active) continue;
            if (_knownCount >= MAX_KNOWN) break;
            memcpy(_knownMacs[_knownCount++], p.mac, 6);
        }
    }

    bool _isAlreadyKnown(const uint8_t* mac) const {
        for (uint8_t i = 0; i < _knownCount; i++)
            if (memcmp(_knownMacs[i], mac, 6) == 0) return true;
        return false;
    }

    void _lock(uint8_t ch) {
        _applyChannel(ch);
        _lockedChannel = ch;
        _state = State::Locked;
    }

    void _applyChannel(uint8_t ch) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }

    uint8_t _currentChannel() const {
        return (_state == State::Searching && _searchIdx < _searchLen)
            ? _searchSeq[_searchIdx] : _lockedChannel;
    }

    uint32_t _randomDwell() {
        // 7000–10000 ms: nudged up from the original 6–9 s (#153), still
        // comfortably covering the 5 s heartbeat period per dwell. Island-
        // formation odds (#321) are mainly addressed by SEARCH_ROUNDS and
        // COMMON_FALLBACK_CHANNEL below, not by widening this further —
        // repeating the whole sequence gives more independent chances to
        // land on a shared channel than one long dwell does.
        return 7000 + (uint32_t)(esp_random() % 3001);
    }
};
