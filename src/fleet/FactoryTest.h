#pragma once
#include <stdint.h>

#include <functional>

#include "../game/GameConfig.h"
#include "../game/GameTypes.h"
#include "FleetConfig.h"
#include "FleetRender.h"

// Sequences of the USB factory test (`test leds`, `test keys`, `test charge`,
// docs/usb-link.md). Plain C++: the caller feeds key edges and charger ADC
// samples, draws frame() while running() and reports the results.
namespace fleet {

class FactoryTest {
   public:
    enum class Kind : uint8_t { None, Leds, Keys, Charge };

    struct Io {
        // on: charger at the event target and fast ADC for the charge test;
        // off: back to the stored target and the normal ADC interval.
        std::function<void(bool on)> chargeMode;
    };

    struct Result {
        Kind kind = Kind::None;
        game::ReqId id;
        uint16_t samples = 0;
        int16_t ibusMa = 0;  // means over the samples
        int16_t ibatMa = 0;
        uint16_t vbusMv = 0;
    };

    void begin(const Io& io) { _io = io; }

    bool running() const { return _kind != Kind::None; }
    Kind kind() const { return _kind; }

    bool startLeds(const game::ReqId& id, uint32_t now) {
        if (running()) return false;
        _start(Kind::Leds, id, now);
        return true;
    }

    bool startKeys(const game::ReqId& id, uint32_t now) {
        if (running()) return false;
        _start(Kind::Keys, id, now);
        _keysSeen = 0;
        return true;
    }

    bool startCharge(const game::ReqId& id, uint32_t now) {
        if (running()) return false;
        _start(Kind::Charge, id, now);
        _samples = 0;
        _ibusSum = _ibatSum = 0;
        _ibatCount = 0;
        _vbusSum = 0;
        if (_io.chargeMode) _io.chargeMode(true);
        return true;
    }

    // Ends the key test; false if it was not running.
    bool endKeys() {
        if (_kind != Kind::Keys) return false;
        _kind = Kind::None;
        return true;
    }

    // The link closed: stop whatever runs, without a result.
    void abort() {
        if (_kind == Kind::Charge && _io.chargeMode) _io.chargeMode(false);
        _kind = Kind::None;
    }

    // Key press during the key test: true if it was taken.
    bool onKey(game::Key k) {
        if (_kind != Kind::Keys || (uint8_t)k > (uint8_t)game::Key::Power) return false;
        _keysSeen |= (uint16_t)(1u << (uint8_t)k);
        return true;
    }

    void onAdcSample(int16_t ibusMa, int16_t ibatMa, bool ibatValid, uint16_t vbusMv) {
        if (_kind != Kind::Charge) return;
        _samples++;
        _ibusSum += ibusMa;
        _vbusSum += vbusMv;
        if (ibatValid) {
            _ibatSum += ibatMa;
            _ibatCount++;
        }
    }

    // A finished `leds` or `charge` run, once.
    bool tick(uint32_t now, Result& out) {
        if (_kind == Kind::Leds && now - _startMs >= _ledsTotalMs()) {
            out = Result{};
            out.kind = Kind::Leds;
            out.id = _id;
            _kind = Kind::None;
            return true;
        }
        if (_kind == Kind::Charge && now - _startMs >= kTestChargeMs) {
            out = Result{};
            out.kind = Kind::Charge;
            out.id = _id;
            out.samples = _samples;
            if (_samples > 0) {
                out.ibusMa = (int16_t)(_ibusSum / (int32_t)_samples);
                out.vbusMv = (uint16_t)(_vbusSum / _samples);
            }
            if (_ibatCount > 0) out.ibatMa = (int16_t)(_ibatSum / (int32_t)_ibatCount);
            _kind = Kind::None;
            if (_io.chargeMode) _io.chargeMode(false);
            return true;
        }
        return false;
    }

    uint16_t ibatSamples() const { return _ibatCount; }

    // The frame while running(): `leds` all LEDs white, red, green, blue at
    // lvl, then one white LED chasing in chain order; `keys` the LED
    // of every key pressed so far; `charge` dark (LED current would skew IBUS).
    void frame(uint32_t now, game::Rgb* px, uint8_t lvl) const {
        using game::Color;
        clearFrame(px);
        if (_kind == Kind::Leds) {
            uint32_t t = now - _startMs;
            static const Color kColors[4] = {Color::White, Color::Red, Color::Green, Color::Blue};
            if (t < 4 * kTestColorMs) {
                game::Rgb c = level(game::colorRgb(kColors[t / kTestColorMs]), lvl);
                for (uint16_t i = 0; i < game::kLedCount; i++) px[i] = c;
                return;
            }
            uint32_t i = (t - 4 * kTestColorMs) / kTestChaseStepMs;
            if (i < game::kLedCount) px[i] = level(game::colorRgb(Color::White), lvl);
            return;
        }
        if (_kind != Kind::Keys) return;
        game::Rgb c = level(game::colorRgb(Color::Green), lvl);
        for (uint8_t k = 0; k <= (uint8_t)game::Key::Power; k++) {
            if (!(_keysSeen & (1u << k))) continue;
            _keyLeds(px, (game::Key)k, c);
        }
    }

   private:
    Io _io;
    Kind _kind = Kind::None;
    game::ReqId _id;
    uint32_t _startMs = 0;
    uint16_t _keysSeen = 0;
    uint16_t _samples = 0;
    uint16_t _ibatCount = 0;
    int32_t _ibusSum = 0;
    int32_t _ibatSum = 0;
    uint32_t _vbusSum = 0;

    void _start(Kind k, const game::ReqId& id, uint32_t now) {
        _kind = k;
        _id = id;
        _startMs = now;
    }

    static uint32_t _ledsTotalMs() {
        return 4 * kTestColorMs + (uint32_t)game::kLedCount * kTestChaseStepMs;
    }

    // BUZZ: the ring; the five D-pad keys: their own LED; L1, L2, R1, R2 and
    // POWER (no LED of their own): every other column LED from the USB-C end.
    static void _keyLeds(game::Rgb* px, game::Key k, game::Rgb c) {
        using game::Key;
        switch (k) {
            case Key::Buzz:
                for (uint8_t i : game::kRingLeds) px[i] = c;
                return;
            case Key::Up:
                px[game::kLedUp] = c;
                return;
            case Key::Down:
                px[game::kLedDown] = c;
                return;
            case Key::Left:
                px[game::kLedLeft] = c;
                return;
            case Key::Right:
                px[game::kLedRight] = c;
                return;
            case Key::Enter:
                px[game::kLedEnter] = c;
                return;
            default:
                px[columnLed((uint8_t)(2 * ((uint8_t)k - (uint8_t)Key::L1)))] = c;
                return;
        }
    }
};

}  // namespace fleet
