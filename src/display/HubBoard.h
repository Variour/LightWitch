#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

namespace display {

// Hub board (Waveshare ESP32-C5-Touch-LCD-3.5): ST7796 panel on SPI, FT6336
// touch and the CH32V003 IO expander on I2C. Pins, init sequence and
// expander registers from Waveshare's Arduino examples
// (waveshareteam/ESP32-C5-Touch-LCD-3.5, examples/arduino/examples/
// 07_lvgl_demo: Board_IO.*, Touch_FT6336.*, 07_lvgl_demo.ino).
class HubBoard {
   public:
    static constexpr uint16_t kWidth = 320;
    static constexpr uint16_t kHeight = 480;

    bool begin() {
        Wire.begin(kSda, kScl);
        Wire.setClock(400000);
        _ioOk = _initExpander();
        SPI.begin(kSclk, -1, kMosi, -1);
        pinMode(kCs, OUTPUT);
        digitalWrite(kCs, HIGH);
        pinMode(kDc, OUTPUT);
        _initPanel();
        pinMode(kTouchInt, INPUT);
        pinMode(kBootKey, INPUT_PULLUP);
        Wire.beginTransmission(kTouchAddr);
        _touchOk = Wire.endTransmission() == 0;
        return _ioOk;
    }

    bool touchOk() const { return _touchOk; }

    // 0-100 %.
    void setBacklight(uint8_t percent) {
        if (!_ioOk) return;
        if (percent > 100) percent = 100;
        _writeReg(kRegPwm, (uint8_t)(percent * 255U / 100U));
    }

    // Rows y0..y1 of a 320-wide RGB565 frame (native byte order).
    void pushRows(const uint16_t* frame, uint16_t y0, uint16_t y1) {
        SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
        digitalWrite(kCs, LOW);
        _cmd(0x2A, {0, 0, (uint8_t)((kWidth - 1) >> 8), (uint8_t)(kWidth - 1)});
        _cmd(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1});
        digitalWrite(kDc, LOW);
        SPI.write(0x2C);
        digitalWrite(kDc, HIGH);
        SPI.writePixels(frame + (size_t)y0 * kWidth, (uint32_t)(y1 - y0 + 1) * kWidth * 2);
        digitalWrite(kCs, HIGH);
        SPI.endTransaction();
    }

    // First touch point in display coordinates.
    bool readTouch(uint16_t& x, uint16_t& y) {
        if (!_touchOk || digitalRead(kTouchInt) == HIGH) return false;
        uint8_t d[5];
        if (!_readRegs(kTouchAddr, 0x02, d, sizeof(d))) return false;
        if ((d[0] & 0x0F) == 0) return false;
        x = (uint16_t)(((d[1] & 0x0F) << 8) | d[2]);
        y = (uint16_t)(((d[3] & 0x0F) << 8) | d[4]);
        return x < kWidth && y < kHeight;
    }

    bool bootKeyDown() const { return digitalRead(kBootKey) == LOW; }

   private:
    static constexpr uint8_t kSda = 27, kScl = 26;
    static constexpr uint8_t kSclk = 6, kMosi = 7, kDc = 5, kCs = 8;
    static constexpr uint8_t kTouchInt = 3;
    static constexpr uint8_t kBootKey = 28;
    static constexpr uint32_t kSpiHz = 60000000;
    static constexpr uint8_t kTouchAddr = 0x38;
    static constexpr uint8_t kIoAddr = 0x24;
    static constexpr uint8_t kRegMode = 0x02, kRegOut = 0x03, kRegPwm = 0x05;
    static constexpr uint8_t kTouchRst = 0, kLcdRst = 1, kPower = 5;

    bool _ioOk = false;
    bool _touchOk = false;

    bool _writeReg(uint8_t reg, uint8_t v) {
        Wire.beginTransmission(kIoAddr);
        Wire.write(reg);
        Wire.write(v);
        return Wire.endTransmission() == 0;
    }

    static bool _readRegs(uint8_t addr, uint8_t reg, uint8_t* d, size_t n) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
        for (size_t i = 0; i < n; i++) d[i] = Wire.read();
        return true;
    }

    // Same order as Waveshare's Board_IO_Init: outputs for both resets and
    // the power switch, pulse both resets, then everything high.
    bool _initExpander() {
        const uint8_t out = (1U << kTouchRst) | (1U << kLcdRst) | (1U << kPower);
        if (!(_writeReg(kRegMode, 0xFF) && _writeReg(kRegOut, 0x00) && _writeReg(kRegPwm, 0x00) &&
              _writeReg(kRegMode, out)))
            return false;
        _writeReg(kRegOut, (1U << kTouchRst) | (1U << kLcdRst));
        delay(50);
        _writeReg(kRegOut, 0);
        delay(50);
        return _writeReg(kRegOut, out) && (delay(120), true);
    }

    void _cmd(uint8_t c, std::initializer_list<uint8_t> data = {}) {
        digitalWrite(kDc, LOW);
        SPI.write(c);
        digitalWrite(kDc, HIGH);
        for (uint8_t b : data) SPI.write(b);
    }

    void _cmdTx(uint8_t c, std::initializer_list<uint8_t> data = {}) {
        SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
        digitalWrite(kCs, LOW);
        _cmd(c, data);
        digitalWrite(kCs, HIGH);
        SPI.endTransaction();
    }

    // ST7796 sequence of Waveshare's BSP.
    void _initPanel() {
        _cmdTx(0x01);
        delay(20);
        _cmdTx(0x11);
        delay(100);
        _cmdTx(0x36, {0x48});
        _cmdTx(0x3A, {0x55});
        _cmdTx(0xF0, {0xC3});
        _cmdTx(0xF0, {0x96});
        _cmdTx(0xB4, {0x01});
        _cmdTx(0xB7, {0xC6});
        _cmdTx(0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33});
        _cmdTx(0xC1, {0x06});
        _cmdTx(0xC2, {0xA7});
        _cmdTx(0xC5, {0x18});
        _cmdTx(0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18,
                      0x1B});
        _cmdTx(0xE1, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17,
                      0x1B});
        _cmdTx(0xF0, {0x3C});
        _cmdTx(0xF0, {0x69});
        _cmdTx(0x21);
        _cmdTx(0x29);
        delay(120);
    }
};

}  // namespace display
