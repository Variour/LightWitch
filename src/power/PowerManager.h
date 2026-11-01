#pragma once
#include <Arduino.h>
#include <math.h>

#include <functional>

#include "../battery/BatteryMonitor.h"
#include "../buttons/ButtonRoles.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "Bq25628.h"

// Board power management on top of the BQ25628 charger
// (DeviceConfig::chargerEnabled): keeps the charger in host mode, powers the
// LED rail from the battery, feeds BatteryMonitor, and switches the device
// off (ship mode) on a long press of the "power" key or when the battery is
// low. Polled from loop() like every other module; no interrupts.
//
// LED rail: the LEDs hang on the charger's PMID node behind the PMID_GD
// switch. With USB present PMID is VBUS. On battery PMID only has power while
// boost OTG runs, so OTG is switched on whenever the charger reports no
// adapter and off whenever it reports one. The charger itself never drives
// VBUS: boost only starts while VBUS < VBAT + VSLEEP, an adapter plug-in ends
// it, and the input FET stays off in OTG (BQ25628 SLUSEG4C §8.3.6.1,
// §8.3.10.3.1).
class PowerManager {
   public:
    // ICHG: the board docs give "up to ~1 A for 2 x 18650" (Buzzerboard
    // hardware/README.md, Open points 2), i.e. <= 0.5 C per cell for any
    // 18650 of 2 Ah or more. On a default USB source the ILIM pin's 446 mA
    // input limit keeps the real charge current lower anyway.
    static constexpr uint16_t CHARGE_CURRENT_MA = 1000;
    // Li-ion charge voltage, 4.20 V (board README, charger table and
    // "Over-charge: VREG 4.20 V regulation").
    static constexpr uint16_t CHARGE_VOLTAGE_MV = 4200;
    // Boost output on PMID: 5.04 V (the POR value) gives the LEDs full colour
    // (board README, "LED rail = PMID"); bypass (PMID = VBAT) dims blue below
    // ~3.7 V.
    static constexpr uint16_t OTG_VOLTAGE_MV = 5040;

    // Power key hold time for "off". Above the ship-mode exit time on the
    // same key (tSM_EXIT 0.55-0.93 s, §8.3.9.2), so the press that switches
    // the device on cannot also switch it off, and far below the charger's
    // hardware power reset on that key (tQON_RST >= 9 s, Table 8-6).
    static constexpr uint32_t POWER_KEY_HOLD_MS = 3000;
    // A power-key press that starts within this time of the first loop pass
    // was already held at boot (the press that woke the device) and does not
    // count.
    static constexpr uint32_t POWER_KEY_BOOT_GRACE_MS = 100;

    // Low-battery cut-off: ship mode once VBAT has stayed below this for
    // LOW_BATTERY_HOLD_MS on battery (board README, "Firmware cut-off (the
    // real one)"). The hold time rides out sag from LED and Wi-Fi load peaks.
    static constexpr uint16_t LOW_BATTERY_MV = 3300;
    static constexpr uint32_t LOW_BATTERY_HOLD_MS = 30000;

    // blank = true: show nothing until called again with false (see main.cpp).
    using LedBlankFn = std::function<void(bool blank)>;
    // The LED rail just got power: resend the current frame.
    using LedRailUpFn = std::function<void()>;

    void setOnLedBlank(LedBlankFn fn) { _onLedBlank = fn; }
    void setOnLedRailUp(LedRailUpFn fn) { _onLedRailUp = fn; }

    // Call once the device I2C bus (Wire) is up.
    void begin(bool enabled) {
        _enabled = enabled;
        if (!_enabled) return;
        auto& c = Config::get();
        if (c.i2cSdaPin == PIN_UNUSED || c.i2cSclPin == PIN_UNUSED) {
            Logger::e("[pwr] charger enabled but no I2C bus configured");
            _enabled = false;
            return;
        }
        _connect(millis());
    }

    // Call on every loop() pass, including while an HTTP update downloads,
    // so the watchdog keeps getting reset; internally rate-limited.
    void tick() {
        uint32_t now = millis();
        if (_firstTickMs == 0) _firstTickMs = now;
        if (!_enabled) return;
        _tickPowerKey(now);
        if (now - _lastPollMs < POLL_INTERVAL_MS) return;
        _lastPollMs = now;

        if (!_online) {
            if (now - _lastProbeMs >= PROBE_INTERVAL_MS) _connect(now);
            return;
        }
        Bq25628::Status st;
        if (!_chg.readStatus(st)) {
            _goOffline("status read failed");
            return;
        }
        if (st.watchdogExpired() && now - _lastConfigureMs >= RECONFIGURE_MIN_MS) {
            // Default mode: watchdog expired (or the charger reset), so EN_OTG,
            // ADC_EN, EN_EXTILIM, ... are back at their defaults and ICHG is
            // halved (§8.4.1). Configure again; the next poll picks up.
            Logger::w("[pwr] charger in default mode (watchdog) - configuring again");
            _lastConfigureMs = now;
            if (!_chg.configure(kSettings)) _goOffline("configure failed");
            _lastKickMs = now;
            _adcBusy = false;
            return;
        }
        if (now - _lastKickMs >= WATCHDOG_KICK_MS && _chg.kickWatchdog()) _lastKickMs = now;

        _logChanges(st);
        _status = st;
        _hasStatus = true;
        _updateOtg(st, now);
        _updateLedRail(st, now);
        _updateTsAdc(st);
        _tickAdc(now);
        _tickShipMode(now);
    }

    // Raw key edges from ButtonManager (see main.cpp): watches the key with
    // role "power".
    void onButtonRaw(uint8_t index, bool pressed, uint32_t ms) {
        if (!_enabled) return;
        if ((int8_t)index != Config::buttonIndexForRole(ButtonRole::POWER)) return;
        if (pressed) {
            if (ms - _firstTickMs < POWER_KEY_BOOT_GRACE_MS) {
                Logger::d("[pwr] power key held since boot - ignored until released");
                return;
            }
            _keyDown = true;
            _keyDownMs = ms;
            _keyArmed = false;
            return;
        }
        bool armed = _keyArmed;
        _keyDown = false;
        _keyArmed = false;
        // Enter ship mode on release: QON low for tSM_EXIT leaves ship mode
        // (§8.3.9.2), so the key has to be up first.
        if (armed) _shutdown("power key");
    }

    // Battery source for BatteryMonitor::setChargerSource().
    BatteryMonitor::ChargerReading batteryReading() const {
        BatteryMonitor::ChargerReading r;
        r.valid = _online && _hasStatus && _adcMs != 0 && millis() - _adcMs < ADC_STALE_MS;
        r.vbatMv = _adc.vbatMv;
        r.externalPower = _status.powerGood();
        return r;
    }

    bool online() const { return _online; }
    const Bq25628::Status& status() const { return _status; }
    // Last completed ADC conversion; adcAgeMs() tells how old it is.
    const Bq25628::Adc& adc() const { return _adc; }
    uint32_t adcAgeMs() const { return _adcMs ? millis() - _adcMs : UINT32_MAX; }

   private:
    static constexpr Bq25628::Settings kSettings = {CHARGE_CURRENT_MA, CHARGE_VOLTAGE_MV,
                                                    OTG_VOLTAGE_MV};

    static constexpr uint32_t POLL_INTERVAL_MS = 200;
    static constexpr uint32_t PROBE_INTERVAL_MS = 5000;
    // Watchdog is 200 s nominal, 136 s minimum (tWDT, §7.5), reset every 10 s
    // from loop(). An HTTP update (Updater) runs in its own task and loop()
    // keeps resetting it. ArduinoOTA blocks loop() for its whole transfer, so
    // only transfers shorter than 136 s keep host mode; after a longer one
    // tick() finds the charger in default mode and configures it again.
    static constexpr uint32_t WATCHDOG_KICK_MS = 10000;
    static constexpr uint32_t OTG_RETRY_MS = 5000;
    // At most one re-configure per this time; the rest of tick() keeps
    // running while WD_STAT stays set.
    static constexpr uint32_t RECONFIGURE_MIN_MS = 1000;
    // Time between the rail coming up and the frame resend.
    // UNVERIFIED: the datasheet gives no PMID_GD delay after VBUS_STAT turns
    // OTG; 100 ms is a margin, check the first frame on battery.
    static constexpr uint32_t LED_RAIL_SETTLE_MS = 100;
    static constexpr uint32_t ADC_INTERVAL_MS = 2000;
    // 8 channels x 30 ms at 12 bit (tADC_CONV, §7.5) = 240 ms.
    static constexpr uint32_t ADC_CONVERSION_MS = 250;
    static constexpr uint32_t ADC_TIMEOUT_MS = 1500;
    static constexpr uint32_t ADC_STALE_MS = 10000;
    static constexpr uint32_t ADC_LOG_INTERVAL_MS = 30000;
    // Time after a ship-mode request (BATFET_DLY 25 ms) after which a still
    // running device withdraws it.
    static constexpr uint32_t SHIP_MODE_TIMEOUT_MS = 2000;

    // Buzzerboard TS network (hardware/gen/buzzer.py block 2): TS_BIAS -R11
    // 5.1k- TS, TS -R12 27k- GND, TS -TH1 10k B3435 NTC- GND.
    // UNVERIFIED: the conversion below takes the TS ADC's "bias reference"
    // (Table 8-43) to be the TS_BIAS pin voltage; compare with a thermometer.
    static constexpr float TS_RT1_OHM = 5100.0f;
    static constexpr float TS_RT2_OHM = 27000.0f;
    static constexpr float TS_NTC_R25_OHM = 10000.0f;
    static constexpr float TS_NTC_BETA = 3435.0f;

    Bq25628 _chg;
    bool _enabled = false;
    bool _online = false;
    bool _hasStatus = false;
    Bq25628::Status _status;
    uint32_t _firstTickMs = 0;
    uint32_t _lastPollMs = 0;
    uint32_t _lastProbeMs = 0;
    uint32_t _lastKickMs = 0;
    uint32_t _lastConfigureMs = 0;
    uint32_t _lastOtgAttemptMs = 0;
    bool _otgFailLogged = false;

    bool _railUp = false;
    bool _railRefreshPending = false;
    uint32_t _railUpMs = 0;

    int8_t _tsAdcOn = -1;  // -1 unknown, else 0/1
    bool _adcBusy = false;
    uint32_t _adcStartMs = 0;
    uint32_t _lastAdcStartMs = 0;
    uint32_t _adcMs = 0;
    uint32_t _lastAdcLogMs = 0;
    Bq25628::Adc _adc;
    uint32_t _lowSinceMs = 0;

    bool _keyDown = false;
    bool _keyArmed = false;
    uint32_t _keyDownMs = 0;

    bool _shipPending = false;
    uint32_t _shipRequestedMs = 0;
    bool _blanked = false;

    LedBlankFn _onLedBlank;
    LedRailUpFn _onLedRailUp;

    void _connect(uint32_t now) {
        _lastProbeMs = now;
        if (!_chg.probe()) {
            if (_online || _lastPollMs == 0) Logger::w("[pwr] no BQ25628 at I2C 0x6A");
            _online = false;
            return;
        }
        _lastConfigureMs = now;
        if (!_chg.configure(kSettings)) {
            Logger::e("[pwr] BQ25628 configuration failed - charger stays offline");
            return;
        }
        _online = true;
        _lastKickMs = now;
        _adcBusy = false;
        _tsAdcOn = -1;
        // Power the LED rail right away when on battery, before the first
        // frame goes out.
        Bq25628::Status st;
        _hasStatus = false;
        if (_chg.readStatus(st)) {
            _logChanges(st);
            _status = st;
            _hasStatus = true;
            _updateOtg(st, now);
        }
    }

    void _goOffline(const char* why) {
        Logger::w("[pwr] charger offline: %s", why);
        _online = false;
        _hasStatus = false;
        _adcBusy = false;
        _lastProbeMs = millis();
    }

    void _blank(bool blank) {
        if (blank == _blanked) return;
        _blanked = blank;
        if (_onLedBlank) _onLedBlank(blank);
    }

    void _updateOtg(const Bq25628::Status& st, uint32_t now) {
        if (st.powerGood()) {
            if (st.otgEnabled()) {
                _chg.setOtg(false);
                Logger::i("[pwr] USB adapter present - OTG off, LEDs on VBUS");
            }
            _otgFailLogged = false;
            return;
        }
        if (_shipPending || st.otgEnabled() || st.vbusStat() != Bq25628::VBUS_STAT_NONE) return;
        // On battery with OTG off: switch it on. The charger clears EN_OTG
        // again by itself on a fault (VBAT below VBAT_OTG, PMID undervoltage,
        // TS outside the OTG window, ... §8.3.10.3), so retry at a slow pace.
        if (_lastOtgAttemptMs != 0 && now - _lastOtgAttemptMs < OTG_RETRY_MS) return;
        bool retry = _lastOtgAttemptMs != 0;
        _lastOtgAttemptMs = now;
        if (!_chg.setOtg(true)) return;
        if (!retry) {
            Logger::i("[pwr] on battery - boost OTG on (%u mV) for the LEDs", OTG_VOLTAGE_MV);
        } else if (!_otgFailLogged) {
            _otgFailLogged = true;
            Logger::w("[pwr] OTG did not stay on (fault 0x%02X) - retrying every %lu s", st.fault,
                      (unsigned long)(OTG_RETRY_MS / 1000));
        }
    }

    void _updateLedRail(const Bq25628::Status& st, uint32_t now) {
        bool up = st.powerGood() || st.otgActive();
        if (up && !_railUp) {
            _railUpMs = now;
            _railRefreshPending = true;
        }
        if (up && st.otgActive()) {
            _otgFailLogged = false;
            _lastOtgAttemptMs = 0;
        }
        _railUp = up;
        if (_railRefreshPending && up && now - _railUpMs >= LED_RAIL_SETTLE_MS) {
            _railRefreshPending = false;
            if (_onLedRailUp) _onLedRailUp();
        }
    }

    // Convert the TS channel on USB only. On battery the ADC runs down to
    // VBAT_LOWV (2.8 V) without it and only down to 3.2 V with it (§8.3.7);
    // the cut-off reads VBAT below 3.3 V.
    void _updateTsAdc(const Bq25628::Status& st) {
        int8_t want = st.powerGood() ? 1 : 0;
        if (want == _tsAdcOn) return;
        if (_chg.setTsAdc(want == 1)) _tsAdcOn = want;
    }

    void _tickAdc(uint32_t now) {
        if (_adcBusy) {
            if (now - _adcStartMs < ADC_CONVERSION_MS) return;
            bool done = false;
            if (!_chg.adcDone(done)) return;
            if (!done) {
                if (now - _adcStartMs >= ADC_TIMEOUT_MS) {
                    Logger::w("[pwr] ADC conversion timed out");
                    _adcBusy = false;
                }
                return;
            }
            _adcBusy = false;
            Bq25628::Adc a;
            if (!_chg.readAdc(a)) return;
            _adc = a;
            _adcMs = now;
            _onVbatSample(now, true, a.vbatMv);
            _logAdc(now);
            return;
        }
        if (_lastAdcStartMs != 0 && now - _lastAdcStartMs < ADC_INTERVAL_MS) return;
        _lastAdcStartMs = now;
        if (_chg.startAdcOneShot()) {
            _adcBusy = true;
            _adcStartMs = now;
            return;
        }
        // Refused (§8.3.7): neither VBUS nor VBAT above its ADC threshold.
        // On battery with the TS channel off that threshold is VBAT_LOWV
        // (2.7-2.9 V), so the battery is below the cut-off.
        Logger::w("[pwr] ADC conversion refused");
        _onVbatSample(now, false, 0);
    }

    // One VBAT sample for the low-battery cut-off; measured = false for a
    // refused conversion on battery (see _tickAdc).
    void _onVbatSample(uint32_t now, bool measured, uint16_t vbatMv) {
        if (_status.powerGood() || _shipPending) {
            _lowSinceMs = 0;
            return;
        }
        if (measured && vbatMv >= LOW_BATTERY_MV) {
            _lowSinceMs = 0;
            return;
        }
        if (_lowSinceMs == 0) {
            _lowSinceMs = now;
            if (measured)
                Logger::w("[pwr] VBAT %u mV below %u mV on battery", vbatMv, LOW_BATTERY_MV);
            else
                Logger::w("[pwr] no VBAT reading on battery - below the ADC threshold");
            return;
        }
        if (now - _lowSinceMs >= LOW_BATTERY_HOLD_MS) _shutdown("low battery");
    }

    void _tickPowerKey(uint32_t now) {
        if (!_keyDown || _keyArmed || now - _keyDownMs < POWER_KEY_HOLD_MS) return;
        if (!_online || !_hasStatus) {
            Logger::w("[pwr] power key held: charger not available, staying on");
            _keyDown = false;
            return;
        }
        if (_status.powerGood()) {
            Logger::i("[pwr] power key held: USB present, ship mode needs battery power");
            _keyDown = false;
            return;
        }
        _keyArmed = true;
        _blank(true);
        Logger::i("[pwr] power key held %lu s - release to switch off",
                  (unsigned long)(POWER_KEY_HOLD_MS / 1000));
    }

    void _shutdown(const char* reason) {
        if (_shipPending) return;
        if (!_online || !_hasStatus || _status.powerGood()) {
            Logger::i("[pwr] %s: no ship mode (charger unavailable or USB present)", reason);
            _blank(false);
            return;
        }
        _blank(true);
        Logger::w("[pwr] %s: entering ship mode (wake: hold the power key ~1 s or plug in USB)",
                  reason);
        if (!_chg.requestShipMode()) {
            Logger::e("[pwr] ship mode request failed");
            _blank(false);
            return;
        }
        _shipPending = true;
        _shipRequestedMs = millis();
    }

    void _tickShipMode(uint32_t now) {
        if (!_shipPending || now - _shipRequestedMs < SHIP_MODE_TIMEOUT_MS) return;
        // Withdraw the request (Table 8-6), so only a new request switches
        // the device off, also after an adapter is removed.
        Logger::w("[pwr] ship mode did not engage - cancelled");
        _chg.cancelShipMode();
        _shipPending = false;
        _lowSinceMs = 0;
        _blank(false);
    }

    void _logChanges(const Bq25628::Status& st) {
        if (_hasStatus && st.stat1 == _status.stat1 && st.fault == _status.fault) return;
        static const char* const kCharge[] = {"not charging", "fast (CC)", "taper (CV)", "top-off"};
        const char* vbus = st.powerGood()                             ? "adapter (power good)"
                           : st.otgActive()                           ? "OTG"
                           : st.vbusStat() == Bq25628::VBUS_STAT_NONE ? "none"
                                                                      : "other";
        Logger::i("[pwr] VBUS %s, charge %s, fault 0x%02X, TS zone %u", vbus,
                  kCharge[(uint8_t)st.chargeState()], st.fault & ~Bq25628::FAULT_TS_MASK,
                  st.tsStat());
    }

    void _logAdc(uint32_t now) {
        if (_lastAdcLogMs != 0 && now - _lastAdcLogMs < ADC_LOG_INTERVAL_MS) return;
        _lastAdcLogMs = now;
        char ts[32] = "off";
        int16_t tsDeciC;
        if (_tsAdcOn == 1 && _tsToDeciC(_adc.tsPermille, tsDeciC))
            snprintf(ts, sizeof(ts), "%.1f%% (%.1f C)", _adc.tsPermille / 10.0f, tsDeciC / 10.0f);
        Logger::d(
            "[pwr] VBAT %u mV, IBAT %d mA, VSYS %u mV, VBUS %u mV, IBUS %d mA, PMID %u mV, "
            "TS %s, TDIE %.1f C",
            _adc.vbatMv, _adc.ibatValid ? _adc.ibatMa : 0, _adc.vsysMv, _adc.vbusMv, _adc.ibusMa,
            _adc.vpmidMv, ts, _adc.tdieDeciC / 10.0f);
    }

    // TS pin share of the TS bias -> NTC temperature on the network above.
    static bool _tsToDeciC(uint16_t permille, int16_t& out) {
        if (permille == 0 || permille >= 1000) return false;
        float p = permille / 1000.0f;
        float rLow = TS_RT1_OHM * p / (1.0f - p);  // R12 || TH1
        if (rLow >= TS_RT2_OHM) return false;
        float rNtc = 1.0f / (1.0f / rLow - 1.0f / TS_RT2_OHM);
        float tK = 1.0f / (1.0f / 298.15f + logf(rNtc / TS_NTC_R25_OHM) / TS_NTC_BETA);
        out = (int16_t)lroundf((tK - 273.15f) * 10.0f);
        return true;
    }
};
