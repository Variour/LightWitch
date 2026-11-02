#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <esp_attr.h>
#include <esp_sleep.h>

#include "../power/PowerManager.h"
#include "FleetConfig.h"
#include "PowerState.h"

// ESP deep sleep of the transport protection (src/fleet/PowerState.h). Wake:
// the LP-GPIO keys (ext1, any low), and a timer every kSleepWakeIntervalS that
// checks USB and VBAT through the charger and decides between boot, sleep
// and ship mode before anything else starts. What that check needs survives
// in RTC memory.
namespace fleet {

struct SleepMemory {
    uint32_t magic;
    uint32_t sleptS;    // time in sleep so far, counted in timer wakes
    uint8_t sda, scl;   // device I2C bus (the charger)
    uint64_t wakePins;  // GPIO mask of the wake keys (active low)
};
static constexpr uint32_t kSleepMagic = 0x464C5331;  // "FLS1"

// Zero at power-on, kept across deep sleep. One translation unit (main.cpp)
// includes this header.
RTC_DATA_ATTR static SleepMemory sSleepMemory;

class DeepSleep {
   public:
    // First thing in setup(), before Serial, LittleFS and Wi-Fi. On a timer
    // wake from sleep it goes back to sleep or into ship mode; it returns
    // when the device boots normally (USB, a key, a reset, or a charger that
    // does not answer).
    static void handleWake() {
        SleepMemory& m = sSleepMemory;
        if (m.magic != kSleepMagic) return;
        if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
            m.magic = 0;
            return;
        }
        m.sleptS += kSleepWakeIntervalS;
        Wire.begin(m.sda, m.scl);
        PowerManager::WakeReading r = PowerManager::readForWake();
        WakeDecision d = decideTimerWake(r.ok, r.powerGood, r.vbatMv, m.sleptS);
        if (d == WakeDecision::Sleep) {
            Wire.end();
            _sleep();
        }
        // Ship mode cuts the power 25 ms after the request; PowerManager
        // withdraws it if USB arrived meanwhile and the device boots.
        if (d == WakeDecision::Ship) PowerManager::shipForWake();
        Wire.end();
        m.magic = 0;
    }

    // Enters sleep; does not return. The caller has switched the LED rail
    // off (PowerManager::prepareDeepSleep).
    static void enter(uint8_t sda, uint8_t scl, uint64_t wakePins) {
        SleepMemory& m = sSleepMemory;
        m.magic = kSleepMagic;
        m.sleptS = 0;
        m.sda = sda;
        m.scl = scl;
        m.wakePins = wakePins;
        _sleep();
    }

   private:
    static void _sleep() {
#if !CONFIG_IDF_TARGET_ESP32  // the ESP32's ext1 has no "any low" mode
        uint64_t mask = 0;
        // A key held down (e.g. by the case) would wake the chip at once:
        // leave it out, the timer still runs.
        for (uint8_t pin = 0; pin < 64; pin++) {
            if (!((sSleepMemory.wakePins >> pin) & 1)) continue;
            pinMode(pin, INPUT);
            if (digitalRead(pin) == HIGH) mask |= 1ULL << pin;
        }
        if (mask) esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
        esp_sleep_enable_timer_wakeup((uint64_t)kSleepWakeIntervalS * 1000000ULL);
        esp_deep_sleep_start();
    }
};

}  // namespace fleet
