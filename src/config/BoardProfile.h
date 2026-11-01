#pragma once
#include "Config.h"

// Seeds the hardware configuration of the board this build targets (lights,
// buttons with roles, I2C bus, charger) into a fresh DeviceConfig. Config::load()
// calls it only when no saved config exists, so it never overrides a device
// that has already been configured. Returns false when the build targets no
// specific board (cfg is left untouched).
bool applyBoardProfile(DeviceConfig& cfg);
