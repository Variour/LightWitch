#pragma once
#include <stdint.h>

// Every tunable of the fleet package (src/fleet/) in one place: device
// states, transport protection, charge target, fleet status and commands,
// maintenance AP, LED display and factory test. "owner" marks a value the
// project owner set, "spec" one from the fleet protocol (docs/usb-link.md,
// docs/mesh-compatibility.md), "firmware" one chosen here.
namespace fleet {

// ── Device states ────────────────────────────────────────────────────────────
// owner: on battery without key, session, USB or host link for 8 h -> standby.
// 8 h covers the breaks between talks on an event day.
static constexpr uint32_t kInactiveMs = 8UL * 60UL * 60UL * 1000UL;
// owner: standby lasts until the battery is down to the storage level, then sleep.
static constexpr uint8_t kStoragePercent = 80;
// firmware: at boot on USB, how long to wait for the USB-Serial-JTAG to see
// a host before booting into charging mode.
static constexpr uint32_t kHostDetectMs = 200;
// firmware: a key press that wakes the device (or that charging mode or a
// key test takes) triggers no button action. Button actions stay off until
// every key has been up this long: ButtonManager fires a short press
// DOUBLE_CLICK_WINDOW_MS (350 ms) after the release.
static constexpr uint32_t kKeyQuietMs = 500;

// ── Sleep (ESP deep sleep) ───────────────────────────────────────────────────
static constexpr uint32_t kSleepWakeIntervalS = 60;  // owner: check USB and VBAT every 60 s
static constexpr uint32_t kSleepToShipS = 7UL * 24UL * 60UL * 60UL;  // owner: 7 days -> ship mode
static constexpr uint16_t kSleepShipMv = 3500;  // owner: VBAT below ~3.5 V in sleep -> ship mode

// ── Charge target (charger VREG) ─────────────────────────────────────────────
// owner: ~80 %, 4.05 V; tune after measuring the real cells.
static constexpr uint16_t kStoreVregMv = 4050;
static constexpr uint16_t kEventVregMv = 4200;  // owner: 100 %

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

// ── Bridge: fleet table ──────────────────────────────────────────────────────
static constexpr uint16_t kMaxFleetDevices = 128;  // firmware: the oldest entry makes room
static constexpr uint8_t kRxQueue = 16;            // firmware: fleet frames between loop() passes

// ── Maintenance AP ───────────────────────────────────────────────────────────
static constexpr uint32_t kApComboMs = 3000;      // owner: L1 + R1 held 3 s
static constexpr uint8_t kApMinutesDefault = 10;  // owner: 10 min
static constexpr uint8_t kApMinutesMax = 60;      // firmware: longest `config ap`

// ── LED display (charging mode, update) ──────────────────────────────────────
static constexpr uint8_t kDisplayLevel = 26;      // firmware: low brightness, about 10 %
static constexpr uint8_t kRedBelowPercent = 30;   // firmware: red below, yellow from here
static constexpr uint8_t kGreenFromPercent = 80;  // firmware: green from the storage level
// owner (display sketch): a full device shows the column for 1 h, then only
// the status dot.
static constexpr uint32_t kFullColumnMs = 60UL * 60UL * 1000UL;
static constexpr uint32_t kTipPulseMs = 2000;    // firmware: slow pulse of the column tip
static constexpr uint32_t kFaultBlinkMs = 1000;  // firmware: ring blinks at 1 Hz
static constexpr uint32_t kRingChaseMs = 150;    // firmware: update ring, one step per 150 ms

// ── Standby radio ────────────────────────────────────────────────────────────
// firmware: ESP-NOW listens kStandbyWakeWindowMs of every kStandbyWakeIntervalMs.
static constexpr uint16_t kStandbyWakeIntervalMs = 1000;
static constexpr uint16_t kStandbyWakeWindowMs = 500;

// ── USB `config wifi` ────────────────────────────────────────────────────────
static constexpr uint8_t kWifiNetworksMax =
    5;                                   // = MAX_WIFI_NETWORKS (Config.h, checked in FleetManager)
static constexpr uint8_t kSsidMax = 32;  // IEEE 802.11: SSID of up to 32 bytes
static constexpr uint8_t kPassMin = 8;   // WPA2 passphrase: 8-63 characters, or "" (open)
static constexpr uint8_t kPassMax = 63;

// ── Factory test (USB `test`) ────────────────────────────────────────────────
static constexpr uint8_t kTestLevel = 20;            // spec: brightness about 20
static constexpr uint32_t kTestColorMs = 600;        // firmware: each colour on all LEDs
static constexpr uint32_t kTestChaseStepMs = 60;     // firmware: chase, per LED
static constexpr uint32_t kTestChargeMs = 10000;     // spec: charge for 10 s
static constexpr uint32_t kTestAdcIntervalMs = 300;  // firmware: charger ADC during the test
static constexpr uint32_t kTestRebootDelayMs = 300;  // firmware: reply first, then restart

}  // namespace fleet
