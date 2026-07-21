#pragma once
#include <Arduino.h>

#include "../actions/ActionExecutor.h"
#include "../config/Config.h"
#include "../io/Tca9555Expander.h"

// Polls configured GPIO buttons and detects short press / long press /
// double click, firing the matching ButtonAction through ActionExecutor.
// Loop-polled (house style — no interrupts/IRAM_ATTR anywhere in this
// codebase), modeled on ChannelManager's enum-state + tick() + dwell-timer
// shape.
class ButtonManager {
   public:
    static constexpr uint32_t DEBOUNCE_MS = 30;
    static constexpr uint32_t LONG_PRESS_MS = 600;
    static constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 350;

    void setExecutor(ActionExecutor* executor) { _executor = executor; }

    void begin() { reconfigure(); }

    // Re-reads Config and re-applies pin setup for every configured button.
    // Called after any button add/update/delete. Native-GPIO reconfiguration
    // has no driver library to reinitialize; an expander-backed button just
    // re-runs its (idempotent) pinMode1 setup — either way no device reboot
    // is needed. Assumes the device I2C bus (Wire) is already begun (see
    // main.cpp) when any button uses a TCA9555 expander.
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
            _state[i] = ButtonState{};
        }
    }

    void tick() {
        uint32_t now = millis();
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

    bool _readActive(uint8_t i, const ButtonHardwareConfig& b) {
        bool high = b.viaExpander ? _expanders[i].read(b.pin) : digitalRead(b.pin) == HIGH;
        return b.activeLow ? !high : high;
    }

    void _fire(const ButtonAction& action) {
        if (_executor) _executor->execute(action);
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
                }
                break;

            case ClickState::Pressed:
                if (active && now - s.pressStartMs >= LONG_PRESS_MS) {
                    _fire(b.onLongPress);
                    s.state = ClickState::LongFired;
                    break;
                }
                if (!active) {
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
                    s.lastReleaseMs = 0;
                    s.state = ClickState::Idle;
                }
                break;
        }
    }
};
