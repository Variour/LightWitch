#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "../battery/BatteryMonitor.h"
#include "../buttons/ButtonRoles.h"
#include "../config/Config.h"
#include "../lamp/Lamp.h"
#include "../led/LedDriver.h"
#include "../logging/Logger.h"
#include "Lis2dh12.h"

// Steuerboard S: round 1S control board with an ESP32-C3-WROOM-02, TP4056
// charger, 5 V boost for the LEDs, two switched LED outputs and an optional
// LIS2DH12 position sensor (Teelicht, Öllampe, Wurfball, Magische Kugel).
//
// PROVISIONAL pin map: the S schematic is not drawn yet. All pins live here
// and in BoardProfile.cpp; change them together with the schematic.
namespace sboard {

constexpr uint8_t PIN_IMU_INT =
    0;                            // LIS2DH12 INT1, active low; GPIO0-5 wake the C3 from deep sleep
constexpr uint8_t PIN_VBAT = 1;   // VBAT / 2 (1M / 1M, 100 nF), ADC1
constexpr uint8_t PIN_SDA = 2;    // strap pin, held high by the I2C pull-up
constexpr uint8_t PIN_CHRG = 3;   // TP4056 CHRG (low while charging), open drain, pull-up;
                                  // the Qi board's BQ51050B CHG can share it (open drain)
constexpr uint8_t PIN_STDBY = 4;  // TP4056 STDBY (low when full), open drain, pull-up
constexpr uint8_t PIN_SCL = 7;
constexpr uint8_t PIN_SW1 = 9;  // SW1 = BOOT, active low
constexpr uint8_t OUTPUTS = 2;
constexpr uint8_t PIN_EN[OUTPUTS] = {5, 6};      // 5 V switch of LED output A / B, high = on
constexpr uint8_t PIN_DATA[OUTPUTS] = {10, 20};  // LED data A / B
constexpr uint8_t VBAT_DIVIDER = 2;

// Battery thresholds under load. WARN: the light blinks dark three times,
// every WARN_REPEAT_MS. CUTOFF: held for CUTOFF_HOLD_MS, the device goes to
// deep sleep until a charger is connected. The cell's own protection
// (DW01A) cuts off far lower, at about 2.4 V.
constexpr uint16_t WARN_MV = 3500;
constexpr uint16_t CUTOFF_MV = 3300;
constexpr uint32_t CUTOFF_HOLD_MS = 30000;
constexpr uint32_t WARN_REPEAT_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t BLINK_STEP_MS = 250;  // dark, light, dark, light, dark, light

// LED outputs: after switching 5 V on, the ICs get RAIL_SETTLE_MS before
// the first frame; an output that stays dark for RAIL_OFF_DELAY_MS is
// switched off, so dark ICs draw no idle current.
constexpr uint32_t RAIL_SETTLE_MS = 10;
constexpr uint32_t RAIL_OFF_DELAY_MS = 2000;

// SW1 held this long switches the device off (light sleep until SW1,
// motion or the charger).
constexpr uint32_t OFF_HOLD_MS = 2000;
constexpr uint32_t BOOT_GRACE_MS = 300;

constexpr uint32_t IMU_POLL_MS = 100;
constexpr uint32_t UP_POLL_MS = 200;

}  // namespace sboard

class SteuerboardS {
   public:
    static bool active() { return Config::get().board == BoardId::SteuerboardS; }

    // VBAT in mV (0 = no plausible reading) and whether a charger feeds the
    // board (TP4056 charging or full, or the Qi charger on CHRG).
    static BatteryMonitor::ChargerReading reading() {
        BatteryMonitor::ChargerReading r;
        uint32_t sum = 0;
        for (uint8_t i = 0; i < 8; i++) sum += analogReadMilliVolts(sboard::PIN_VBAT);
        uint32_t mv = sum / 8 * sboard::VBAT_DIVIDER;
        r.valid = mv > 2000 && mv < 4600;
        r.vbatMv = r.valid ? (uint16_t)mv : 0;
        r.externalPower = charging() || digitalRead(sboard::PIN_STDBY) == LOW;
        return r;
    }
    static bool charging() { return digitalRead(sboard::PIN_CHRG) == LOW; }

    // After Config::load(), before Wi-Fi: on battery below the cut-off the
    // device goes straight back to deep sleep.
    static void checkBatteryAtBoot() {
        if (!active()) return;
        pinMode(sboard::PIN_CHRG, INPUT_PULLUP);
        pinMode(sboard::PIN_STDBY, INPUT_PULLUP);
        for (uint8_t k = 0; k < sboard::OUTPUTS; k++) {
            pinMode(sboard::PIN_EN[k], OUTPUT);
            digitalWrite(sboard::PIN_EN[k], LOW);
        }
        BatteryMonitor::ChargerReading r = reading();
        Logger::i("[s] boot: VBAT %u mV, %s", r.vbatMv,
                  r.externalPower ? (charging() ? "charging" : "charger, full") : "on battery");
        if (r.valid && !r.externalPower && r.vbatMv < sboard::CUTOFF_MV) {
            Wire.begin(sboard::PIN_SDA, sboard::PIN_SCL);
            _deepSleepUntilCharger();
        }
    }

    // Light slot i was created with driver drv. Outputs start switched off;
    // tick() powers an output once its frame lights any LED.
    void attach(uint8_t light, LedDriver* drv) {
        if (!active() || light >= MAX_LIGHTS) return;
        uint8_t pin = Config::get().lights[light].dataPin;
        for (uint8_t k = 0; k < sboard::OUTPUTS; k++) {
            if (sboard::PIN_DATA[k] != pin) continue;
            _out[k].drv = drv;
            drv->setRailPowered(false);
        }
    }

    // After the I2C bus is up.
    void begin(BatteryMonitor* battery) {
        if (!active()) return;
        _battery = battery;
        _imuOk = _imu.probe(Wire) && _imu.begin();
        if (_imuOk) pinMode(sboard::PIN_IMU_INT, INPUT_PULLUP);
        Logger::i("[s] position sensor %s, motion off after %u s", _imuOk ? "found" : "absent",
                  Config::get().motionOffS);
        _lastMotion = millis();
    }

    // Raw key edges (ButtonManager): the "power" key held OFF_HOLD_MS
    // switches the device off. A press already down at boot (the one that
    // woke the device) does not count.
    void onButtonRaw(uint8_t index, bool pressed, uint32_t ms) {
        if (!active() || index >= MAX_BUTTONS) return;
        if (strcmp(Config::get().buttons[index].role, ButtonRole::POWER) != 0) return;
        _powerDownSince = pressed && _loopMs && ms >= _loopMs + sboard::BOOT_GRACE_MS ? ms : 0;
    }

    // hold: a firmware update is running; the device does not switch off.
    void tick(uint32_t now, bool hold) {
        if (!active()) return;
        if (!_loopMs) _loopMs = now;
        BatteryMonitor::Status bs = _battery ? _battery->status() : BatteryMonitor::Status{};
        bool onBattery = bs.present && bs.state == BatteryMonitor::State::OnBattery;
        uint16_t mv = _battery ? _battery->millivolts() : 0;

        _tickImu(now);
        _tickBattery(now, onBattery && !hold, mv);
        _tickRails(now);
        if (hold) return;

        if (_powerDownSince && now - _powerDownSince >= sboard::OFF_HOLD_MS) {
            Logger::i("[s] SW1 held: off");
            _sleepOff();
        }
        uint16_t offS = Config::get().motionOffS;
        if (_imuOk && offS && onBattery && now - _lastMotion >= (uint32_t)offS * 1000UL) {
            Logger::i("[s] no motion for %u s: off", offS);
            _sleepOff();
        }
    }

   private:
    struct Out {
        LedDriver* drv = nullptr;
        uint8_t state = 0;  // 0 off, 1 settling, 2 on
        uint32_t since = 0;
        uint32_t darkSince = 0;
    };
    Out _out[sboard::OUTPUTS];
    BatteryMonitor* _battery = nullptr;
    Lis2dh12 _imu;
    bool _imuOk = false;
    uint32_t _lastMotion = 0, _lastImuPoll = 0, _lastUpPoll = 0;
    float _up[3] = {0, 0, 1}, _upSet[3] = {0, 0, 1};
    uint32_t _powerDownSince = 0, _loopMs = 0;  // _loopMs: first tick()
    uint32_t _lowSince = 0, _lastWarn = 0, _blinkStart = 0;
    bool _blinking = false;

    void _tickImu(uint32_t now) {
        if (!_imuOk) return;
        if (now - _lastImuPoll >= sboard::IMU_POLL_MS) {
            _lastImuPoll = now;
            if (digitalRead(sboard::PIN_IMU_INT) == LOW && _imu.readMotion()) _lastMotion = now;
        }
        if (!lamp::Lamp::loaded() || now - _lastUpPoll < sboard::UP_POLL_MS) return;
        _lastUpPoll = now;
        float g[3];
        if (!_imu.readG(g)) return;
        // Sensor axes = lamp axes (S lies flat, component side up, +y towards
        // the milled flat). UNVERIFIED: check on the first ball.
        for (uint8_t i = 0; i < 3; i++) _up[i] += (g[i] - _up[i]) * 0.3f;
        float n = sqrtf(_up[0] * _up[0] + _up[1] * _up[1] + _up[2] * _up[2]);
        if (n < 0.5f) return;  // thrown or falling: keep the last up
        float d = (_up[0] * _upSet[0] + _up[1] * _upSet[1] + _up[2] * _upSet[2]) / n;
        if (d > 0.9986f) return;  // within ~3 degrees
        for (uint8_t i = 0; i < 3; i++) _upSet[i] = _up[i] / n;
        lamp::Lamp::setUp(_upSet);
    }

    void _tickBattery(uint32_t now, bool onBattery, uint16_t mv) {
        if (!onBattery || mv == 0) {
            _lowSince = 0;
            _lastWarn = 0;
            return;
        }
        if (mv < sboard::CUTOFF_MV) {
            if (!_lowSince) _lowSince = now;
            if (now - _lowSince >= sboard::CUTOFF_HOLD_MS) {
                Logger::w("[s] battery %u mV: deep sleep until charged", mv);
                _deepSleepUntilCharger();
            }
        } else {
            _lowSince = 0;
        }
        if (mv < sboard::WARN_MV && !_blinking &&
            (_lastWarn == 0 || now - _lastWarn >= sboard::WARN_REPEAT_MS)) {
            Logger::w("[s] battery low: %u mV", mv);
            _lastWarn = now;
            _blinkStart = now;
            _blinking = true;
        }
    }

    void _tickRails(uint32_t now) {
        bool blinkDark = false;
        if (_blinking) {
            uint32_t step = (now - _blinkStart) / sboard::BLINK_STEP_MS;
            if (step >= 6)
                _blinking = false;
            else
                blinkDark = (step % 2) == 0;
        }
        for (uint8_t k = 0; k < sboard::OUTPUTS; k++) {
            Out& o = _out[k];
            if (!o.drv) continue;
            bool lit = o.drv->lit();
            if (blinkDark && o.state != 0) {
                _railOff(k);
                continue;
            }
            if (blinkDark) continue;
            switch (o.state) {
                case 0:
                    if (!lit) break;
                    digitalWrite(sboard::PIN_EN[k], HIGH);
                    o.state = 1;
                    o.since = now;
                    break;
                case 1:
                    if (now - o.since < sboard::RAIL_SETTLE_MS) break;
                    o.drv->setRailPowered(true);
                    o.drv->show();
                    o.state = 2;
                    o.darkSince = 0;
                    break;
                default:
                    if (lit) {
                        o.darkSince = 0;
                    } else if (!o.darkSince) {
                        o.darkSince = now;
                    } else if (now - o.darkSince >= sboard::RAIL_OFF_DELAY_MS) {
                        _railOff(k);
                    }
            }
        }
    }

    // Data low first (the driver stops sending), then 5 V off, so the data
    // line never feeds an unpowered IC.
    void _railOff(uint8_t k) {
        Out& o = _out[k];
        if (o.drv) o.drv->setRailPowered(false);
        digitalWrite(sboard::PIN_EN[k], LOW);
        o.state = 0;
    }

    // Off: LEDs and radio off, light sleep until SW1, motion (motion
    // timeout set and sensor present) or a charger; then a fresh boot. Light
    // sleep because SW1 sits on GPIO9, which cannot wake the C3 from deep
    // sleep.
    void _sleepOff() {
        for (uint8_t k = 0; k < sboard::OUTPUTS; k++) _railOff(k);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        // The key that switched off must be released first, or it wakes the
        // device at once.
        uint32_t t0 = millis();
        while (digitalRead(sboard::PIN_SW1) == LOW && millis() - t0 < 10000) delay(10);
        delay(50);
        bool motion = _imuOk && Config::get().motionOffS > 0;
        if (motion) {
            _imu.readMotion();
            gpio_wakeup_enable((gpio_num_t)sboard::PIN_IMU_INT, GPIO_INTR_LOW_LEVEL);
        } else {
            _imu.powerDown();
        }
        gpio_wakeup_enable((gpio_num_t)sboard::PIN_SW1, GPIO_INTR_LOW_LEVEL);
        // A charger already connected would wake it at once.
        if (!charging()) gpio_wakeup_enable((gpio_num_t)sboard::PIN_CHRG, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        Serial.flush();
        esp_light_sleep_start();
        esp_restart();
    }

    // Deep sleep until the charger pulls CHRG low (GPIO3 is a deep-sleep
    // wake pin). The LED switches' gate pulldowns keep the outputs off.
    // Wire must be begun.
    static void _deepSleepUntilCharger() {
        Lis2dh12 imu;
        if (imu.probe(Wire)) imu.powerDown();
        Serial.flush();
        esp_deep_sleep_enable_gpio_wakeup(1ULL << sboard::PIN_CHRG, ESP_GPIO_WAKEUP_GPIO_LOW);
        esp_deep_sleep_start();
    }
};
