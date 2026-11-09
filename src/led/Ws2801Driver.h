#pragma once
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include <vector>

#include "LedDriver.h"

// WS2801 (data + clock) output, clocked out bit by bit on two GPIOs, so any
// pin pair works and every light has its own.
class Ws2801Driver : public LedDriver {
   public:
    void setup(uint8_t dataPin, uint8_t clockPin, uint16_t numLeds,
               ColorOrder order = ColorOrder::RGB) {
        _dataPin = dataPin;
        _clockPin = clockPin;
        _numLeds = numLeds;
        _order = order;
    }

    void begin() override {
        _pixels.assign((size_t)_numLeds * 3, 0);
        _wired = _pinValid(_dataPin) && _pinValid(_clockPin);
        if (_wired) {
            gpio_config_t io = {};
            io.pin_bit_mask = (1ULL << _dataPin) | (1ULL << _clockPin);
            io.mode = GPIO_MODE_OUTPUT;
            gpio_config(&io);
        }
        show();
    }

    // Shifts the frame out most significant bit first: the data line is set
    // while the clock is low and taken over on its rising edge. The clock
    // staying low for LATCH_MS afterwards latches the frame.
    void show() override {
        if (!_wired) return;
        const gpio_num_t data = (gpio_num_t)_dataPin;
        const gpio_num_t clock = (gpio_num_t)_clockPin;
        for (uint8_t v : _pixels) {
            for (uint8_t bit = 0x80; bit; bit >>= 1) {
                gpio_set_level(data, (v & bit) ? 1 : 0);
                gpio_set_level(clock, 1);
                gpio_set_level(clock, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LATCH_MS));
    }

    void off() override {
        memset(_pixels.data(), 0, _pixels.size());
        show();
    }

    void setColorOrder(ColorOrder order) override { _order = order; }

   protected:
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        for (size_t k = 0; k + 2 < _pixels.size(); k += 3) {
            _pixels[k] = a;
            _pixels[k + 1] = bb;
            _pixels[k + 2] = c;
        }
        show();
    }

    void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        size_t k = (size_t)idx * 3;
        if (k + 2 >= _pixels.size()) return;
        _pixels[k] = a;
        _pixels[k + 1] = bb;
        _pixels[k + 2] = c;
    }

   private:
    static constexpr uint32_t LATCH_MS = 1;

    uint8_t _dataPin = 25;
    uint8_t _clockPin = 26;
    uint16_t _numLeds = 1;
    ColorOrder _order = ColorOrder::RGB;
    // The bytes that go out on the wire: 3 per pixel, in wire order.
    std::vector<uint8_t> _pixels;
    bool _wired = false;

    static bool _pinValid(uint8_t pin) {
        return pin < GPIO_NUM_MAX && GPIO_IS_VALID_OUTPUT_GPIO((int)pin);
    }
};
