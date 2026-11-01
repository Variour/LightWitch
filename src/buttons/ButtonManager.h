#pragma once
#include <Arduino.h>

#include <atomic>
#include <functional>

#include "../actions/ActionExecutor.h"
#include "../config/Config.h"
#include "../io/Tca9555Expander.h"

// Polls configured GPIO buttons and detects short press / long press /
// double click, firing the matching ButtonAction through ActionExecutor.
// Loop-polled (house style — no interrupts/IRAM_ATTR anywhere in this
// codebase), modeled on ChannelManager's enum-state + tick() + dwell-timer
// shape.
//
// Alongside that, every debounced press and release edge is reported raw
// (see setRawCallback), without waiting for the long-press or double-click
// decision.
class ButtonManager {
   public:
    static constexpr uint32_t DEBOUNCE_MS = 30;
    static constexpr uint32_t LONG_PRESS_MS = 600;
    static constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 350;

    // index: button slot (see Config::buttonIndexForRole to find a key by
    // role). pressed: true on the press edge, false on the release edge.
    // ms: millis() at which the edge was first sampled — for a press that is
    // the start of its DEBOUNCE_MS window, so the event itself arrives
    // DEBOUNCE_MS (+ one tick) later. Presses and releases of a button always
    // alternate, starting with a press. Called from tick() only, i.e. on the
    // loop task.
    using RawCallback = std::function<void(uint8_t index, bool pressed, uint32_t ms)>;

    void setExecutor(ActionExecutor* executor) { _executor = executor; }

    // Single consumer: main.cpp owns it and fans out to every module that
    // needs raw key edges.
    void setRawCallback(RawCallback cb) { _rawCb = cb; }

    void begin() { reconfigure(); }

    // Re-reads Config and re-applies pin setup for every configured button.
    // Called after any button add/update/delete. Native-GPIO reconfiguration
    // has no driver library to reinitialize; an expander-backed button just
    // re-runs its (idempotent) pinMode1 setup — either way no device reboot
    // is needed. Assumes the device I2C bus (Wire) is already begun (see
    // main.cpp) when any button uses a TCA9555 expander.
    //
    // A button that was held when this runs gets its raw release reported on
    // the next tick(), so raw consumers never see a press without a release.
    void reconfigure() {
        for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
            auto& b = Config::get().buttons[i];
            if (b.exists) {
                if (b.viaExpander) {
                    _expanders[i].setup(Config::get().expanderAddress);
                    _expanders[i].beginInput(b.pin);
                } else {
                    pinMode(b.pin, b.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
                }
            }
            if (_isHeld(_state[i].state)) _pendingRelease.fetch_or((uint16_t)(1u << i));
            _state[i] = ButtonState{};
        }
    }

    void tick() {
        uint32_t now = millis();
        // reconfigure() runs on the web server task; exchange() keeps a bit
        // it sets meanwhile for the next tick.
        uint16_t pending = _pendingRelease.exchange(0);
        for (uint8_t i = 0; pending && i < MAX_BUTTONS; i++)
            if (pending & (1u << i)) _raw(i, false, now);
        Config::forEachButton([&](uint8_t i, ButtonHardwareConfig& b) { _tickButton(i, b, now); });
    }

   private:
    enum class ClickState : uint8_t { Idle, Debounce, Pressed, WaitDoubleClick, LongFired };

    struct ButtonState {
        ClickState state = ClickState::Idle;
        uint32_t debounceStart = 0;
        uint32_t pressStartMs = 0;
        uint32_t lastReleaseMs = 0;
    };

    ButtonState _state[MAX_BUTTONS];
    Tca9555Expander _expanders[MAX_BUTTONS];
    ActionExecutor* _executor = nullptr;
    RawCallback _rawCb;
    // Buttons whose raw release reconfigure() owes the raw consumer.
    std::atomic<uint16_t> _pendingRelease{0};
    static_assert(MAX_BUTTONS <= 16, "_pendingRelease holds one bit per button");

    // States in which a raw press has been reported and its release has not.
    static bool _isHeld(ClickState s) {
        return s == ClickState::Pressed || s == ClickState::LongFired;
    }

    bool _readActive(uint8_t i, const ButtonHardwareConfig& b) {
        bool high = b.viaExpander ? _expanders[i].read(b.pin) : digitalRead(b.pin) == HIGH;
        return b.activeLow ? !high : high;
    }

    void _fire(const ButtonAction& action) {
        if (_executor) _executor->execute(action);
    }

    void _raw(uint8_t i, bool pressed, uint32_t ms) {
        if (_rawCb) _rawCb(i, pressed, ms);
    }

    void _tickButton(uint8_t i, ButtonHardwareConfig& b, uint32_t now) {
        auto& s = _state[i];
        bool active = _readActive(i, b);

        switch (s.state) {
            case ClickState::Idle:
                if (active) {
                    s.state = ClickState::Debounce;
                    s.debounceStart = now;
                }
                break;

            case ClickState::Debounce:
                if (!active) {
                    s.state = ClickState::Idle;
                    break;
                }
                if (now - s.debounceStart >= DEBOUNCE_MS) {
                    s.state = ClickState::Pressed;
                    s.pressStartMs = now;
                    _raw(i, true, s.debounceStart);
                }
                break;

            case ClickState::Pressed:
                if (active && now - s.pressStartMs >= LONG_PRESS_MS) {
                    _fire(b.onLongPress);
                    s.state = ClickState::LongFired;
                    break;
                }
                if (!active) {
                    _raw(i, false, now);
                    if (s.lastReleaseMs != 0 && now - s.lastReleaseMs <= DOUBLE_CLICK_WINDOW_MS) {
                        _fire(b.onDoubleClick);
                        s.lastReleaseMs = 0;
                        s.state = ClickState::Idle;
                    } else {
                        s.lastReleaseMs = now;
                        s.state = ClickState::WaitDoubleClick;
                    }
                }
                break;

            case ClickState::WaitDoubleClick:
                if (active) {
                    // Second press within the window — debounce it like any other
                    // press; the double-click fires on its release (Pressed state
                    // above still has lastReleaseMs from the first click).
                    s.state = ClickState::Debounce;
                    s.debounceStart = now;
                    break;
                }
                if (now - s.lastReleaseMs > DOUBLE_CLICK_WINDOW_MS) {
                    _fire(b.onShortPress);
                    s.lastReleaseMs = 0;
                    s.state = ClickState::Idle;
                }
                break;

            case ClickState::LongFired:
                if (!active) {
                    _raw(i, false, now);
                    s.lastReleaseMs = 0;
                    s.state = ClickState::Idle;
                }
                break;
        }
    }
};
