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
//   Active   --on battery, inactive for the timeout-----------> Standby
//   Standby  --key, invite, host link or USB (activity)--------> Active / Charging
//   Standby  --battery <= kStoragePercent----------------------> Sleep request
//
// Timeout: kInactiveMs, or kWakeInactiveMs after a key wake from sleep until
// a real interaction follows (a further key press, an invite or session,
// USB, a host link). Activity that is no interaction (a running update or
// factory test, a fleet `ap`) restarts the timeout without ending the short
// one. Every state ends in Active while a game session owns the device or
// no charger answers (no USB or battery reading to act on).
namespace fleet {

class PowerState {
   public:
    struct Inputs {
        bool managed = false;   // a charger answers
        bool usb = false;       // charger power good
        bool hostLink = false;  // USB link traffic, or the USB-Serial-JTAG sees a host
        bool session = false;   // a game session owns the device
        bool hold = false;      // update, OTA or factory test running
        uint8_t battery = kNoBattery;
        uint16_t vbatMv = 0;  // 0 = unknown; the sleep check without a percentage
    };

    enum class Action : uint8_t { None, Sleep };

    // keyWake: booted by a key from sleep; the short timeout applies.
    void begin(uint32_t now, bool keyWake = false) {
        _state = State::Active;
        _lastActivity = now;
        _short = keyWake;
    }

    // Keeps the device awake (fleet `ap`); a short timeout stays short.
    void activity(uint32_t now) { _lastActivity = now; }

    // A key press or a visible invite: awake, and the normal timeout.
    void interaction(uint32_t now) {
        _lastActivity = now;
        _short = false;
    }

    Action tick(const Inputs& in, uint32_t now) {
        if (!in.managed || in.session) {
            interaction(now);
            _state = State::Active;
            return Action::None;
        }
        if (in.usb || in.hostLink) interaction(now);
        if (in.hold) _lastActivity = now;
        if (in.usb) {
            _state = in.hostLink ? State::Active : State::Charging;
            return Action::None;
        }
        // On battery.
        uint32_t timeout = _short ? kWakeInactiveMs : kInactiveMs;
        if (now - _lastActivity < timeout) {
            _state = State::Active;
            return Action::None;
        }
        _state = State::Standby;
        bool atStorage = in.battery != kNoBattery ? in.battery <= kStoragePercent
                                                  : in.vbatMv != 0 && in.vbatMv <= kStorageMv;
        return atStorage ? Action::Sleep : Action::None;
    }

    State state() const { return _state; }
    uint32_t lastActivity() const { return _lastActivity; }
    // Still on the short timeout of a key wake.
    bool shortWake() const { return _short; }

   private:
    State _state = State::Active;
    uint32_t _lastActivity = 0;
    bool _short = false;
};

// The USB supply in charging mode. weak(): power good, but the battery has
// been discharging (IBAT below -kWeakIbatMa) for kWeakSupplyMs; latched until
// USB is unplugged or charging mode ends. sleep(): on USB with VBAT below
// kUsbSleepMv for kUsbSleepHoldMs, in any state.
class UsbSupply {
   public:
    // Every loop() pass. sample: a new charger ADC reading came in.
    void tick(bool usb, bool charging, bool sample, bool ibatValid, int16_t ibatMa, uint16_t vbatMv,
              uint32_t now) {
        if (!usb) {
            _weak = false;
            _discharging = false;
            _low = false;
            return;
        }
        if (!charging) {
            _weak = false;
            _discharging = false;
        } else if (sample && ibatValid && !_weak) {
            if (ibatMa < -kWeakIbatMa) {
                if (!_discharging) {
                    _discharging = true;
                    _dischargingSince = now;
                }
                if (now - _dischargingSince >= kWeakSupplyMs) _weak = true;
            } else {
                _discharging = false;
            }
        }
        if (vbatMv == 0 || vbatMv >= kUsbSleepMv) {
            _low = false;
        } else if (!_low) {
            _low = true;
            _lowSince = now;
        }
    }

    bool weak() const { return _weak; }
    bool sleep(uint32_t now) const { return _low && now - _lowSince >= kUsbSleepHoldMs; }

   private:
    bool _weak = false;
    bool _discharging = false;
    uint32_t _dischargingSince = 0;
    bool _low = false;
    uint32_t _lowSince = 0;
};

// What a timer wake from sleep does next.
enum class WakeDecision : uint8_t {
    Boot,   // boot normally
    Sleep,  // back to sleep for kSleepWakeIntervalS
    Ship,   // ship mode: VBAT below kSleepShipMv, or kSleepToShipS in sleep
};

// readOk: the charger answered. vbatMv 0: no VBAT reading (on battery the
// charger refuses the ADC below its threshold, so the battery is low). On
// USB the device boots, unless VBAT is below kUsbWakeMv: then it sleeps on
// while the charger charges.
inline WakeDecision decideTimerWake(bool readOk, bool usb, uint16_t vbatMv, uint32_t sleptS) {
    if (!readOk) return WakeDecision::Boot;
    if (usb) return vbatMv != 0 && vbatMv < kUsbWakeMv ? WakeDecision::Sleep : WakeDecision::Boot;
    if (vbatMv == 0 || vbatMv < kSleepShipMv) return WakeDecision::Ship;
    if (sleptS >= kSleepToShipS) return WakeDecision::Ship;
    return WakeDecision::Sleep;
}

}  // namespace fleet
