#pragma once
#include <stdint.h>

class TwoWire;

// TI TMAG5273 3-axis Hall sensor on I2C, the triggers on Buzzerboard Rev 2
// (U6 at 0x35, TMAG5273A1; U7 at 0x44, TMAG5273D1; both on one INT line).
// Polling only: begin() starts continuous X/Y/Z conversion at the default
// range (+-40 mT on the x1 variants) and no averaging. The caller begins the
// bus (Wire) first; the sensor shares it with the charger and the IMU.
class Tmag5273 {
   public:
    struct Field {
        int16_t x = 0;  // raw, full scale +-32768 = +-rangeMt()
        int16_t y = 0;
        int16_t z = 0;
    };

    // Checks the manufacturer ID and starts continuous conversion.
    bool begin(TwoWire& wire, uint8_t address);
    bool online() const { return _online; }

    // The latest conversion.
    bool read(Field& out);

    // Full scale in mT for the current range: 40 or 80 (x1), 133 or 266 (x2).
    uint16_t rangeMt() const { return _rangeMt; }

   private:
    bool _write(uint8_t reg, uint8_t value);
    bool _read(uint8_t reg, uint8_t* buf, uint8_t len);

    TwoWire* _wire = nullptr;
    uint8_t _address = 0;
    bool _online = false;
    uint16_t _rangeMt = 0;
};
