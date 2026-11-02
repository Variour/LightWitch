#pragma once
#include <stdint.h>

#include "FleetConfig.h"
#include "FleetTypes.h"

// Device states outside a game session (owner decisions, fleet protocol v0).
// Plain C++: the caller feeds the inputs on every loop() pass and carries out
// the state's effects and the sleep request.
//
//   Active   --USB power, no host data link-------------------> Charging
//   Charging --USB unplugged, or a host data link-------------> Active
//   Active   --on battery, kInactiveMs without activity--------> Standby
//   Standby  --key, invite, host link or USB (activity)--------> Active / Charging
//   Standby  --battery <= kStoragePercent----------------------> Sleep request
//
// Activity: a key press, a visible session invite, USB power, a host data
// link, a running update or factory test. Every state ends in Active while a
// game session owns the device or no charger answers (no USB or battery
// reading to act on).
namespace fleet {

class PowerState {
   public:
    struct Inputs {
        bool managed = false;   // a charger answers
        bool usb = false;       // charger power good
        bool hostLink = false;  // USB link traffic, or the USB-Serial-JTAG sees a host
        bool session = false;   // a game session owns the device
        bool hold = false;      // update or factory test running
        uint8_t battery = kNoBattery;
    };

    enum class Action : uint8_t { None, Sleep };

    void begin(uint32_t now) {
        _state = State::Active;
        _lastActivity = now;
    }

    // A key press, a visible invite or a fleet `ap` command.
    void activity(uint32_t now) { _lastActivity = now; }

    Action tick(const Inputs& in, uint32_t now) {
        if (!in.managed || in.session) {
            _lastActivity = now;
            _state = State::Active;
            return Action::None;
        }
        if (in.usb || in.hostLink || in.hold) _lastActivity = now;
        if (in.usb) {
            _state = in.hostLink ? State::Active : State::Charging;
            return Action::None;
        }
        // On battery.
        bool inactive = now - _lastActivity >= kInactiveMs;
        if (!inactive) {
            _state = State::Active;
            return Action::None;
        }
        _state = State::Standby;
        if (in.battery != kNoBattery && in.battery <= kStoragePercent) return Action::Sleep;
        return Action::None;
    }

    State state() const { return _state; }
    uint32_t lastActivity() const { return _lastActivity; }

   private:
    State _state = State::Active;
    uint32_t _lastActivity = 0;
};

// What a timer wake from sleep does next.
enum class WakeDecision : uint8_t {
    Boot,   // boot normally (USB present, or the charger did not answer)
    Sleep,  // back to sleep for kSleepWakeIntervalS
    Ship,   // ship mode: VBAT below kSleepShipMv, or kSleepToShipS in sleep
};

// readOk: the charger answered. vbatMv 0: no VBAT reading on battery (the
// charger refuses the ADC below its threshold, so the battery is low).
inline WakeDecision decideTimerWake(bool readOk, bool usb, uint16_t vbatMv, uint32_t sleptS) {
    if (!readOk || usb) return WakeDecision::Boot;
    if (vbatMv == 0 || vbatMv < kSleepShipMv) return WakeDecision::Ship;
    if (sleptS >= kSleepToShipS) return WakeDecision::Ship;
    return WakeDecision::Sleep;
}

}  // namespace fleet
