#pragma once
#include <stdint.h>
#include <string.h>

#include <new>

#include "GameFrames.h"

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#endif

namespace game {

// Hand-off of radio frames from the ESP-NOW receive callback (WiFi task) to
// loop(): the callback only copies into a fixed ring, loop() pops. A full
// ring drops the frame and counts it (gateway protocol v0, "Puffer im Gateway").
class RxRing {
   public:
    struct Frame {
        uint8_t len;
        int8_t rssi;
        uint8_t data[kMaxFrameLen];
    };

    ~RxRing() { delete[] _slots; }

    bool allocate(uint8_t capacity) {
        if (_slots) return true;
        Frame* slots = new (std::nothrow) Frame[capacity];
        if (!slots) return false;
        _lock();
        _slots = slots;
        _cap = capacity;
        _unlock();
        return true;
    }

    bool allocated() const { return _slots != nullptr; }

    // Radio task.
    void push(const uint8_t* d, size_t len, int8_t rssi) {
        if (len == 0 || len > kMaxFrameLen) return;
        _lock();
        if (!_slots || _count >= _cap) {
            _overflow++;
            _unlock();
            return;
        }
        Frame& f = _slots[(_head + _count) % _cap];
        memcpy(f.data, d, len);
        f.len = (uint8_t)len;
        f.rssi = rssi;
        _count++;
        _unlock();
    }

    // loop().
    bool pop(Frame& out) {
        _lock();
        if (_count == 0) {
            _unlock();
            return false;
        }
        out = _slots[_head];
        _head = (uint8_t)((_head + 1) % _cap);
        _count--;
        _unlock();
        return true;
    }

    uint32_t overflow() const { return _overflow; }

   private:
    Frame* _slots = nullptr;
    uint8_t _cap = 0;
    uint8_t _head = 0;
    uint8_t _count = 0;
    uint32_t _overflow = 0;  // written under the lock
#if defined(ESP_PLATFORM)
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    void _lock() { portENTER_CRITICAL(&_mux); }
    void _unlock() { portEXIT_CRITICAL(&_mux); }
#else
    void _lock() {}
    void _unlock() {}
#endif
};

}  // namespace game
