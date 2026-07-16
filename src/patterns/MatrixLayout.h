#pragma once
#include "../config/Config.h"

// Shared LED-index mapping and stretch-scaling math for matrix pattern
// renderers (GradientMatrix, SceneMatrix, TextMatrix, TimeMatrix), used via
// composition so wiring/orientation fixes only need to be made in one place.
class MatrixLayout {
   public:
    void setDimensions(uint16_t w, uint16_t h) {
        _width = w;
        _height = h;
    }
    void setWiring(MatrixStart start, MatrixDirection dir, bool serpentine) {
        _start = start;
        _dir = dir;
        _serpentine = serpentine;
    }

    uint16_t width() const { return _width; }
    uint16_t height() const { return _height; }

    // Serpentine (zig-zag/boustrophedon) wiring: the physical strip reverses
    // direction on every other line along the primary axis (the one named by
    // the configured MatrixDirection), since it's one continuous strip folded
    // back and forth.
    uint16_t ledIndex(uint16_t row, uint16_t col) const {
        uint16_t r = (_start == MatrixStart::BottomLeft || _start == MatrixStart::BottomRight)
                         ? (_height - 1 - row)
                         : row;
        uint16_t c = (_start == MatrixStart::TopRight || _start == MatrixStart::BottomRight)
                         ? (_width - 1 - col)
                         : col;
        if (_dir == MatrixDirection::Vertical) {
            if (_serpentine && (c & 1)) r = _height - 1 - r;
            return c * _height + r;
        } else {
            if (_serpentine && (r & 1)) c = _width - 1 - c;
            return r * _width + c;
        }
    }

    // Maps a destination coordinate (0..dstSize) to the nearest source coordinate
    // (0..srcSize) for stretch scaling, sampling at the center of each destination cell.
    static uint16_t nearest(uint16_t dst, uint16_t dstSize, uint16_t srcSize) {
        uint32_t src = ((uint32_t)dst * 2 + 1) * srcSize / ((uint32_t)dstSize * 2);
        return (src >= srcSize) ? (uint16_t)(srcSize - 1) : (uint16_t)src;
    }

   private:
    uint16_t _width = 1, _height = 1;
    MatrixStart _start = MatrixStart::TopLeft;
    MatrixDirection _dir = MatrixDirection::Horizontal;
    bool _serpentine = false;
};
