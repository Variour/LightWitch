#pragma once
#include <stdint.h>

// Every tunable of the fleet package (src/fleet/) in one place: device
// states, transport protection, charge target, fleet status and commands,
// maintenance AP, LED display and factory test. "owner" marks a value the
// project owner set, "spec" one from the fleet protocol (docs/usb-link.md,
// docs/mesh-compatibility.md), "review" one from the safety review,
// "decided" a project decision, "firmware" one chosen here.
//
// Build flags for a bring-up image with short times (defaults below):
//   -DFLEET_INACTIVE_MS=...       kInactiveMs
//   -DFLEET_WAKE_INACTIVE_MS=...  kWakeInactiveMs
//   -DFLEET_SLEEP_TO_SHIP_S=...   kSleepToShipS
//   -DFLEET_STORAGE_PERCENT=...   kStoragePercent
//   -DFLEET_STORAGE_MV=...        kStorageMv
#ifndef FLEET_INACTIVE_MS
#define FLEET_INACTIVE_MS (8UL * 60UL * 60UL * 1000UL)
#endif
#ifndef FLEET_WAKE_INACTIVE_MS
#define FLEET_WAKE_INACTIVE_MS (5UL * 60UL * 1000UL)
#endif
#ifndef FLEET_SLEEP_TO_SHIP_S
#define FLEET_SLEEP_TO_SHIP_S (7UL * 24UL * 60UL * 60UL)
#endif
#ifndef FLEET_STORAGE_PERCENT
#define FLEET_STORAGE_PERCENT 80
#endif
#ifndef FLEET_STORAGE_MV
#define FLEET_STORAGE_MV 4020
#endif

namespace fleet {

// ── Device states ────────────────────────────────────────────────────────────
// owner: on battery without key, session, USB or host link for 8 h -> standby.
// 8 h covers the breaks between talks on an event day.
static constexpr uint32_t kInactiveMs = FLEET_INACTIVE_MS;
// review: after a key wake from sleep, 5 min until standby, unless a further
// key press, an invite or session, USB or a host link follows (then 8 h).
static constexpr uint32_t kWakeInactiveMs = FLEET_WAKE_INACTIVE_MS;
// firmware: a key press within this time of the first loop pass was held at
// boot (the press that woke the device); like PowerManager's boot grace.
static constexpr uint32_t kBootPressGraceMs = 100;
// owner: standby lasts until the battery is down to the storage level, then sleep.
static constexpr uint8_t kStoragePercent = FLEET_STORAGE_PERCENT;
// firmware: the same level as VBAT, for a device without a battery
// percentage: 4.02 V is 80 % on BatteryMonitor's curve.
static constexpr uint16_t kStorageMv = FLEET_STORAGE_MV;
// firmware: at boot on USB, how long to wait for the USB-Serial-JTAG to see
// a host before booting into charging mode.
static constexpr uint32_t kHostDetectMs = 200;
// firmware: a key press that wakes the device (or that charging mode or a
// key test takes) triggers no button action. Button actions stay off until
// every key has been up this long: ButtonManager fires a short press
// DOUBLE_CLICK_WINDOW_MS (350 ms) after the release.
static constexpr uint32_t kKeyQuietMs = 500;
// owner (2026-11-10): Up and Down step the brightness at any time outside a
// game session: active, in charging mode, in standby and with the press that
// wakes the device. The fleet runs the key's own short-press step
// (config/BoardProfile.cpp) on the press and takes the key, so it steps once
// per press without waiting for ButtonManager. Down to 0 switches the LEDs
// off, Up from 0 on again. Except in charging mode (its display is the
// level) the battery level then shows for kBatteryOverlayMs at the new
// brightness. A participant in a session keeps Up and Down as answer keys.

// ── Sleep (ESP deep sleep) ───────────────────────────────────────────────────
static constexpr uint32_t kSleepWakeIntervalS = 60;  // owner: check USB and VBAT every 60 s
static constexpr uint32_t kSleepToShipS = FLEET_SLEEP_TO_SHIP_S;  // owner: 7 days -> ship mode
static constexpr uint16_t kSleepShipMv = 3500;    // owner: VBAT below ~3.5 V in sleep -> ship mode
static constexpr uint8_t kWakeReadTries = 3;      // review: charger reads at a timer wake
static constexpr uint32_t kWakeReadRetryMs = 10;  // firmware: pause between those reads
static constexpr uint32_t kSleepRetryMs = 1000;   // firmware: next try after a refused sleep

// ── Weak USB supply ──────────────────────────────────────────────────────────
// review: in charging mode, power good but the battery discharging (IBAT
// below -kWeakIbatMa) for kWeakSupplyMs: the supply is too weak. LEDs off,
// standby radio, error bit, until USB is unplugged.
static constexpr uint32_t kWeakSupplyMs = 60000;
static constexpr int16_t kWeakIbatMa = 40;  // firmware: above the IBAT noise around 0
// review: on USB with VBAT below kUsbSleepMv for kUsbSleepHoldMs (any
// state): deep sleep, the charger charges on. A timer wake on USB boots
// again from kUsbWakeMv (firmware: hysteresis).
static constexpr uint16_t kUsbSleepMv = 3300;
static constexpr uint32_t kUsbSleepHoldMs = 30000;  // firmware: like the low-battery cut-off
static constexpr uint16_t kUsbWakeMv = 3500;

// ── Charge target (charger VREG) ─────────────────────────────────────────────
// owner: ~80 %, 4.05 V; tune after measuring the real cells.
static constexpr uint16_t kStoreVregMv = 4050;
static constexpr uint16_t kEventVregMv = 4200;  // owner: 100 %
// VREG has 10 mV steps over 3500-4800 mV (SLUSEG4C Table 8-10); Li-ion stays
// at or below 4.20 V (checked against PowerManager in FleetManager.h).
static_assert(kStoreVregMv % 10 == 0 && kEventVregMv % 10 == 0, "VREG has 10 mV steps");
static_assert(kStoreVregMv >= 3500 && kStoreVregMv <= kEventVregMv && kEventVregMv <= 4200,
              "charge targets within 3500-4200 mV");
// review: lowering the target below VBAT on USB trips the charger's battery
// over-voltage protection (VBAT_OVP 103-105 % of VREG, §7.5) until VBAT has
// come down. Expected up to the event target plus this margin.
static constexpr uint16_t kVregOvpMarginMv = 50;

// ── FleetStatus beacon ───────────────────────────────────────────────────────
static constexpr uint32_t kStatusPeriodMs = 30000;   // spec: about every 30 s
static constexpr uint32_t kStatusJitterMs = 3000;    // firmware: period +- jitter
static constexpr uint32_t kStatusFirstMaxMs = 3000;  // firmware: first beacon 0-3 s after boot
static constexpr uint32_t kStatusMinGapMs = 1000;    // firmware: coalesce faster changes
static constexpr uint32_t kStatusProgressMs = 3000;  // firmware: progress beacons during an update

// ── FleetCommand ─────────────────────────────────────────────────────────────
static constexpr uint8_t kCommandRepeats = 3;  // spec: every command goes out 3 times
// firmware: gap between the repeats, drawn per repeat. Spread over about one
// standby wake interval so a standby device catches at least one.
static constexpr uint32_t kCommandGapMinMs = 300;
static constexpr uint32_t kCommandGapMaxMs = 700;
static constexpr uint8_t kCommandSlots = 4;      // firmware: commands in flight on the bridge
static constexpr uint8_t kMaxTargets = 32;       // firmware: MACs per command (0 = all devices)
static constexpr uint8_t kNonceMemory = 8;       // firmware: remembered nonces per device
static constexpr uint32_t kNonceHoldMs = 10000;  // firmware: a nonce counts as seen this long
// owner (2026-11-10): the console sets the brightness of every Buzzerboard
// (fleet `brightness`, 0-255 like every LightWitch brightness): light 0's
// brightness override, stored, as Up and Down set it, so it overrides the
// room's (group's) brightness; the keys go on from there. It also applies in
// a game session: there the bridge sends the command on the session channel
// (MeshManager lets FleetCommand through quiet mode) and the devices act on
// no other op. A Buzzerboard bridge applies it to itself at once.

// ── Bridge: fleet table ──────────────────────────────────────────────────────
static constexpr uint16_t kMaxFleetDevices = 128;  // firmware: the oldest entry makes room
static constexpr uint8_t kRxQueue = 16;            // firmware: fleet frames between loop() passes

// ── Maintenance AP ───────────────────────────────────────────────────────────
static constexpr uint32_t kApComboMs = 3000;      // owner: L1 + R1 held 3 s
static constexpr uint8_t kApMinutesDefault = 10;  // owner: 10 min
static constexpr uint8_t kApMinutesMax = 60;      // firmware: longest `config ap`

// ── LED display (charging mode, update) ──────────────────────────────────────
// owner (2026-11-09): every display follows the brightness light 0 renders at
// (its brightness override, else its group's brightness), at least kOffLevel.
// owner (display sketch): a full device shows the column for 1 h, then only
// the status dot.
static constexpr uint32_t kFullColumnMs = 60UL * 60UL * 1000UL;
static constexpr uint32_t kTipPulseMs = 2000;  // firmware: slow pulse of the column tip
// owner (2026-11-10): every level on the column runs red at LED1 to green
// at the USB-C end, as with the LEDs switched off. While the battery
// charges, the level shows at kChargeFillPercent of lvl (at least
// kOffLevel), and only its last kChargeGlowLeds LEDs glow up together per
// kTipGlowMs to kChargeGlowPercent of lvl (at least kOffTipLevel). A full
// column shows at the fill level.
static constexpr int16_t kFlowMinMa = 20;          // firmware: charging from this current
static constexpr uint8_t kChargeFillPercent = 20;  // owner (2026-11-10)
static constexpr uint8_t kChargeGlowPercent = 40;  // owner (2026-11-10)
static constexpr uint8_t kChargeGlowLeds = 3;      // owner (2026-11-10)
// owner (2026-11-09): when active and idle, one breath of the idle pattern,
// then the level on the column, in turn. kIdlePulseMs matches the idle
// pattern's period (config/BoardProfile.cpp, Breathing at speed 1.0).
static constexpr uint32_t kIdlePulseMs = 4000;
static constexpr uint32_t kIdleLevelMs = 3000;  // firmware
// owner (2026-11-09): LEDs switched off with a key, on USB: only the column
// shows the level at about 1 %, filling from LED1, red at LED1 to green at
// the USB-C end.
// While charging, the last LED of the level pulses slowly. 1/255 is the
// smallest step of the LEDs.
static constexpr uint8_t kOffLevel = 2;       // about 0.8 %
static constexpr uint8_t kOffTipLevel = 5;    // firmware: pulse peak, about 2 %
static constexpr uint32_t kTipGlowMs = 3000;  // firmware: one glow of the last LED
// owner (2026-11-09): the bottom-right side key (R2) shows the battery level
// on the column for this long, active and outside a session.
static constexpr uint32_t kBatteryOverlayMs = 3000;  // firmware
// owner (2026-11-09): any device can be the gateway. While it is, a soft white
// pulse runs from the USB-C end down the column over its own colours, once
// per kGatewayFlowMs.
static constexpr uint32_t kGatewayFlowMs = 2500;  // firmware
static constexpr uint32_t kFaultBlinkMs = 1000;   // firmware: ring blinks at 1 Hz
static constexpr uint32_t kRingChaseMs = 150;     // firmware: update ring, one step per 150 ms

// ── Standby radio ────────────────────────────────────────────────────────────
// firmware: ESP-NOW listens kStandbyWakeWindowMs of every kStandbyWakeIntervalMs.
static constexpr uint16_t kStandbyWakeIntervalMs = 1000;
static constexpr uint16_t kStandbyWakeWindowMs = 500;

// ── Maintenance AP display ───────────────────────────────────────────────────
static constexpr uint32_t kApPulseMs = 2000;  // decided: status dot pulses blue slowly

// ── USB `config wifi` ────────────────────────────────────────────────────────
static constexpr uint8_t kWifiNetworksMax =
    5;                                   // = MAX_WIFI_NETWORKS (Config.h, checked in FleetManager)
static constexpr uint8_t kSsidMax = 32;  // IEEE 802.11: SSID of up to 32 bytes
static constexpr uint8_t kPassMin = 8;   // WPA2 passphrase: 8-63 characters, or "" (open)
static constexpr uint8_t kPassMax = 63;

// ── USB `config token` ───────────────────────────────────────────────────────
static constexpr uint8_t kTokenMax = 127;  // = sizeof(DeviceConfig::githubToken) - 1

// ── USB `config name` ────────────────────────────────────────────────────────
// owner (2026-11-09): the production script names each Buzzerboard
// "buzzer-<number>-<animal>". One DNS label: a-z, 0-9 and '-'.
static constexpr uint8_t kNameMax = 31;  // = sizeof(DeviceConfig::deviceName) - 1

// ── Factory test (USB `test`) ────────────────────────────────────────────────
// owner (2026-11-09): the test frames follow the display brightness, at
// least kTestLevelMin so every colour stays easy to judge.
static constexpr uint8_t kTestLevelMin = 8;
static constexpr uint32_t kTestColorMs = 600;        // firmware: each colour on all LEDs
static constexpr uint32_t kTestChaseStepMs = 60;     // firmware: chase, per LED
static constexpr uint32_t kTestChargeMs = 10000;     // spec: charge for 10 s
static constexpr uint32_t kTestAdcIntervalMs = 300;  // firmware: charger ADC during the test
static constexpr uint32_t kTestRebootDelayMs = 300;  // firmware: reply first, then restart

}  // namespace fleet
