#pragma once
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "../logging/Logger.h"
#include "LedDriver.h"

// WS2812B (single-wire) output through the ESP-IDF RMT TX driver.
class Ws2812bDriver : public LedDriver {
   public:
    // Current of one colour channel at full scale (255), the model behind the
    // optional current cap: 12 mA per channel, 36 mA per pixel at full white
    // (OPSCO SK6812MINI-E datasheet Rev. 04, §9 "SK6812MINI-E 12mA"; the part
    // also comes as a 5 mA version, for which this estimate is high). A
    // pixel's idle current is not part of the estimate.
    static constexpr uint32_t CHANNEL_FULL_SCALE_MA = 12;
    // With a cap, the estimated current rises by at most this much per shown
    // frame, so a bright frame after a dark one (or after the LED rail comes
    // up) ramps up over a few frames instead of one step.
    static constexpr uint32_t CAP_SLEW_MA_PER_FRAME = 50;

    void setup(uint8_t dataPin, uint16_t numLeds, ColorOrder order = ColorOrder::GRB) {
        _pin = dataPin;
        _numLeds = numLeds;
        _order = order;
    }

    void begin() override {
        _pixels.assign((size_t)_numLeds * 3, 0);
        if (!_busLock) _busLock = xSemaphoreCreateMutex();
        // The data line is a push-pull output that idles low.
        if (_pinValid()) {
            gpio_set_level((gpio_num_t)_pin, 0);
            gpio_config_t io = {};
            io.pin_bit_mask = 1ULL << _pin;
            io.mode = GPIO_MODE_OUTPUT;
            gpio_config(&io);
        }
        _transmit();
    }

    // Returns once the frame is on the wire. While the LED rail is down, the
    // frame stays in the buffer and goes out with the show() after
    // setRailPowered(true).
    void show() override {
        _shownSinceService = true;
        if (!_railPowered) return;
        _syncCurrentLimit();
        if (_limitMa) _writeCappedFrame();
        _transmit();
    }

    void off() override {
        _syncCurrentLimit();
        if (_limitMa) memset(_frame.data(), 0, _frame.size());
        _shownCounts = 0;
        _rampPending = false;
        memset(_pixels.data(), 0, _pixels.size());
        if (_railPowered) _transmit();
    }

    void setRailPowered(bool powered) override {
        _railPowered = powered;
        if (!powered) _shownCounts = 0;
    }

    // Continues a slew-limited ramp for frames that are not redrawn (static
    // patterns), one step per render tick.
    void service() override {
        if (_rampPending && _railPowered && !_shownSinceService) show();
        _shownSinceService = false;
    }

    void setColorOrder(ColorOrder order) override { _order = order; }

    // Safe to call from any task: the new cap takes effect on the next write
    // or show(), which run on the render (loop) task.
    void setCurrentLimitMa(uint16_t mA) override { _requestedLimitMa.store(mA); }

   protected:
    void writeColor(uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _syncCurrentLimit();
        std::vector<uint8_t>& buf = _limitMa ? _frame : _pixels;
        for (size_t k = 0; k + 2 < buf.size(); k += 3) {
            buf[k] = a;
            buf[k + 1] = bb;
            buf[k + 2] = c;
        }
        show();
    }

    void writePixel(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) override {
        uint8_t a, bb, c;
        applyColorOrder(_order, r, g, b, a, bb, c);
        _syncCurrentLimit();
        std::vector<uint8_t>& buf = _limitMa ? _frame : _pixels;
        size_t k = (size_t)idx * 3;
        if (k + 2 >= buf.size()) return;
        buf[k] = a;
        buf[k + 1] = bb;
        buf[k + 2] = c;
    }

   private:
    uint8_t _pin = 25;
    uint16_t _numLeds = 1;
    ColorOrder _order = ColorOrder::GRB;
    // The bytes that go out on the wire: 3 per pixel, in wire order.
    // applyColorOrder() in writeColor()/writePixel() is the single place that
    // permutes r,g,b.
    std::vector<uint8_t> _pixels;
    // End of this light's last frame (esp_timer_get_time()).
    int64_t _frameEndUs = 0;

    // Current cap. With a cap, pixel writes go to _frame (the frame as
    // requested, in wire order) and show() writes a scaled copy into
    // _pixels; without one (0), writes go straight to _pixels and _frame
    // stays empty.
    std::atomic<uint16_t> _requestedLimitMa{0};
    uint16_t _limitMa = 0;
    std::vector<uint8_t> _frame;
    // Capped mode only: channel sum of what the LEDs show now (0 after the
    // rail went down), and whether the slew limit held the last frame back.
    uint32_t _shownCounts = 0;
    bool _rampPending = false;
    bool _shownSinceService = false;
    bool _railPowered = true;
    bool _startFailureLogged = false;

    // Applies a cap change from setCurrentLimitMa() on the render task. The
    // wire buffer _pixels holds 3 bytes per pixel in wire order, the same
    // layout as _frame.
    void _syncCurrentLimit() {
        uint16_t want = _requestedLimitMa.load();
        if (want == _limitMa || _pixels.empty()) return;
        if (_limitMa == 0) {
            // Cap switches on: the uncapped _pixels become the requested
            // frame.
            _frame = _pixels;
            _shownCounts = 0;
            if (_railPowered)
                for (uint8_t v : _frame) _shownCounts += v;
        } else if (want == 0) {
            // Cap switches off: _pixels get the requested frame back,
            // unscaled.
            _pixels = _frame;
            std::vector<uint8_t>().swap(_frame);
        }
        _limitMa = want;
    }

    // Estimates the frame's current as sum(channel values) x
    // CHANNEL_FULL_SCALE_MA / 255 and, if it exceeds the budget, scales every
    // channel by the same factor (rounded down, so the result stays within
    // it). The budget is the cap, or less while the slew limit ramps up from
    // the current shown frame.
    void _writeCappedFrame() {
        uint8_t* p = _pixels.data();
        uint32_t sum = 0;
        for (uint8_t v : _frame) sum += v;
        uint32_t cap = (uint32_t)_limitMa * 255 / CHANNEL_FULL_SCALE_MA;
        uint32_t slewed = _shownCounts + CAP_SLEW_MA_PER_FRAME * 255 / CHANNEL_FULL_SCALE_MA;
        uint32_t budget = slewed < cap ? slewed : cap;
        uint32_t shown = 0;
        if (sum <= budget) {
            memcpy(p, _frame.data(), _frame.size());
            shown = sum;
        } else {
            for (size_t k = 0; k < _frame.size(); k++) {
                p[k] = (uint8_t)((uint32_t)_frame[k] * budget / sum);
                shown += p[k];
            }
        }
        _rampPending = sum > budget && budget < cap;
        _shownCounts = shown;
    }

    // Wire format (800 kHz): a 0 bit is 0.4 us high and 0.8 us low, a 1 bit
    // 0.8 us high and 0.4 us low, most significant bit first; LATCH_US of low
    // level between two frames latch the first one.
    static constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;  // 0.1 us per tick
    static constexpr uint16_t BIT_SHORT_TICKS = 4;
    static constexpr uint16_t BIT_LONG_TICKS = 8;
    static constexpr int64_t LATCH_US = 300;
    // A frame waits this long for the bus while another task sends.
    static constexpr uint32_t BUS_WAIT_MS = 50;

    // All WS2812 lights send through one RMT TX channel, routed to the pin of
    // the light that sends, so MAX_LIGHTS lights fit the two TX channels of
    // an ESP32-C3/C5. The bytes encoder turns the pixel bytes into RMT symbols
    // while the channel sends them. The lock serializes frames across lights
    // and tasks.
    static inline SemaphoreHandle_t _busLock = nullptr;
    static inline rmt_channel_handle_t _busChannel = nullptr;
    static inline rmt_encoder_handle_t _busEncoder = nullptr;
    static inline int _busPin = -1;

    bool _pinValid() const { return _pin < GPIO_NUM_MAX && GPIO_IS_VALID_OUTPUT_GPIO((int)_pin); }

    // Sends _pixels and returns once the frame is on the wire.
    void _transmit() {
        if (_pixels.empty() || !_busLock) return;
        if (xSemaphoreTake(_busLock, pdMS_TO_TICKS(BUS_WAIT_MS)) != pdTRUE) return;
        if (_routeBus()) {
            int64_t sinceUs = esp_timer_get_time() - _frameEndUs;
            if (sinceUs < LATCH_US) esp_rom_delay_us((uint32_t)(LATCH_US - sinceUs));
            rmt_transmit_config_t tx = {};
            if (rmt_transmit(_busChannel, _busEncoder, _pixels.data(), _pixels.size(), &tx) ==
                ESP_OK)
                rmt_tx_wait_all_done(_busChannel, -1);
            _frameEndUs = esp_timer_get_time();
        }
        xSemaphoreGive(_busLock);
    }

    // Logs once per light why its output did not start.
    bool _startFailed(const char* why) {
        if (!_startFailureLogged) {
            _startFailureLogged = true;
            Logger::e("[led] WS2812 on GPIO%u: %s", (unsigned)_pin, why);
        }
        return false;
    }

    // Routes the bus to this light's pin; the first frame creates the channel
    // and the encoder. Called with the lock held.
    bool _routeBus() {
        if (!_pinValid()) return _startFailed("the pin cannot be an output");
        if (_busPin == _pin) return true;
        if (!_busChannel) {
            rmt_bytes_encoder_config_t enc = {};
            enc.bit0.level0 = 1;
            enc.bit0.duration0 = BIT_SHORT_TICKS;
            enc.bit0.level1 = 0;
            enc.bit0.duration1 = BIT_LONG_TICKS;
            enc.bit1.level0 = 1;
            enc.bit1.duration0 = BIT_LONG_TICKS;
            enc.bit1.level1 = 0;
            enc.bit1.duration1 = BIT_SHORT_TICKS;
            enc.flags.msb_first = 1;
            if (!_busEncoder && rmt_new_bytes_encoder(&enc, &_busEncoder) != ESP_OK)
                return _startFailed("no RMT encoder");
            rmt_tx_channel_config_t ch = {};
            ch.gpio_num = (gpio_num_t)_pin;
            ch.clk_src = RMT_CLK_SRC_DEFAULT;
            ch.resolution_hz = RMT_RESOLUTION_HZ;
            ch.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
            ch.trans_queue_depth = 1;
            if (rmt_new_tx_channel(&ch, &_busChannel) != ESP_OK)
                return _startFailed("no RMT TX channel");
        } else {
            // The pin the bus leaves is a plain output again, low since
            // begin().
            rmt_disable(_busChannel);
            rmt_tx_switch_gpio(_busChannel, (gpio_num_t)_pin, false);
            gpio_set_direction((gpio_num_t)_busPin, GPIO_MODE_OUTPUT);
        }
        rmt_enable(_busChannel);
        _busPin = _pin;
        return true;
    }
};
