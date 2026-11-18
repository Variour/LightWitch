#pragma once
#include <stdint.h>

#include "Config.h"

// The brightness light `index` renders at now: its brightness override when
// enabled, else its group's brightness (255 without a group).
inline uint8_t currentLightLevel(uint8_t index) {
    if (index >= MAX_LIGHTS) return 255;
    const LightHardwareConfig& l = Config::get().lights[index];
    if (l.brightnessOverrideEnabled) return l.brightnessOverride;
    const GroupConfig* g = Config::group(l.groupId);
    return g ? g->light.brightness : 255;
}

// A channel value designed for full brightness, scaled to `level`: at least 1
// while v is above 0.
inline uint8_t scaleToLevel(uint8_t v, uint8_t level) {
    if (v == 0) return 0;
    uint8_t s = (uint8_t)((uint16_t)v * level / 255);
    return s == 0 ? 1 : s;
}
