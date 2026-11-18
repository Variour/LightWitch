#pragma once
#include <stdint.h>

// Spatial dithering for fades at low brightness (Timo, 2026-11-10). A fine
// channel value carries 8 fractional bits: 1/256 of an LED step, 0-65280 for
// 0-255. Each LED of a chain rounds up once the fraction passes its own fixed
// threshold, and the thresholds are spread along the chain (golden-ratio
// sequence, neighbours about 0.38 or 0.62 of a step apart). During a fade the
// LEDs reach the next step one after another, so the mean glides in steps of
// a fraction of an LED step instead of every LED jumping at once. A steady
// value stays steady, and in a monotonic fade each LED changes at most once
// per step. The channels of an LED share its threshold, so each channel
// keeps its share and the hue holds at low brightness.
namespace dither {

// Threshold of chain LED i, in 1/256 of a step.
inline uint8_t threshold(uint16_t i) { return (uint8_t)(i * 158u + 79u); }

// Fine channel value v on chain LED i, as the LED's 0-255.
inline uint8_t channel(uint16_t v, uint16_t i) {
    uint8_t whole = (uint8_t)(v >> 8);
    return whole < 255 && (uint8_t)v > threshold(i) ? (uint8_t)(whole + 1) : whole;
}

// Channel c (0-255 at full scale) at the fine level lvl (8.8, 0-65280).
inline uint16_t scale(uint8_t c, uint16_t lvl) { return (uint16_t)((uint32_t)c * lvl / 255); }

// A level 0-255 as a fine level.
inline uint16_t fine(uint8_t lvl) { return (uint16_t)(lvl << 8); }

}  // namespace dither
