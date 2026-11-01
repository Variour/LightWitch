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
