#pragma once
#include <stdint.h>
#include <string.h>

#include "FleetConfig.h"

// Shared enums and pure helpers of the fleet package. Plain C++ with no
// Arduino dependency, so the state machines and frames build on the host.
namespace fleet {

// The device's own colours (Timo, 2026-11-09, `config look`): a gradient of
// 2 to kLookStopsMax stops on the column, by LED index from LED1 (0) to the
// USB-C end (23), positions ascending. count 0: no own colours.
static constexpr uint8_t kLookStopsMax = 4;
struct Look {
    uint8_t count = 0;
    uint8_t pos[kLookStopsMax] = {};
    uint8_t rgb[kLookStopsMax][3] = {};
};

// Device state outside a game session (owner decision, fleet protocol v0).
// Sleep and ship mode are not running states: the device is off then.
enum class State : uint8_t {
    Active = 0,    // normal operation
    Charging = 1,  // USB power without a host data link: charges or waits, keys do nothing
    Standby = 2,   // on battery, inactive for kInactiveMs: LEDs off, radio low duty
};
static constexpr const char* kStateNames[] = {"active", "charging", "standby"};

// Charge status from the charger. None: the device has no charger.
enum class Charge : uint8_t { None = 0, Charging = 1, Full = 2, Discharging = 3, Fault = 4 };
static constexpr const char* kChargeNames[] = {"none", "charging", "full", "discharging", "fault"};

// Charge target: charger VREG kStoreVregMv or kEventVregMv.
enum class Target : uint8_t { Store = 0, Event = 1 };
static constexpr const char* kTargetNames[] = {"store", "event"};

// Progress of a fleet update. Installing: the image is written, the device
// restarts.
enum class Update : uint8_t { Idle = 0, Checking = 1, Downloading = 2, Installing = 3, Failed = 4 };
static constexpr const char* kUpdateNames[] = {"idle", "checking", "downloading", "installing",
                                               "failed"};

// Why the last fleet update failed.
enum class UpdateError : uint8_t {
    None = 0,
    NoWifi = 1,      // no WiFi network configured
    WifiFailed = 2,  // no configured network reachable
    LowBattery = 3,  // on battery below 3.5 V, or no battery reading
    Failed = 4,      // release check, download or flash failed
};
static constexpr const char* kUpdateErrorNames[] = {"none", "no_wifi", "wifi_failed", "low_battery",
                                                    "failed"};

// FleetStatus.errors bits.
static constexpr uint16_t kErrChargerOffline = 1u << 0;  // charger configured, not answering
static constexpr uint16_t kErrTsCold = 1u << 1;          // battery NTC cold: no charging
static constexpr uint16_t kErrTsHot = 1u << 2;           // battery NTC hot: no charging
static constexpr uint16_t kErrThermal = 1u << 3;         // charger thermal shutdown
static constexpr uint16_t kErrBattery = 1u << 4;         // battery over-current or over-voltage
static constexpr uint16_t kErrSafetyTimer = 1u << 5;     // charge safety timer expired
static constexpr uint16_t kErrSupply = 1u << 6;          // VBUS or VSYS fault
static constexpr uint16_t kErrTsBias = 1u << 7;          // NTC bias fault (no NTC, open TS)
static constexpr uint16_t kErrLowBattery = 1u << 8;      // on battery below 3.5 V
static constexpr uint16_t kErrWeakSupply = 1u << 9;      // charging mode: USB cannot hold the load

// FleetStatus.flags bits.
static constexpr uint8_t kFlagUsb = 1u << 0;   // USB power present (charger power good)
static constexpr uint8_t kFlagWifi = 1u << 1;  // at least one WiFi network configured

static constexpr uint8_t kNoBattery = 0xFF;  // battery percent unknown

// What the charger reports, as plain facts (filled from Bq25628::Status).
struct ChargerFacts {
    bool enabled = false;          // the device has a charger (DeviceConfig::chargerEnabled)
    bool online = false;           // it answers and is configured
    bool powerGood = false;        // VBUS_STAT = adapter
    bool charging = false;         // CHG_STAT != 0 (trickle, pre, fast, taper or top-off)
    uint8_t tsZone = 0;            // TS_STAT, BQ25628 SLUSEG4C Table 8-28
    bool thermalShutdown = false;  // TSHUT_STAT
    bool batteryFault = false;     // BAT_FAULT_STAT
    bool supplyFault = false;      // VBUS_FAULT_STAT or SYS_FAULT_STAT
    bool safetyTimer = false;      // SAFETY_TMR_STAT, Table 8-26
    bool lowBattery = false;       // on battery below 3.5 V (PowerManager::UPDATE_MIN_MV)
    bool weakSupply = false;       // WeakSupply::weak()
    uint16_t vbatMv = 0;           // 0 = unknown
    uint16_t vregMv = 0;           // the charge voltage set now
};

// BAT_FAULT on USB with VBAT above VREG but within the event target: the
// battery over-voltage protection after the target was lowered (e.g. event
// -> store). Charging resumes once VBAT is below VBAT_OVPZ; no error.
inline bool batteryOvpExpected(const ChargerFacts& f) {
    return f.powerGood && f.batteryFault && f.vbatMv != 0 && f.vregMv != 0 && f.vbatMv > f.vregMv &&
           f.vbatMv <= kEventVregMv + kVregOvpMarginMv;
}

// TS_STAT zones (Table 8-28): 001 cold, 010 hot, 111 bias reference fault.
// Cool, warm, pre-cool and pre-warm only reduce the charge current or voltage.
static constexpr uint8_t kTsCold = 1;
static constexpr uint8_t kTsHot = 2;
static constexpr uint8_t kTsBiasFault = 7;

inline uint16_t chargerErrors(const ChargerFacts& f) {
    if (!f.enabled) return 0;
    if (!f.online) return kErrChargerOffline;
    uint16_t e = 0;
    if (f.tsZone == kTsCold) e |= kErrTsCold;
    if (f.tsZone == kTsHot) e |= kErrTsHot;
    if (f.tsZone == kTsBiasFault) e |= kErrTsBias;
    if (f.thermalShutdown) e |= kErrThermal;
    if (f.batteryFault && !batteryOvpExpected(f)) e |= kErrBattery;
    if (f.safetyTimer) e |= kErrSafetyTimer;
    if (f.supplyFault) e |= kErrSupply;
    if (f.lowBattery && !f.powerGood) e |= kErrLowBattery;
    if (f.weakSupply && f.powerGood) e |= kErrWeakSupply;
    return e;
}

// Errors that stop charging and turn the display red.
static constexpr uint16_t kChargeFaultMask = kErrChargerOffline | kErrTsCold | kErrTsHot |
                                             kErrThermal | kErrBattery | kErrSafetyTimer |
                                             kErrSupply | kErrTsBias;

inline Charge chargeStatus(const ChargerFacts& f) {
    if (!f.enabled) return Charge::None;
    if (!f.online) return Charge::Fault;
    if (!f.powerGood) return Charge::Discharging;
    if (chargerErrors(f) & kChargeFaultMask) return Charge::Fault;
    return f.charging ? Charge::Charging : Charge::Full;
}

inline int nameIndex(const char* s, const char* const* names, uint8_t count) {
    if (!s) return -1;
    for (uint8_t i = 0; i < count; i++)
        if (strcmp(s, names[i]) == 0) return i;
    return -1;
}

}  // namespace fleet
