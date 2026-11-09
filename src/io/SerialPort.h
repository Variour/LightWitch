#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
#include <driver/usb_serial_jtag.h>
#else
#include <driver/uart.h>
#include <soc/soc_caps.h>
#include <soc/uart_pins.h>
#endif

// The firmware's serial port: the Logger's serial sink and the NDJSON link of
// src/game/Link.h (docs/usb-link.md). It calls the IDF driver of the port the
// env is built for:
//
// - USB-Serial-JTAG (envs with ARDUINO_USB_MODE=1 and ARDUINO_USB_CDC_ON_BOOT=1:
//   esp32c3, esp32c5, esp32s3) through driver/usb_serial_jtag.h.
// - UART0 at 115200 baud, 8N1 (esp32dev) through driver/uart.h.
//
// The IDF console and the ROM print routines (Arduino core log, panic output)
// write to the same port next to the driver.
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT

// From core debug level 4 the Arduino core starts its own USB-Serial-JTAG
// driver before setup() (chip debug report); this one needs the port alone.
#if defined(CORE_DEBUG_LEVEL) && CORE_DEBUG_LEVEL >= 4
#error "SerialPort drives the USB-Serial-JTAG: build this env with CORE_DEBUG_LEVEL 3 or lower"
#endif

// A host that reads gets every byte in order. While no host reads (USB power
// only, or the host has the port closed) the port holds the newest bytes, one
// TX buffer full, and hands them over once a host reads again.
class SerialPort {
   public:
    // Buffer sizes of a port without the game link.
    static constexpr size_t kDefaultBufferBytes = 256;
    // How long write() gives a host that reads to make room.
    static constexpr uint32_t kWriteWaitMs = 100;

    // In setup(), before the first log line.
    static void start(size_t rxBytes = kDefaultBufferBytes, size_t txBytes = kDefaultBufferBytes) {
        usb_serial_jtag_driver_config_t cfg = {};
        cfg.rx_buffer_size = rxBytes;
        cfg.tx_buffer_size = txBytes;
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
        _txBytes = txBytes;
        _room = txBytes;
        _held = (uint8_t*)malloc(txBytes);
        _lock = xSemaphoreCreateMutex();
        _started = _lock != nullptr;
    }

    // Bytes from the host, at most n, without waiting.
    static size_t read(uint8_t* buf, size_t n) {
        return _started ? (size_t)usb_serial_jtag_read_bytes(buf, n, 0) : 0;
    }

    // Bytes write() takes now without waiting and without giving up held ones.
    static size_t writable() {
        if (!_started) return 0;
        _take();
        size_t n;
        if (_heldLen > 0 || !hostConnected()) {
            n = _held ? _txBytes - _heldLen : 0;
        } else {
            // The driver has no free-space query, it reports when all written
            // bytes have left: count down from the full buffer, less the last
            // write, which that report can be one behind on.
            if (usb_serial_jtag_wait_tx_done(0) == ESP_OK) _room = _txBytes - _lastWrite;
            n = _room;
        }
        _give();
        return n;
    }

    // Takes all n bytes. While a host reads, waits up to the write wait in
    // total for room in the TX buffer; after a wait that ran out, the next
    // one comes once the host reads again.
    static void write(const void* data, size_t n) {
        if (!_started) return;
        const uint8_t* p = (const uint8_t*)data;
        _take();
        if (hostConnected()) {
            Wait wait(_stalled ? 0 : pdMS_TO_TICKS(_waitMs));
            _handOver(wait);
            while (n > 0 && _heldLen == 0) {
                size_t chunk = n < _txBytes ? n : _txBytes;
                if (!_toDriver(p, chunk, wait.left())) break;
                p += chunk;
                n -= chunk;
            }
        }
        _hold(p, n);
        _give();
    }

    // 0: write() and flush() never wait (the game bridge's loop).
    static void setWriteWaitMs(uint32_t ms) { _waitMs = ms; }

    // Waits until a host has taken the written bytes, at most the write wait.
    static void flush() {
        if (!_started || !hostConnected()) return;
        _take();
        Wait wait(pdMS_TO_TICKS(_waitMs));
        _handOver(wait);
        if (_heldLen == 0) usb_serial_jtag_wait_tx_done(wait.left());
        _give();
    }

    // Every loop() pass: held bytes go out once a host reads. One try per
    // FreeRTOS tick, without waiting.
    static void tick() {
        if (_heldLen == 0 || !hostConnected()) return;
        TickType_t now = xTaskGetTickCount();
        if (now == _lastTry || xSemaphoreTake(_lock, 0) != pdTRUE) return;
        _lastTry = now;
        Wait wait(0);
        _handOver(wait);
        _give();
    }

    // A USB host sends start-of-frame packets (IDF connection monitor). A
    // charger or power bank does not.
    static bool hostConnected() { return usb_serial_jtag_is_connected(); }

   private:
    static constexpr size_t kPacketBytes = 64;  // one USB packet of the port

    inline static bool _started = false;
    inline static SemaphoreHandle_t _lock = nullptr;
    inline static size_t _txBytes = 0;
    inline static uint32_t _waitMs = kWriteWaitMs;
    inline static bool _stalled = false;     // the host let a write wait run out
    inline static size_t _room = 0;          // free bytes in the driver's TX buffer, at least
    inline static size_t _lastWrite = 0;     // size of the last write to the driver
    inline static uint8_t* _held = nullptr;  // ring of the newest bytes not handed over
    inline static size_t _heldHead = 0;
    inline static size_t _heldLen = 0;
    inline static TickType_t _lastTry = 0;

    // The time one call may still wait for the host.
    struct Wait {
        explicit Wait(TickType_t ticks) : _left(ticks) { vTaskSetTimeOutState(&_since); }
        TickType_t left() {
            xTaskCheckForTimeOut(&_since, &_left);
            return _left;
        }

       private:
        TimeOut_t _since;
        TickType_t _left;
    };

    static void _take() { xSemaphoreTake(_lock, portMAX_DELAY); }
    static void _give() { xSemaphoreGive(_lock); }

    // All n bytes into the driver's TX buffer, or none.
    static bool _toDriver(const uint8_t* p, size_t n, TickType_t wait) {
        if (usb_serial_jtag_write_bytes(p, n, wait) == 0) {
            if (wait > 0) _stalled = true;
            return false;
        }
        _stalled = false;
        _lastWrite = n;
        _room -= n < _room ? n : _room;
        return true;
    }

    // Held bytes into the driver, oldest first, while it takes them.
    static void _handOver(Wait& wait) {
        while (_heldLen > 0) {
            size_t chunk = _txBytes - _heldHead;
            if (chunk > _heldLen) chunk = _heldLen;
            if (chunk > kPacketBytes) chunk = kPacketBytes;
            if (!_toDriver(_held + _heldHead, chunk, wait.left())) return;
            _heldHead = (_heldHead + chunk) % _txBytes;
            _heldLen -= chunk;
        }
    }

    // Keeps the newest _txBytes bytes.
    static void _hold(const uint8_t* p, size_t n) {
        if (!_held) return;
        if (n > _txBytes) {
            p += n - _txBytes;
            n = _txBytes;
        }
        for (; n > 0; n--) {
            _held[(_heldHead + _heldLen) % _txBytes] = *p++;
            if (_heldLen < _txBytes)
                _heldLen++;
            else
                _heldHead = (_heldHead + 1) % _txBytes;
        }
    }
};

#else

class SerialPort {
   public:
    // Buffer sizes of a port without the game link.
    static constexpr size_t kDefaultBufferBytes = 256;

    // In setup(), before the first log line.
    static void start(size_t rxBytes = kDefaultBufferBytes, size_t txBytes = kDefaultBufferBytes) {
        uart_config_t cfg = {};
        cfg.baud_rate = 115200;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        // A clock that keeps the baud rate through CPU frequency changes.
#if SOC_UART_SUPPORT_XTAL_CLK
        cfg.source_clk = UART_SCLK_XTAL;
#elif SOC_UART_SUPPORT_REF_TICK
        cfg.source_clk = UART_SCLK_REF_TICK;
#else
        cfg.source_clk = UART_SCLK_DEFAULT;
#endif
        if (uart_driver_install(kUart, rxBytes, txBytes, 0, nullptr, 0) != ESP_OK) return;
        uart_param_config(kUart, &cfg);
        uart_set_pin(kUart, U0TXD_GPIO_NUM, U0RXD_GPIO_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        // Received bytes reach read() one idle symbol after the last one.
        uart_set_rx_timeout(kUart, 1);
        uart_set_rx_full_threshold(kUart, kRxFifoFullBytes);
        _started = true;
    }

    // Bytes from the host, at most n, without waiting.
    static size_t read(uint8_t* buf, size_t n) {
        if (!_started) return 0;
        int got = uart_read_bytes(kUart, buf, n, 0);
        return got > 0 ? (size_t)got : 0;
    }

    // Free bytes in the TX buffer: what write() takes now without waiting.
    static size_t writable() {
        size_t n = 0;
        if (_started) uart_get_tx_buffer_free_size(kUart, &n);
        return n;
    }

    // Takes all n bytes; waits for room while the TX buffer is full.
    static void write(const void* data, size_t n) {
        if (_started) uart_write_bytes(kUart, data, n);
    }

    // Waits until the written bytes are on the wire.
    static void flush() {
        if (_started) uart_wait_tx_done(kUart, pdMS_TO_TICKS(kFlushMs));
    }

    // Every loop() pass.
    static void tick() {}

   private:
    static constexpr uart_port_t kUart = UART_NUM_0;
    static constexpr int kRxFifoFullBytes = 120;
    static constexpr uint32_t kFlushMs = 500;  // a full 4 KB TX buffer takes 370 ms

    inline static bool _started = false;
};

#endif
