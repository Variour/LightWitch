#pragma once
#include <stdint.h>
#include <string.h>

#include <functional>
#include <new>

#include "FleetConfig.h"
#include "FleetFrames.h"
#include "FleetTypes.h"

// Radio side of the fleet package: when a device sends its FleetStatus,
// which FleetCommands it acts on, how a bridge repeats a command, and the
// bridge's table of the latest status per device. Plain C++.
namespace fleet {

// When to send FleetStatus: every kStatusPeriodMs +- kStatusJitterMs, at once
// (at most every kStatusMinGapMs) when a reported field other than the
// battery changes, and every kStatusProgressMs while the update percentage
// moves.
class StatusBeacon {
   public:
    using RandomFn = std::function<uint32_t(uint32_t lo, uint32_t hi)>;

    struct Snapshot {
        State state = State::Active;
        Charge charge = Charge::None;
        Target target = Target::Store;
        bool usb = false;
        Update update = Update::Idle;
        uint8_t updatePct = 0;
        uint16_t errors = 0;
        uint16_t number = 0;

        bool sameKey(const Snapshot& o) const {
            return state == o.state && charge == o.charge && target == o.target && usb == o.usb &&
                   update == o.update && errors == o.errors && number == o.number;
        }
    };

    void begin(RandomFn rnd, uint32_t now) {
        _rnd = rnd;
        _next = now + _draw(0, kStatusFirstMaxMs);
        _sentOnce = false;
    }

    // True if a FleetStatus goes out now; records it as sent.
    bool tick(const Snapshot& s, uint32_t now) {
        bool due = (int32_t)(now - _next) >= 0;
        bool gapOk = !_sentOnce || now - _lastSent >= kStatusMinGapMs;
        bool changed = _sentOnce && !s.sameKey(_last);
        bool progress = _sentOnce && s.update == Update::Downloading &&
                        s.updatePct != _last.updatePct && now - _lastSent >= kStatusProgressMs;
        if (!(due || (changed && gapOk) || progress)) return false;
        _last = s;
        _lastSent = now;
        _sentOnce = true;
        _next = now + kStatusPeriodMs - kStatusJitterMs + _draw(0, 2 * kStatusJitterMs);
        return true;
    }

   private:
    RandomFn _rnd;
    Snapshot _last;
    uint32_t _next = 0;
    uint32_t _lastSent = 0;
    bool _sentOnce = false;

    uint32_t _draw(uint32_t lo, uint32_t hi) { return _rnd ? _rnd(lo, hi) : lo; }
};

// Device side: acts on each command once (nonce) and, when the command
// lists MACs, only if its own MAC is among them.
class CommandFilter {
   public:
    void begin(const uint8_t* ownMac) { memcpy(_mac, ownMac, 6); }

    bool accept(const FleetCommandMsg& m, uint32_t now) {
        if (m.targetCount > 0) {
            bool mine = false;
            for (uint8_t i = 0; i < m.targetCount && !mine; i++)
                mine = memcmp(m.targets[i], _mac, 6) == 0;
            if (!mine) return false;
        }
        for (const Seen& s : _seen)
            if (s.used && s.nonce == m.nonce && now - s.at < kNonceHoldMs) return false;
        Seen& slot = _seen[_next];
        _next = (uint8_t)((_next + 1) % kNonceMemory);
        slot.used = true;
        slot.nonce = m.nonce;
        slot.at = now;
        return true;
    }

   private:
    struct Seen {
        bool used = false;
        uint32_t nonce = 0;
        uint32_t at = 0;
    };
    uint8_t _mac[6] = {};
    Seen _seen[kNonceMemory];
    uint8_t _next = 0;
};

// Bridge side: sends each queued command kCommandRepeats times, the first
// copy on the next tick and the others kCommandGapMinMs-kCommandGapMaxMs apart.
class CommandSender {
   public:
    using SendFn = std::function<void(const uint8_t*, size_t)>;
    using RandomFn = std::function<uint32_t(uint32_t lo, uint32_t hi)>;

    void begin(SendFn send, RandomFn rnd) {
        _send = send;
        _rnd = rnd;
    }

    // False when kCommandSlots commands are still being repeated.
    bool queue(const FleetCommandMsg& m, uint32_t now) {
        for (Slot& s : _slots) {
            if (s.left > 0) continue;
            s.msg = m;
            s.left = kCommandRepeats;
            s.due = now;
            return true;
        }
        return false;
    }

    void tick(uint32_t now) {
        for (Slot& s : _slots) {
            if (s.left == 0 || (int32_t)(now - s.due) < 0) continue;
            if (_send) _send((const uint8_t*)&s.msg, commandFrameLen(s.msg));
            s.left--;
            s.due = now + (_rnd ? _rnd(kCommandGapMinMs, kCommandGapMaxMs) : kCommandGapMinMs);
        }
    }

    bool idle() const {
        for (const Slot& s : _slots)
            if (s.left > 0) return false;
        return true;
    }

   private:
    struct Slot {
        FleetCommandMsg msg;
        uint8_t left = 0;
        uint32_t due = 0;
    };
    Slot _slots[kCommandSlots];
    SendFn _send;
    RandomFn _rnd;
};

// Bridge side: the latest FleetStatus per device MAC, for the USB link's
// `fleet status` events (latest value per device, like `game value`).
class FleetTable {
   public:
    struct Entry {
        bool used;
        bool dirty;
        uint8_t mac[6];
        int8_t rssi;
        uint32_t heardMs;
        FleetStatusMsg st;
    };

    ~FleetTable() { delete[] _entries; }

    bool allocate() {
        if (_entries) return true;
        _entries = new (std::nothrow) Entry[kMaxFleetDevices];
        if (!_entries) return false;
        memset(_entries, 0, sizeof(Entry) * kMaxFleetDevices);
        return true;
    }
    bool allocated() const { return _entries != nullptr; }

    void onStatus(const uint8_t* mac, const FleetStatusMsg& st, int8_t rssi, uint32_t now) {
        if (!_entries) return;
        Entry* e = _find(mac);
        if (!e) e = _free();
        if (!e) e = _oldest();
        if (!e->used || memcmp(e->mac, mac, 6) != 0) {
            memset(e, 0, sizeof(Entry));
            memcpy(e->mac, mac, 6);
            e->used = true;
            if (_count < kMaxFleetDevices) _count++;
        }
        e->st = st;
        e->rssi = rssi;
        e->heardMs = now;
        if (!e->dirty) _dirty++;
        e->dirty = true;
    }

    void markAllDirty() {
        if (!_entries) return;
        _dirty = 0;
        for (uint16_t i = 0; i < kMaxFleetDevices; i++) {
            _entries[i].dirty = _entries[i].used;
            if (_entries[i].dirty) _dirty++;
        }
    }

    uint16_t dirtyCount() const { return _dirty; }
    uint16_t count() const { return _count; }

    // Next dirty entry after the last one returned (round robin), or nullptr.
    Entry* nextDirty() {
        if (!_entries || _dirty == 0) return nullptr;
        for (uint16_t k = 0; k < kMaxFleetDevices; k++) {
            _cursor = (uint16_t)((_cursor + 1) % kMaxFleetDevices);
            if (_entries[_cursor].dirty) return &_entries[_cursor];
        }
        return nullptr;
    }

    void clearDirty(Entry* e) {
        if (!e->dirty) return;
        e->dirty = false;
        _dirty--;
    }

    const Entry* find(const uint8_t* mac) const {
        if (!_entries) return nullptr;
        for (uint16_t i = 0; i < kMaxFleetDevices; i++)
            if (_entries[i].used && memcmp(_entries[i].mac, mac, 6) == 0) return &_entries[i];
        return nullptr;
    }

   private:
    Entry* _entries = nullptr;
    uint16_t _dirty = 0;
    uint16_t _count = 0;
    uint16_t _cursor = kMaxFleetDevices - 1;

    Entry* _find(const uint8_t* mac) { return const_cast<Entry*>(find(mac)); }

    Entry* _free() {
        for (uint16_t i = 0; i < kMaxFleetDevices; i++)
            if (!_entries[i].used) return &_entries[i];
        return nullptr;
    }

    Entry* _oldest() {
        Entry* o = &_entries[0];
        for (uint16_t i = 1; i < kMaxFleetDevices; i++)
            if ((int32_t)(_entries[i].heardMs - o->heardMs) < 0) o = &_entries[i];
        if (o->dirty) {
            o->dirty = false;
            _dirty--;
        }
        return o;
    }
};

}  // namespace fleet
