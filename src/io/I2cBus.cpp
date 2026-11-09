#include "I2cBus.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <soc/soc_caps.h>
#include <string.h>

I2cBus i2cBus;

namespace {

struct Lock {
    explicit Lock(SemaphoreHandle_t lock) : _lock(lock) { xSemaphoreTake(_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(_lock); }
    SemaphoreHandle_t _lock;
};

}  // namespace

I2cBus::I2cBus() : _lock(xSemaphoreCreateMutexStatic(&_lockStorage)) {}

esp_err_t I2cBus::begin(int sda, int scl, uint32_t hz) {
    Lock lock(_lock);
    if (_bus) return ESP_OK;

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = 1;
#if SOC_I2C_SUPPORT_SLEEP_RETENTION
    cfg.flags.allow_pd = 1;  // the I2C registers are kept across light sleep
#endif
    esp_err_t err = i2c_new_master_bus(&cfg, &_bus);
    // Bus errors are reported by the chip drivers through Logger.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    if (err != ESP_OK) {
        _bus = nullptr;
        return err;
    }
    _hz = hz;
    return ESP_OK;
}

void I2cBus::end() {
    Lock lock(_lock);
    if (!_bus) return;
    for (i2c_master_dev_handle_t& dev : _devices) {
        if (!dev) continue;
        i2c_master_bus_rm_device(dev);
        dev = nullptr;
    }
    if (i2c_del_master_bus(_bus) == ESP_OK) _bus = nullptr;
}

esp_err_t I2cBus::_device(uint8_t address, i2c_master_dev_handle_t& out) {
    if (!_bus) return ESP_ERR_INVALID_STATE;
    if (address >= kAddressCount) return ESP_ERR_INVALID_ARG;
    if (!_devices[address]) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = _hz;
        esp_err_t err = i2c_master_bus_add_device(_bus, &cfg, &_devices[address]);
        if (err != ESP_OK) {
            _devices[address] = nullptr;
            return err;
        }
    }
    out = _devices[address];
    return ESP_OK;
}

esp_err_t I2cBus::probe(uint8_t address) {
    Lock lock(_lock);
    if (!_bus) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(_bus, address, kTimeoutMs);
}

esp_err_t I2cBus::write(uint8_t address, uint8_t reg, const uint8_t* data, size_t len) {
    if (len > kMaxWriteLen) return ESP_ERR_INVALID_SIZE;
    uint8_t frame[1 + kMaxWriteLen];
    frame[0] = reg;
    memcpy(frame + 1, data, len);

    Lock lock(_lock);
    i2c_master_dev_handle_t dev;
    esp_err_t err = _device(address, dev);
    if (err != ESP_OK) return err;
    return i2c_master_transmit(dev, frame, 1 + len, kTimeoutMs);
}

esp_err_t I2cBus::read(uint8_t address, uint8_t reg, uint8_t* data, size_t len) {
    Lock lock(_lock);
    i2c_master_dev_handle_t dev;
    esp_err_t err = _device(address, dev);
    if (err != ESP_OK) return err;
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, kTimeoutMs);
}
