#pragma once
#include <Arduino.h>
#include <Wire.h>

// One accelerometer reading from the Buzzerboard's LSM6DS3TR-C for `test info`
// (I2C 0x6B: SA0 tied to 3V3, board hardware/README.md "IMU"). Registers from
// ST DocID030071 Rev 3. The accelerometer runs only for this reading and is
// back in power-down afterwards. Wire must be begun.
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
        _write(w, REG_CTRL1_XL, 0);  // ODR_XL = 0000: power-down
        return r;
    }

   private:
    static constexpr uint8_t ADDRESS = 0x6B;
    static constexpr uint8_t REG_WHO_AM_I = 0x0F;  // §9.12
    static constexpr uint8_t REG_CTRL1_XL = 0x10;  // §9.13
    static constexpr uint8_t REG_STATUS = 0x1E;    // §9.27, XLDA = bit 0
    // OUTX_L_XL..OUTZ_H_XL (§9.35-9.40), little endian; CTRL3_C IF_INC is 1
    // by default (reset value 04h), so one burst reads all six.
    static constexpr uint8_t REG_OUTX_L_XL = 0x28;
    static constexpr uint8_t STATUS_XLDA = 1 << 0;
    // ODR_XL = 0100 (104 Hz), FS_XL = 00 (+-2 g), Tables 51 and 52.
    static constexpr uint8_t CTRL1_XL_104HZ_2G = 0x40;
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
