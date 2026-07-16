#pragma once
#include <vector>

#include "GradientCommon.h"
#include "Pattern.h"

// Renders a color gradient across a string light. Stop colors are sourced
// from a scene's palette (first-seen distinct colors in the scene's first
// frame) rather than being picked directly.
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop instead of running linearly from end to end.
// With morphEnabled, each gradient stop continuously wanders to a freshly
// chosen random palette color, and the whole ramp is resampled from the
// stops' live colors every tick, so the gradient updates in real time.
class GradientString : public GradientCommon::Base<GradientString> {
   public:
    float getPeriod() const override { return 0.0f; }

    void setNumLeds(uint16_t n) { _numLeds = n; }

   private:
    friend class GradientCommon::Base<GradientString>;
    uint16_t _numLeds = 1;

    uint32_t _totalPixels() const { return _numLeds; }
    uint16_t _gradientLength() const { return _numLeds; }
    float _gradientCoord(uint32_t i) const { return (float)i; }
    uint16_t _ledIndex(uint32_t i) const { return (uint16_t)i; }
};
