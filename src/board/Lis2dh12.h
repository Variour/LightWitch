#pragma once
#include <Arduino.h>
#include <Wire.h>

// ST LIS2DH12 accelerometer on the device I2C bus (Steuerboard S). Registers
// and bit names from the ST LIS2DH12 datasheet (DocID025056); verify against the first board. Runs
// in low-power mode at 10 Hz,
// +-2 g: gravity tells where up is, and the high-pass filtered wake-up
// interrupt on INT1 reports motion. INT1 is latched and active low, so a
// deep-sleep or light-sleep GPIO wake can wait for it; readMotion() clears it.
class Lis2dh12 {
   public:
    // Motion threshold of the wake-up interrupt: INT1_THS LSB = 16 mg at
    // +-2 g. 5 = 80 mg after the high-pass filter, enough to ignore a
    // table being bumped lightly, low enough for picking a ball up.
    static constexpr uint8_t MOTION_THRESHOLD = 5;

    // Looks for the sensor at 0x18 (SA0 low) and 0x19 (SA0 high), WHO_AM_I
    // 0x33. Wire must be begun.
    bool probe(TwoWire& w = Wire) {
        _w = &w;
        for (uint8_t a : {ADDR_SA0_LOW, ADDR_SA0_HIGH}) {
            _addr = a;
            uint8_t id = 0;
            if (_read(REG_WHO_AM_I, &id, 1) && id == WHO_AM_I_VALUE) return _found = true;
        }
        return _found = false;
    }
    bool found() const { return _found; }

    // Low-power mode 10 Hz, all axes, wake-up interrupt on INT1.
    bool begin() {
        if (!_found) return false;
        bool ok = _write(REG_CTRL_REG1, CTRL1_10HZ_LP_XYZ) &&
                  _write(REG_CTRL_REG2, CTRL2_HP_IA1) &&    // high-pass on the interrupt only
                  _write(REG_CTRL_REG3, CTRL3_I1_IA1) &&    // IA1 on INT1
                  _write(REG_CTRL_REG4, CTRL4_BDU) &&       // +-2 g
                  _write(REG_CTRL_REG5, CTRL5_LIR_INT1) &&  // latched
                  _write(REG_CTRL_REG6, CTRL6_INT_ACTIVE_LOW) &&
                  _write(REG_INT1_THS, MOTION_THRESHOLD) && _write(REG_INT1_DURATION, 0) &&
                  _write(REG_INT1_CFG, INT1_CFG_XYZ_HIGH_OR);
        uint8_t dummy;
        _read(REG_REFERENCE, &dummy, 1);  // sets the high-pass filter to the current reading
        _read(REG_INT1_SRC, &dummy, 1);   // clears a stale latch
        return ok;
    }

    // Power-down: ODR 0000, the interrupt stops too.
    void powerDown() {
        if (_found) _write(REG_CTRL_REG1, 0);
    }

    // Acceleration in g, sensor axes. At rest it points up (the reaction to
    // gravity). Low-power mode: 8-bit left-justified, 16 mg per digit at +-2 g
    //.
    bool readG(float* g) {
        uint8_t b[6];
        if (!_read(REG_OUT_X_L | AUTO_INCREMENT, b, sizeof(b))) return false;
        for (uint8_t i = 0; i < 3; i++) {
            int16_t raw = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
            g[i] = (raw >> 8) * 0.016f;
        }
        return true;
    }

    // Whether the wake-up interrupt fired since the last call; clears the
    // latch (reading INT1_SRC).
    bool readMotion() {
        uint8_t src = 0;
        if (!_read(REG_INT1_SRC, &src, 1)) return false;
        return src & INT1_SRC_IA;
    }

   private:
    static constexpr uint8_t ADDR_SA0_LOW = 0x18;
    static constexpr uint8_t ADDR_SA0_HIGH = 0x19;
    static constexpr uint8_t REG_WHO_AM_I = 0x0F;
    static constexpr uint8_t WHO_AM_I_VALUE = 0x33;
    static constexpr uint8_t REG_CTRL_REG1 = 0x20;
    static constexpr uint8_t REG_CTRL_REG2 = 0x21;
    static constexpr uint8_t REG_CTRL_REG3 = 0x22;
    static constexpr uint8_t REG_CTRL_REG4 = 0x23;
    static constexpr uint8_t REG_CTRL_REG5 = 0x24;
    static constexpr uint8_t REG_CTRL_REG6 = 0x25;
    static constexpr uint8_t REG_REFERENCE = 0x26;
    static constexpr uint8_t REG_OUT_X_L = 0x28;
    static constexpr uint8_t REG_INT1_CFG = 0x30;
    static constexpr uint8_t REG_INT1_SRC = 0x31;
    static constexpr uint8_t REG_INT1_THS = 0x32;
    static constexpr uint8_t REG_INT1_DURATION = 0x33;
    static constexpr uint8_t AUTO_INCREMENT = 0x80;  // MSB of the sub-address

    static constexpr uint8_t CTRL1_10HZ_LP_XYZ = 0x2F;     // ODR 0010, LPen, Z/Y/X enable
    static constexpr uint8_t CTRL2_HP_IA1 = 0x01;          // HP_IA1
    static constexpr uint8_t CTRL3_I1_IA1 = 0x40;          // I1_IA1
    static constexpr uint8_t CTRL4_BDU = 0x80;             // block data update, FS 00
    static constexpr uint8_t CTRL5_LIR_INT1 = 0x08;        // latch INT1
    static constexpr uint8_t CTRL6_INT_ACTIVE_LOW = 0x02;  // INT_POLARITY
    static constexpr uint8_t INT1_CFG_XYZ_HIGH_OR = 0x2A;  // ZHIE, YHIE, XHIE, OR
    static constexpr uint8_t INT1_SRC_IA = 0x40;

    TwoWire* _w = &Wire;
    uint8_t _addr = ADDR_SA0_LOW;
    bool _found = false;

    bool _read(uint8_t reg, uint8_t* buf, size_t len) {
        _w->beginTransmission(_addr);
        _w->write(reg);
        if (_w->endTransmission(false) != 0) return false;
        if (_w->requestFrom((uint16_t)_addr, len, true) != len) return false;
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)_w->read();
        return true;
    }

    bool _write(uint8_t reg, uint8_t v) {
        _w->beginTransmission(_addr);
        _w->write(reg);
        _w->write(v);
        return _w->endTransmission(true) == 0;
    }
};
