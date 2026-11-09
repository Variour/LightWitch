#pragma once
#include <driver/i2c_types.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

// The device I2C bus (I2C port 0) on the IDF I2C master driver: register
// access to 7-bit devices. main.cpp brings it up on the configured pins;
// every chip driver on the bus goes through the one instance, i2cBus.
//
// probe(), write() and read() are one bus transaction each, wait up to
// kTimeoutMs for it and may be called from any task. They return ESP_OK or
// the driver's error, and ESP_ERR_INVALID_STATE while the bus is not begun.
class I2cBus {
   public:
    static constexpr uint32_t kStandardHz = 100000;
    static constexpr int kTimeoutMs = 50;
    // Most data bytes write() takes in one call.
    static constexpr size_t kMaxWriteLen = 16;

    I2cBus();

    // Starts the bus on the given pins at hz, with the pins' internal
    // pull-ups on. On a bus that already runs it returns ESP_OK and leaves
    // the bus as it is.
    esp_err_t begin(int sda, int scl, uint32_t hz = kStandardHz);

    // Stops the bus and releases its pins; begin() starts it again.
    void end();

    // ESP_OK if a device acknowledges `address`.
    esp_err_t probe(uint8_t address);

    // Writes len bytes to the device's registers starting at reg: start,
    // address, reg, data, stop.
    esp_err_t write(uint8_t address, uint8_t reg, const uint8_t* data, size_t len);

    // Reads len bytes from the device's registers starting at reg: start,
    // address, reg, repeated start, address, data, stop.
    esp_err_t read(uint8_t address, uint8_t reg, uint8_t* data, size_t len);

   private:
    static constexpr uint8_t kAddressCount = 128;

    StaticSemaphore_t _lockStorage;
    SemaphoreHandle_t _lock;  // guards everything below and spans each transaction
    i2c_master_bus_handle_t _bus = nullptr;
    uint32_t _hz = kStandardHz;
    // The driver's handle for each address used so far, created on first use.
    i2c_master_dev_handle_t _devices[kAddressCount] = {};

    esp_err_t _device(uint8_t address, i2c_master_dev_handle_t& out);
};

extern I2cBus i2cBus;
