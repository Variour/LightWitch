#pragma once
#include <Arduino.h>

#include <functional>

#include "../config/Config.h"

// Central log of GenericEvent broadcasts (issue #442), recorded in receipt
// order by whichever device this runs on — RAM only, no persistence across
// reboot (confirmed acceptable; ordering only needs to be receipt order at
// this device, no clock sync). Sender identity is resolved by the caller
// (PeerRegistry/self name lookup by mac) before recording, same as
// AutomationManager::onGenericEvent — the mesh layer only identifies senders
// by mac.
struct EventLogEntry {
    char peerName[32] = {};
    char eventType[EVENT_TYPE_LEN] = {};
    uint16_t payload = 0;
    uint32_t order = 0;
};

class EventLog {
   public:
    // 128 entries (~9.5KB) comfortably covers a multi-round buzzer session
    // with 10+ players; negligible against typical ESP32 free heap.
    static constexpr uint16_t CAPACITY = 128;

    using AppendCb = std::function<void(const EventLogEntry&)>;
    void setOnAppend(AppendCb cb) { _onAppend = cb; }

    using ClearCb = std::function<void()>;
    void setOnClear(ClearCb cb) { _onClear = cb; }

    void record(const char* peerName, const char* eventType, uint16_t payload) {
        EventLogEntry& e = _entries[_head];
        strlcpy(e.peerName, peerName, sizeof(e.peerName));
        strlcpy(e.eventType, eventType, sizeof(e.eventType));
        e.payload = payload;
        e.order = _nextOrder++;
        _head = (_head + 1) % CAPACITY;
        if (_count < CAPACITY) _count++;
        if (_onAppend) _onAppend(e);
    }

    // Wipes all stored entries (device-side "clear events" action) and
    // restarts arrival order at 0 — a fresh start, same as after a reboot.
    void clear() {
        _head = 0;
        _count = 0;
        _nextOrder = 0;
        if (_onClear) _onClear();
    }

    // Visits at most `limit` (0 = unlimited) of the most-recently recorded
    // matching entries, oldest-first, optionally restricted to a single
    // eventType ("" = no filter).
    void forEach(const char* eventTypeFilter, uint16_t limit,
                 const std::function<void(const EventLogEntry&)>& fn) const {
        uint16_t start = (_count < CAPACITY) ? 0 : _head;
        auto matches = [&](const EventLogEntry& e) {
            return !eventTypeFilter || !eventTypeFilter[0] ||
                   strcmp(e.eventType, eventTypeFilter) == 0;
        };
        uint16_t total = 0;
        for (uint16_t i = 0; i < _count; i++)
            if (matches(_entries[(start + i) % CAPACITY])) total++;
        uint16_t skip = (limit > 0 && total > limit) ? total - limit : 0;
        uint16_t seen = 0;
        for (uint16_t i = 0; i < _count; i++) {
            const EventLogEntry& e = _entries[(start + i) % CAPACITY];
            if (!matches(e)) continue;
            if (seen++ < skip) continue;
            fn(e);
        }
    }

   private:
    EventLogEntry _entries[CAPACITY];
    uint16_t _head = 0;
    uint16_t _count = 0;
    uint32_t _nextOrder = 0;
    AppendCb _onAppend;
    ClearCb _onClear;
};
