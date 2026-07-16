#pragma once
#include <Arduino.h>

// Reads the optional battery-voltage sense line (see hardware note below) and
// derives a percentage + power-source state from it.
//
// Hardware: BAT (battery +) is divided 200 kΩ / 100 kΩ (±1% each) down to a
// net called BAT_ADC, which can be solder-bridged onto GPIO1. Vbat = Vadc *
// (200+100)/100 = Vadc * 3.
//
// The charger IC (ETA6098) does have a STAT charge-status pin, but per the
// schematic it only drives a dedicated LED (through a 27 kΩ resistor) —
// it isn't broken out to any GPIO, so firmware has no way to read it.
// BAT_ADC is the only signal available. That means "running on battery" vs.
// "connected to mains" can only be inferred from the voltage itself: a
// charger (or a mains-fed float rail) holds BAT_ADC at/near the Li-ion
// charge-termination voltage, while a battery discharging on its own settles
// below it. This is a heuristic, not a true charge-status reading — a
// battery resting right at full charge with the charger already removed
// reads the same as "charging".
class BatteryMonitor {
   public:
    enum class State : uint8_t { OnBattery = 0, Charging = 1 };

    struct Status {
        bool present = false;  // monitoring active (HW capable + enabled in Config)
        uint8_t percent = 0;   // 0-100, only meaningful if present
        State state = State::OnBattery;
    };

    static constexpr bool kHwSupported =
#ifdef BATTERY_ADC_PIN
        true;
#else
        false;
#endif

    void begin(bool enabled) {
        _enabled = enabled && kHwSupported;
#ifdef BATTERY_ADC_PIN
        if (_enabled) {
            analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
        }
#endif
    }

    // Call regularly (e.g. every loop() background tick); internally rate-limited.
    void tick() {
#ifdef BATTERY_ADC_PIN
        if (!_enabled) return;
        uint32_t now = millis();
        if (_lastSampleMs != 0 && now - _lastSampleMs < SAMPLE_INTERVAL_MS) return;
        _lastSampleMs = now;

        uint32_t sumMv = 0;
        for (uint8_t i = 0; i < SAMPLE_COUNT; i++) sumMv += analogReadMilliVolts(BATTERY_ADC_PIN);
        uint16_t pinMv = (uint16_t)(sumMv / SAMPLE_COUNT);
        uint16_t battMv = (uint16_t)(pinMv * DIVIDER_RATIO_X10 / 10);

        // Exponential smoothing across ticks to damp ADC noise near threshold
        // boundaries (avoids flickering between states/percent).
        _smoothedMv = (_smoothedMv == 0) ? battMv : (uint16_t)((_smoothedMv * 7 + battMv * 3) / 10);

        _status.present = true;
        _status.state = _smoothedMv >= CHARGING_THRESHOLD_MV ? State::Charging : State::OnBattery;
        _status.percent = _voltageToPercent(_smoothedMv);
#endif
    }

    const Status& status() const { return _status; }
    uint16_t millivolts() const { return _smoothedMv; }

   private:
    static constexpr uint32_t SAMPLE_INTERVAL_MS = 2000;
    static constexpr uint8_t SAMPLE_COUNT = 16;
    // (200k + 100k) / 100k, x10 to stay integer: 30 → ratio 3.0
    static constexpr uint32_t DIVIDER_RATIO_X10 = 30;
    // At/above this, treat BAT_ADC as being actively held up by a charger/
    // mains-fed rail rather than a battery discharging on its own.
    static constexpr uint16_t CHARGING_THRESHOLD_MV = 4100;

    bool _enabled = false;
    uint32_t _lastSampleMs = 0;
    uint16_t _smoothedMv = 0;
    Status _status;

    // Piecewise-linear approximation of a generic 1S Li-ion/LiPo open-circuit
    // discharge curve (3.0-4.2V), most-charged first.
    struct CurvePoint {
        uint16_t mv;
        uint8_t pct;
    };
    static constexpr CurvePoint kCurve[] = {
        {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
        {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
        {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3270, 0},
    };

    static uint8_t _voltageToPercent(uint16_t mv) {
        constexpr size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
        if (mv >= kCurve[0].mv) return 100;
        if (mv <= kCurve[n - 1].mv) return 0;
        for (size_t i = 1; i < n; i++) {
            if (mv >= kCurve[i].mv) {
                uint16_t hiMv = kCurve[i - 1].mv, loMv = kCurve[i].mv;
                uint8_t hiPct = kCurve[i - 1].pct, loPct = kCurve[i].pct;
                return (uint8_t)(loPct + (uint32_t)(mv - loMv) * (hiPct - loPct) / (hiMv - loMv));
            }
        }
        return 0;
    }
};
