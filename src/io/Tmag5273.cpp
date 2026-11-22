#include "Tmag5273.h"

#include <Wire.h>

namespace {
constexpr uint8_t kDeviceConfig2 = 0x01;
constexpr uint8_t kSensorConfig1 = 0x02;
constexpr uint8_t kDeviceId = 0x0D;
constexpr uint8_t kManufacturerIdLsb = 0x0E;
constexpr uint8_t kXMsb = 0x12;

constexpr uint16_t kManufacturerId = 0x5449;  // "TI"
constexpr uint8_t kModeContinuous = 0x02;     // DEVICE_CONFIG_2 OPERATING_MODE
constexpr uint8_t kChannelsXyz = 0x70;        // SENSOR_CONFIG_1 MAG_CH_EN = 7h
}  // namespace

bool Tmag5273::begin(TwoWire& wire, uint8_t address) {
    _wire = &wire;
    _address = address;
    _online = false;

    uint8_t id[2];
    if (!_read(kManufacturerIdLsb, id, 2)) return false;
    if ((uint16_t(id[1]) << 8 | id[0]) != kManufacturerId) return false;

    uint8_t version;
    if (!_read(kDeviceId, &version, 1)) return false;
    _rangeMt = (version & 0x03) == 0x02 ? 133 : 40;  // default range, Z_RANGE/X_Y_RANGE = 0

    if (!_write(kSensorConfig1, kChannelsXyz)) return false;
    if (!_write(kDeviceConfig2, kModeContinuous)) return false;
    _online = true;
    return true;
}

bool Tmag5273::read(Field& out) {
    if (!_online) return false;
    uint8_t b[6];
    if (!_read(kXMsb, b, 6)) return false;
    out.x = int16_t(uint16_t(b[0]) << 8 | b[1]);
    out.y = int16_t(uint16_t(b[2]) << 8 | b[3]);
    out.z = int16_t(uint16_t(b[4]) << 8 | b[5]);
    return true;
}

bool Tmag5273::_write(uint8_t reg, uint8_t value) {
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write(value);
    return _wire->endTransmission() == 0;
}

bool Tmag5273::_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    _wire->beginTransmission(_address);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(_address, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = _wire->read();
    return true;
}
