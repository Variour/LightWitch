#pragma once
#include <vector>
#include "Pattern.h"
#include "GradientCommon.h"
#include "MatrixLayout.h"

// Renders a horizontal color gradient across a matrix light — the same
// left-to-right ramp on every row. Stop colors are sourced from a scene's
// palette (first-seen distinct colors in the scene's first frame).
// When the light's wrapWidth topology is set, the gradient loops seamlessly
// back to its first stop at the end of each row.
// With morphEnabled, each gradient stop continuously wanders to a freshly
// chosen random palette color, and the whole ramp is resampled from the
// stops' live colors every tick — so entire columns (the ones spanning each
// stop) move together in real time, rather than individual LEDs shimmering.
class GradientMatrix : public GradientCommon::Base<GradientMatrix> {
public:
    float getPeriod() const override { return 0.0f; }

    void setDimensions(uint16_t w, uint16_t h) { _layout.setDimensions(w, h); }
    void setMatrixLayout(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _layout.setWiring(start, dir, serpentine);
    }

private:
    friend class GradientCommon::Base<GradientMatrix>;
    MatrixLayout _layout;

    uint32_t _totalPixels() const { return (uint32_t)_layout.width() * _layout.height(); }
    uint16_t _gradientLength() const { return _layout.width(); }
    float    _gradientCoord(uint32_t i) const { return (float)(i % _layout.width()); }
    uint16_t _ledIndex(uint32_t i) const { return _layout.ledIndex(i / _layout.width(), i % _layout.width()); }
};
