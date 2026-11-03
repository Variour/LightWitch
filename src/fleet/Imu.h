#pragma once
#include <Arduino.h>
#include <Wire.h>

// The Buzzerboard's LSM6DS3TR-C (I2C 0x6B: SA0 tied to 3V3, board
// hardware/README.md "IMU"). Registers from ST DocID030071 Rev 3. Wire must
// be begun.
//
// - read(): one accelerometer reading for `test info`; the accelerometer's
//   data rate is back to what it was afterwards.
// - orientationOn()/Off(): the built-in 6D orientation detection for a game
//   session (firmware spec "Lage v0"): accelerometer at 26 Hz, 6D event
//   latched on INT1 (GPIO25), position in D6D_SRC. readD6d() reads the
//   position and clears the latch.
namespace fleet {

struct ImuReading {
    bool found = false;  // WHO_AM_I answered
    uint8_t id = 0;      // WHO_AM_I, 6Ah on this part (§9.12)
    bool accelValid = false;
    float g[3] = {};
};

class Imu {
   public:
    static ImuReading read(TwoWire& w = Wire) {
        ImuReading r;
        uint8_t id;
        if (!_read(w, REG_WHO_AM_I, &id, 1)) return r;
        r.found = true;
        r.id = id;
        uint8_t ctrl1 = 0;
        if (!_read(w, REG_CTRL1_XL, &ctrl1, 1)) return r;
        if (!_write(w, REG_CTRL1_XL, CTRL1_XL_104HZ_2G)) return r;
        uint32_t t0 = millis();
        uint8_t status = 0;
        while (millis() - t0 < READY_TIMEOUT_MS) {
            delay(5);
            if (_read(w, REG_STATUS, &status, 1) && (status & STATUS_XLDA)) break;
        }
        uint8_t b[6];
        if ((status & STATUS_XLDA) && _read(w, REG_OUTX_L_XL, b, sizeof(b))) {
            for (uint8_t i = 0; i < 3; i++) {
                int16_t raw = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
                r.g[i] = raw * MG_PER_LSB / 1000.0f;
            }
            r.accelValid = true;
        }
        // Power-down (ODR_XL = 0000) outside a game session, the 6D rate in one.
        _write(w, REG_CTRL1_XL, ctrl1);
        return r;
    }

    // Starts the 6D orientation detection. False if the part does not answer
    // as an LSM6DS3TR-C.
    static bool orientationOn(TwoWire& w = Wire) {
        uint8_t id;
        if (!_read(w, REG_WHO_AM_I, &id, 1) || id != WHO_AM_I_VALUE) return false;
        bool ok = _write(w, REG_CTRL1_XL, CTRL1_XL_26HZ_2G) &&
                  _write(w, REG_TAP_THS_6D, TAP_THS_6D_50DEG) &&
                  _write(w, REG_CTRL8_XL, CTRL8_XL_LPF2_ON_6D) &&
                  _write(w, REG_TAP_CFG, TAP_CFG_INT_LIR) &&
                  _write(w, REG_MD1_CFG, MD1_CFG_INT1_6D);
        uint8_t src;
        return ok && readD6d(src, w);  // clears a latch left from before
    }

    // Back to the reset values: no interrupt, accelerometer in power-down.
    static void orientationOff(TwoWire& w = Wire) {
        _write(w, REG_MD1_CFG, 0);
        _write(w, REG_TAP_CFG, 0);
        _write(w, REG_CTRL8_XL, 0);
        _write(w, REG_TAP_THS_6D, 0);
        _write(w, REG_CTRL1_XL, 0);
    }

    // D6D_SRC: position bits (game::kD6d*) and D6D_IA; reading it clears the
    // latched INT1.
    static bool readD6d(uint8_t& src, TwoWire& w = Wire) { return _read(w, REG_D6D_SRC, &src, 1); }

   private:
    static constexpr uint8_t ADDRESS = 0x6B;
    static constexpr uint8_t REG_WHO_AM_I = 0x0F;  // §9.12
    static constexpr uint8_t WHO_AM_I_VALUE = 0x6A;
    static constexpr uint8_t REG_CTRL1_XL = 0x10;  // §9.13
    static constexpr uint8_t REG_CTRL8_XL = 0x17;  // §9.20
    static constexpr uint8_t REG_D6D_SRC = 0x1D;   // §9.26
    static constexpr uint8_t REG_STATUS = 0x1E;    // §9.27, XLDA = bit 0
    // OUTX_L_XL..OUTZ_H_XL (§9.35-9.40), little endian; CTRL3_C IF_INC is 1
    // by default (reset value 04h), so one burst reads all six.
    static constexpr uint8_t REG_OUTX_L_XL = 0x28;
    static constexpr uint8_t REG_TAP_CFG = 0x58;     // §9.75
    static constexpr uint8_t REG_TAP_THS_6D = 0x59;  // §9.76
    static constexpr uint8_t REG_MD1_CFG = 0x5E;     // §9.81
    static constexpr uint8_t STATUS_XLDA = 1 << 0;
    // ODR_XL = 0100 (104 Hz), FS_XL = 00 (+-2 g), Tables 51 and 52.
    static constexpr uint8_t CTRL1_XL_104HZ_2G = 0x40;
    // ODR_XL = 0010 (26 Hz, high-performance mode by default), FS_XL = 00.
    static constexpr uint8_t CTRL1_XL_26HZ_2G = 0x20;
    // D4D_EN = 0 (6D), SIXD_THS = 11 (50 degrees, Table 187): the most
    // tolerant threshold, for a device held at a slant.
    static constexpr uint8_t TAP_THS_6D_50DEG = 0x60;
    // LOW_PASS_ON_6D = 1: the 6D function reads the LPF2 output (Figure 8).
    static constexpr uint8_t CTRL8_XL_LPF2_ON_6D = 0x01;
    // INTERRUPTS_ENABLE = 1, LIR = 1 (latched until D6D_SRC is read).
    static constexpr uint8_t TAP_CFG_INT_LIR = 0x81;
    // INT1_6D = 1 (Table 198). INT1 is push-pull, active high (CTRL3_C reset value).
    static constexpr uint8_t MD1_CFG_INT1_6D = 0x04;
    // LA_So at +-2 g: 0.061 mg/LSB (mechanical characteristics).
    static constexpr float MG_PER_LSB = 0.061f;
    // Turn-on time 35 ms (Ton) plus the first sample at 104 Hz.
    static constexpr uint32_t READY_TIMEOUT_MS = 100;

    static bool _read(TwoWire& w, uint8_t reg, uint8_t* buf, size_t len) {
        w.beginTransmission(ADDRESS);
        w.write(reg);
        if (w.endTransmission(false) != 0) return false;
        if (w.requestFrom((uint16_t)ADDRESS, len, true) != len) return false;
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)w.read();
        return true;
    }

    static bool _write(TwoWire& w, uint8_t reg, uint8_t v) {
        w.beginTransmission(ADDRESS);
        w.write(reg);
        w.write(v);
        return w.endTransmission(true) == 0;
    }
};

}  // namespace fleet
