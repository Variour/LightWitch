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

    // Visits stored entries oldest-first, optionally restricted to a single
    // eventType ("" = no filter).
    void forEach(const char* eventTypeFilter,
                 const std::function<void(const EventLogEntry&)>& fn) const {
        uint16_t start = (_count < CAPACITY) ? 0 : _head;
        for (uint16_t i = 0; i < _count; i++) {
            const EventLogEntry& e = _entries[(start + i) % CAPACITY];
            if (eventTypeFilter && eventTypeFilter[0] && strcmp(e.eventType, eventTypeFilter) != 0)
                continue;
            fn(e);
        }
    }

   private:
    EventLogEntry _entries[CAPACITY];
    uint16_t _head = 0;
    uint16_t _count = 0;
    uint32_t _nextOrder = 0;
    AppendCb _onAppend;
};
