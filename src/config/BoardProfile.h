#pragma once
#include "Config.h"

// Looks for the board this build targets and keeps the answer for the rest of
// the boot. Config::load() calls it first, before the device I2C bus is set
// up: it borrows Wire on the board's own I2C pins and releases it again.
void detectBoard();

// Seeds the hardware configuration of the board this build targets (lights,
// buttons with roles, I2C bus, charger) into a fresh DeviceConfig. Config::load()
// calls it only when no saved config exists, so it never overrides a device
// that has already been configured. Returns false unless detectBoard() found
// the board (cfg is left untouched).
bool applyBoardProfile(DeviceConfig& cfg);

// The LED current cap (mA, 0 = off) to drive a light with when its setting is
// configuredMa: on a detected board whose LED rail has a supply limit, at most
// that board's cap, also when the setting is off; elsewhere the setting.
uint16_t boardLedCurrentLimitMa(uint16_t configuredMa);

// How often to send an unchanged LED frame again (ms, 0 = only on change):
// on a detected board whose LED rail can drop for a moment without the
// firmware seeing it, so the LEDs come back; elsewhere 0.
uint16_t boardLedRefreshMs();

// Whether detectBoard() found the board this build targets at this boot (the
// Buzzerboard on esp32c5 builds; false on every other build).
bool boardDetected();

// Whether the device opens its fallback softAP by itself (no WiFi network
// configured or reachable). False on a detected Buzzerboard: 50 open APs at
// an event saturate the channel and let anyone reconfigure the devices, so
// the Buzzerboard opens a maintenance AP on request only (src/mesh/SoftAp.h).
bool boardHasFallbackAp();
