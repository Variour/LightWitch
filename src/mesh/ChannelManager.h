#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "../logging/Logger.h"

// Coordinates the ESP-NOW radio channel so all mesh peers stay on the same channel.
//
// Call begin() after setupWifi() and before MeshManager::begin().
// Call tick() from loop().
// Call beginSearch() to trigger a manual re-search from the web UI.
//
// State machine:
//   WiFi connected    → LOCKED (channel saved to NVS)
//   WiFi failed       → SEARCH (stored channel first, then 1/6/11)
//   SEARCH + peer heard  → LOCKED (channel saved to NVS)
//   SEARCH + full cycle  → LOCKED (NVS unchanged, fall back to stored or ch 1)
//   SEARCH + WiFi reconnects → LOCKED (channel saved to NVS)
//   LOCKED + beginSearch()   → SEARCH

class ChannelManager {
public:
    // Called by MeshManager when a heartbeat is received. During SEARCH, signals
    // that the current channel has a live peer.
    void onPeerHeard() {
        if (_state != State::Searching) return;
        uint8_t ch = _currentChannel();
        _saveChannel(ch);
        Logger::i("[ch] peer heard on ch %u — locked", ch);
        _lock(ch);
    }

    void begin() {
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
            // Full cycle with no peer heard — lock to stored channel or ch 1, NVS unchanged
            uint8_t fallback = _storedChannel > 0 ? _storedChannel : 1;
            Logger::i("[ch] search complete, no peers — locking to ch %u", fallback);
            _lock(fallback);
            return;
        }

        _applyChannel(_searchSeq[_searchIdx]);
        _dwellStart = now;
        _dwellMs    = _randomDwell();
        Logger::i("[ch] search: ch %u (dwell %u ms)", _searchSeq[_searchIdx], _dwellMs);
    }

    // Trigger a manual re-search (called from web UI endpoint).
    // Bypasses the SoftAP client guard — user explicitly confirmed disruption.
    void beginSearch() {
        Logger::i("[ch] manual re-search triggered");
        _startSearch();
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

    // Search sequence: [stored, 1, 6, 11] deduplicated
    static constexpr uint8_t MAX_SEQ = 4;
    uint8_t _searchSeq[MAX_SEQ];
    uint8_t _searchLen  = 0;
    uint8_t _searchIdx  = 0;

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

    void _buildSearchSeq() {
        _searchLen = 0;
        static const uint8_t standard[] = {1, 6, 11};
        // Stored channel first (if set)
        if (_storedChannel > 0) _searchSeq[_searchLen++] = _storedChannel;
        for (uint8_t ch : standard) {
            if (ch != _storedChannel) _searchSeq[_searchLen++] = ch;
        }
    }

    void _startSearch() {
        _state     = State::Searching;
        _searchIdx = 0;
        _buildSearchSeq();
        _applyChannel(_searchSeq[0]);
        _dwellStart = millis();
        _dwellMs    = _randomDwell();
        Logger::i("[ch] search start: ch %u (dwell %u ms)", _searchSeq[0], _dwellMs);
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
        // 12000–18000 ms: widened from the original 6–9 s (#153) so two
        // independently-booting devices get a bigger window to overlap on a
        // shared channel — reduces (does not eliminate) disjoint subsets
        // each locking to a different channel before ever hearing each other
        // (#321). Still comfortably covers the 5 s heartbeat period; the
        // trade-off is a lone device takes longer to give up and fall back
        // (~36–54 s full 3-channel cycle instead of ~18–27 s).
        return 12000 + (uint32_t)(esp_random() % 6001);
    }
};
