#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <functional>

#include "../battery/BatteryMonitor.h"
#include "../buttons/ButtonRoles.h"
#include "../config/BoardProfile.h"
#include "../config/Config.h"
#include "../config/LightLevel.h"
#include "../game/GameManager.h"
#include "../game/RxRing.h"
#include "../led/LedDriver.h"
#include "../logging/Logger.h"
#include "../mesh/MeshManager.h"
#include "../mesh/SoftAp.h"
#include "../mesh/WifiElection.h"
#include "../power/PowerManager.h"
#include "../update/Updater.h"
#include "DeepSleep.h"
#include "FactoryTest.h"
#include "FleetConfig.h"
#include "FleetFrames.h"
#include "FleetLink.h"
#include "FleetRadio.h"
#include "FleetRender.h"
#include "FleetTypes.h"
#include "Imu.h"
#include "PowerState.h"

static_assert(fleet::kWifiNetworksMax == MAX_WIFI_NETWORKS,
              "config wifi takes as many networks as Config stores");
static_assert(fleet::kTokenMax + 1 == sizeof(DeviceConfig::githubToken),
              "config token takes as long a token as Config stores");
static_assert(fleet::kEventVregMv <= PowerManager::CHARGE_VOLTAGE_MV,
              "the event target stays at or below the board's charge voltage");
static_assert(MAX_BUTTONS <= 16, "the muted-key mask holds one bit per button");

// Arduino side of the fleet package (src/fleet/): device states with their
// effects on radio, keys and LEDs, the charge target in NVS, FleetStatus and
// FleetCommand on the mesh, "update all", the maintenance AP, deep sleep and
// the USB namespaces `config`, `fleet` and `test` through the game link.
// main.cpp wires it like every other module. Only a detected Buzzerboard
// sends FleetStatus and acts on fleet commands; any device can be the bridge
// that sends them. Without a charger the device stays active.
class FleetManager {
   public:
    struct Io {
        std::function<void(bool enabled)> keysEnabled;  // button actions on or off
        std::function<void(uint16_t mask)> mutedKeys;   // ButtonManager::setMuted
        // An update or OTA runs outside the fleet (Updater checking,
        // downloading or done, ArduinoOTA): no standby, no sleep.
        std::function<bool()> updateBusy;
    };

    // Before power.begin(): the stored charge target, so the charger starts
    // with it, and the device's own colours.
    void loadStored() {
        Preferences p;
        if (p.begin(NVS_NS, /*readOnly=*/true)) {
            _target = p.getUChar(KEY_TARGET, 0) == (uint8_t)fleet::Target::Event
                          ? fleet::Target::Event
                          : fleet::Target::Store;
            fleet::Look l;
            if (p.getBytesLength(KEY_LOOK) == sizeof(l) && p.getBytes(KEY_LOOK, &l, sizeof(l)) &&
                l.count <= fleet::kLookStopsMax)
                _look = l;
            p.end();
        }
    }

    uint16_t vregMv() const {
        return _target == fleet::Target::Event ? fleet::kEventVregMv : fleet::kStoreVregMv;
    }

    // After power.begin(): USB power without a host (no SOF on the
    // USB-Serial-JTAG within kHostDetectMs) boots straight into charging mode,
    // without a WiFi connection.
    static bool bootInCharging(PowerManager& power) {
        if (!power.online() || !power.status().powerGood()) return false;
        uint32_t t0 = millis();
        do {
            if (_usbHost()) return false;
            delay(10);
        } while (millis() - t0 < fleet::kHostDetectMs);
        return true;
    }

    // The light the fleet renders on: the first one with the Buzzerboard's 34 LEDs.
    void setLight(LedDriver* led, uint16_t ledCount) {
        if (!_led && ledCount == game::kLedCount) _led = led;
    }

    // After mesh.begin(), before gameSession.begin().
    void begin(const Io& io, PowerManager* power, BatteryMonitor* battery, GameManager* game,
               MeshManager* mesh, WifiElection* wifi, bool bootCharging, uint32_t now) {
        _io = io;
        _power = power;
        _battery = battery;
        _game = game;
        _mesh = mesh;
        _wifi = wifi;
        _radioInactive = bootCharging;
        _fleetDevice = boardDetected();
        if (Config::wifiCount() == 0) Config::loadWifi();

        WiFi.macAddress(_mac);
        _filter.begin(_mac);
        if (!_rx.allocate(fleet::kRxQueue))
            Logger::e("[fleet] out of memory for the receive queue");
        // A key wake from sleep: the short timeout, and the time in sleep so
        // far goes on toward ship mode unless a real interaction follows.
        _bootMs = now;
        _sleptCarryS = fleet::DeepSleep::sleptS();
        _ps.begin(now, fleet::DeepSleep::keyWake());
        if (fleet::DeepSleep::keyWake())
            Logger::i("[fleet] woken by a key: standby after %lu s without interaction",
                      (unsigned long)(fleet::kWakeInactiveMs / 1000));
        auto rnd = [](uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); };
        _beacon.begin(rnd, now);
        _sender.begin([this](const uint8_t* d, size_t n) { _mesh->broadcastFleetFrame(d, n); },
                      rnd);

        fleet::FactoryTest::Io tio;
        tio.chargeMode = [this](bool on) {
            if (on) {
                _power->setChargeVoltageMv(fleet::kEventVregMv);
                _power->setAdcIntervalMs(fleet::kTestAdcIntervalMs);
            } else {
                _power->setChargeVoltageMv(vregMv());
                _power->resetAdcInterval();
            }
        };
        _test.begin(tio);
        _link.begin(game->link(), _linkIo(), &_table, &_sender, &_test);
        game->setLinkExtension(&_link);
        Logger::i("[fleet] charge target %s (VREG %u mV)", fleet::kTargetNames[(uint8_t)_target],
                  vregMv());
    }

    // ESP-NOW receive callback (WiFi task): copy only, processed in tick().
    void onFrame(const uint8_t* mac, const uint8_t* data, int len, int8_t rssi) {
        if (len > 0) _rx.push(data, (size_t)len, rssi, mac);
    }

    // Raw key edge of button slot `index`. True: the fleet takes it
    // (charging mode, standby wake, the key that woke the device, key test),
    // so no other module sees it, and that key's actions stay muted until it
    // has been up for kKeyQuietMs.
    bool onKey(uint8_t index, const char* role, bool pressed, uint32_t ms) {
        if (index >= MAX_BUTTONS) return false;
        uint16_t bit = (uint16_t)(1u << index);
        game::Key k = game::keyFromName(role);
        if (k == game::Key::L1) _l1Down = pressed;
        if (k == game::Key::R1) _r1Down = pressed;
        // R2, active and outside a session: the battery level for
        // kBatteryOverlayMs (Timo, 2026-11-09).
        if (k == game::Key::R2 && pressed && _led && _managed() &&
            _ps.state() == fleet::State::Active && !_game->radioQuiet() && !_test.running() &&
            !_upd.running && _batteryPercent() != fleet::kNoBattery)
            _batteryOverlayUntil = millis() + fleet::kBatteryOverlayMs;
        if (pressed) {
            _keysDown |= bit;
        } else {
            _keysDown &= (uint16_t)~bit;
            _keyUpMs[index] = ms;
        }
        bool take = false;
        if (_test.kind() == fleet::FactoryTest::Kind::Keys) {
            if (pressed && k != game::Key::None) _link.onTestKey(k);
            take = true;
        } else if (_managed() && _ps.state() != fleet::State::Active) {
            // Charging mode takes every key; in standby a press wakes the device.
            if (pressed && _ps.state() == fleet::State::Standby) _ps.interaction(millis());
            take = true;
        } else if (_managed() && pressed) {
            // A press already held at the first loop pass woke the device:
            // it keeps the device awake but is no interaction.
            bool wakePress = !_ticked || ms - _firstTickMs < fleet::kBootPressGraceMs;
            if (wakePress)
                _ps.activity(millis());
            else
                _ps.interaction(millis());
            take = wakePress;
        }
        if (take && pressed) _quietMask |= bit;
        return take || (_quietMask & bit);
    }

    // Every loop() pass, also during an update download.
    void tick(uint32_t now) {
        if (!_ticked) {
            _ticked = true;
            _firstTickMs = now;
        }
        game::RxRing::Frame f;
        while (_rx.pop(f)) _onFrameLoop(f, now);

        bool usb = _usb();
        bool session = _game->radioQuiet();
        if (!session && _game->inviteVisible(now)) _ps.interaction(now);
        uint32_t adcCount = _managed() ? _power->adcCount() : 0;
        bool sample = adcCount != _lastAdcCount;
        _lastAdcCount = adcCount;
        uint16_t vbat = _managed() ? _vbatMv() : 0;

        fleet::PowerState::Inputs in;
        in.managed = _managed();
        in.usb = usb;
        in.hostLink = _game->linkActive() || _usbHost();
        in.session = session;
        in.hold = _upd.running || _test.running() || (_io.updateBusy && _io.updateBusy());
        in.battery = _batteryPercent();
        in.vbatMv = vbat;
        fleet::State before = _ps.state();
        fleet::PowerState::Action action = _ps.tick(in, now);
        if (!_applied || _ps.state() != before) _apply(_ps.state());
        if (action == fleet::PowerState::Action::Sleep && !in.hold) _sleep(now, false);

        // The USB supply: too weak in charging mode, or VBAT low on USB.
        const Bq25628::Adc& adc = _power->adc();
        _usbSupply.tick(usb, _ps.state() == fleet::State::Charging, sample, adc.ibatValid,
                        adc.ibatMa, vbat, now);
        bool weak = _weakNow();
        if (weak != _weakLogged) {
            _weakLogged = weak;
            if (weak) Logger::w("[fleet] USB supply too weak: LEDs off, radio low duty");
        }
        _setStandbyRadio(_ps.state() == fleet::State::Standby || weak);
        if (_usbSupply.sleep(now) && !in.hold) _sleep(now, true);

        fleet::ChargerFacts facts = _facts(vbat);
        _charge = fleet::chargeStatus(facts);
        _errors = fleet::chargerErrors(facts);
        if (_charge != fleet::Charge::Full) {
            _full = false;
        } else if (!_full) {
            _full = true;
            _fullSince = now;
        }

        _sender.tick(now);
        _tickUpdate();
        if (sample) _testSample();
        _link.tick(now);
        _tickCombo(now);
        _tickIdleLevel(now);
        SoftAp::tick(now);
        _tickKeys(now);

        // `fleet update` on the bridge: it updates itself once the command's
        // repeats are out.
        if (_selfUpdate && _sender.idle()) {
            _selfUpdate = false;
            _startUpdate(_selfSource, _selfPr);
        }

        if (!session && _fleetDevice) {
            fleet::StatusBeacon::Snapshot s = _snapshot(usb);
            if (_beacon.tick(s, now)) _sendStatus(s, usb, now);
        }
    }

    // Charging or standby: the main loop leaves WiFi, web, OTA, MQTT, time
    // and scene/playlist sync alone, as during a game session.
    bool radioInactive() const { return _radioInactive; }
    // A fleet update runs: WifiElection keeps ticking for its connection,
    // also in charging mode.
    bool updateRunning() const { return _upd.running; }
    bool hasLight() const { return _led != nullptr; }

    // The fleet draws the light: factory test, update, charging, standby,
    // switched off on USB, the battery overlay, the device's own colours.
    bool ownsLeds() const {
        return _led && (_test.running() || _upd.running || _ps.state() != fleet::State::Active ||
                        _levelShowing || _offLevel() || _batteryOverlay() || _lookShowing());
    }

    // Full frame while ownsLeds().
    void render(uint32_t now) {
        if (!_led) return;
        _ledsOwned = true;
        game::Rgb px[game::kLedCount];
        if (_test.running())
            _test.frame(
                now, px,
                _displayLevel() < fleet::kTestLevelMin ? fleet::kTestLevelMin : _displayLevel());
        else if (_upd.running)
            fleet::renderUpdate(_upd.pct, now, px, _displayLevel());
        else if (_batteryOverlay())
            fleet::renderLevel(_batteryPercent(), _ibatMa(), now, px, _displayLevel());
        else if (_offLevel())
            fleet::renderOffLevel(_batteryPercent(), _charge == fleet::Charge::Charging, now, px);
        else if (_ps.state() == fleet::State::Charging && !_weakNow() && !_ledsSwitchedOff())
            fleet::renderCharging(_chargeView(now), now, px, _displayLevel());
        else if (_levelShowing)
            fleet::renderLevel(_batteryPercent(), _ibatMa(), now, px, _displayLevel());
        else if (_lookShowing())
            fleet::renderLook(_look, now - (_levelNext - fleet::kIdlePulseMs), _displayLevel(), px);
        else
            fleet::clearFrame(px);
        _show(px);
    }

    enum class Overlay : uint8_t { None, Drawn, Ended };

    // While the maintenance AP is open: the status dot pulses blue slowly
    // over the normal pattern. Ended: the caller repaints the pattern.
    Overlay renderOverlay(uint32_t now) {
        if (!_led) return Overlay::None;
        if (SoftAp::maintenanceOpen() && _ps.state() == fleet::State::Active) {
            game::Rgb c = fleet::level(game::colorRgb(game::Color::Blue),
                                       fleet::slowPulse(now, fleet::kApPulseMs, _displayLevel()));
            _led->setPixel(game::kStatusDot, c.r, c.g, c.b);
            _led->show();
            _overlayOn = true;
            return Overlay::Drawn;
        }
        if (!_overlayOn) return Overlay::None;
        _overlayOn = false;
        return Overlay::Ended;
    }

    // The update display while an update writes the flash (any trigger).
    void renderUpdate(uint32_t now, uint8_t percent) {
        if (!_led) return;
        _ledsOwned = true;
        game::Rgb px[game::kLedCount];
        fleet::renderUpdate(percent, now, px, _displayLevel());
        _show(px);
    }

    // True once after the fleet stopped drawing: the caller repaints the pattern.
    bool takeLedsReleased() {
        if (!_ledsOwned) return false;
        _ledsOwned = false;
        return true;
    }

   private:
    static constexpr const char* NVS_NS = "fleet";
    static constexpr const char* KEY_TARGET = "target";
    static constexpr const char* KEY_LOOK = "look";

    Io _io;
    PowerManager* _power = nullptr;
    BatteryMonitor* _battery = nullptr;
    GameManager* _game = nullptr;
    MeshManager* _mesh = nullptr;
    WifiElection* _wifi = nullptr;
    LedDriver* _led = nullptr;

    fleet::PowerState _ps;
    fleet::StatusBeacon _beacon;
    fleet::CommandFilter _filter;
    fleet::CommandSender _sender;
    fleet::FleetTable _table;
    fleet::FactoryTest _test;
    fleet::FleetLink _link;
    game::RxRing _rx;

    fleet::Target _target = fleet::Target::Store;
    fleet::Charge _charge = fleet::Charge::None;
    uint16_t _errors = 0;
    bool _full = false;
    uint32_t _fullSince = 0;
    bool _levelShowing = false;
    bool _idle = false;                 // active and idle (see _tickIdleLevel)
    fleet::Look _look;                  // the device's own colours (`config look`), count 0: none
    uint32_t _batteryOverlayUntil = 0;  // millis(); 0 = no overlay
    uint32_t _levelUntil = 0;
    uint32_t _levelNext = 0;
    bool _applied = false;
    bool _radioInactive = false;
    bool _standbyRadio = false;
    wifi_ps_type_t _psBefore = WIFI_PS_NONE;
    bool _ledsOwned = false;
    bool _overlayOn = false;
    bool _fleetDevice = false;  // a detected Buzzerboard: FleetStatus and fleet commands
    uint32_t _bootMs = 0;
    bool _ticked = false;
    uint32_t _firstTickMs = 0;
    uint32_t _sleptCarryS = 0;  // time in sleep before a key wake
    bool _sleepTried = false;
    uint32_t _lastSleepTry = 0;
    fleet::UsbSupply _usbSupply;
    bool _weakLogged = false;

    uint16_t _keysDown = 0;               // bit per button slot
    uint32_t _keyUpMs[MAX_BUTTONS] = {};  // last release per slot
    uint16_t _quietMask = 0;              // taken keys: muted until up for kKeyQuietMs
    int8_t _keysOn = -1;                  // last value given to Io::keysEnabled
    int32_t _mutedSent = -1;              // last mask given to Io::mutedKeys
    bool _l1Down = false;
    bool _r1Down = false;
    bool _comboHeld = false;
    uint32_t _comboSince = 0;
    bool _comboFired = false;

    uint32_t _lastAdcCount = 0;
    uint8_t _mac[6] = {};

    bool _selfUpdate = false;
    fleet::Source _selfSource = fleet::Source::Release;
    uint16_t _selfPr = 0;

    struct UpdateRun {
        bool running = false;
        bool launched = false;  // Updater::installSourceAsync() was called
        fleet::Update state = fleet::Update::Idle;
        uint8_t pct = 0;
        fleet::UpdateError error = fleet::UpdateError::None;
    };
    UpdateRun _upd;

    static bool _usbHost() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
        // SOF frames from a host within the last few ms (IDF connection monitor).
        // UNVERIFIED on the ESP32-C5: that a power-only USB source reads false
        // and a laptop reads true within kHostDetectMs of boot.
        return HWCDC::isPlugged();
#else
        return false;  // UART bridge: only link traffic counts
#endif
    }

    bool _managed() const { return _power && _power->online(); }
    bool _usb() const { return _managed() && _power->status().powerGood(); }

    uint8_t _batteryPercent() const {
        const auto& bs = _battery->status();
        return bs.present ? bs.percent : fleet::kNoBattery;
    }

    uint16_t _vbatMv() const {
        BatteryMonitor::ChargerReading r = _power->batteryReading();
        return r.valid ? r.vbatMv : 0;
    }

    // Weak USB supply with its effects: in charging mode only.
    bool _weakNow() const { return _usbSupply.weak() && _ps.state() == fleet::State::Charging; }

    fleet::ChargerFacts _facts(uint16_t vbat) const {
        fleet::ChargerFacts f;
        f.enabled = Config::get().chargerEnabled;
        f.online = _managed();
        if (!f.online) return f;
        const Bq25628::Status& st = _power->status();
        f.powerGood = st.powerGood();
        f.charging = st.chargeState() != Bq25628::ChargeState::NotCharging;
        // TS and the safety timer matter while charging. On battery TS_STAT
        // can read cold because the TS resistor string has no supply
        // (SLUSEG4C Table 8-28, TS_STAT 001b).
        if (f.powerGood) {
            f.tsZone = st.tsStat();
            f.safetyTimer = st.stat0 & Bq25628::STAT0_SAFETY_TMR;
        }
        f.thermalShutdown = st.fault & Bq25628::FAULT_TSHUT;
        f.batteryFault = st.fault & Bq25628::FAULT_BAT;
        f.supplyFault = st.fault & (Bq25628::FAULT_VBUS | Bq25628::FAULT_SYS);
        f.lowBattery = vbat != 0 && vbat < PowerManager::UPDATE_MIN_MV;
        f.weakSupply = _weakNow();
        f.vbatMv = vbat;
        f.vregMv = _power->chargeVoltageMv();
        return f;
    }

    fleet::ChargeView _chargeView(uint32_t now) const {
        fleet::ChargeView v;
        v.charge = _charge;
        v.battery = _batteryPercent();
        v.fullForMs = _full ? now - _fullSince : 0;
        v.ibatMa = _ibatMa();
        return v;
    }

    // The LEDs switched off with a key (light 0's brightness override at 0,
    // see config/BoardProfile.cpp): on battery everything stays dark, on USB
    // only _offLevel() shows (Timo, 2026-11-09).
    bool _ledsSwitchedOff() const {
        const LightHardwareConfig& l = Config::get().lights[0];
        return l.brightnessOverrideEnabled && l.brightnessOverride == 0;
    }

    // The brightness of every fleet display: what light 0 renders at, at
    // least kOffLevel (Timo, 2026-11-09).
    uint8_t _displayLevel() const {
        uint8_t b = currentLightLevel(0);
        return b < fleet::kOffLevel ? fleet::kOffLevel : b;
    }

    bool _batteryOverlay() const {
        return _batteryOverlayUntil != 0 && (int32_t)(millis() - _batteryOverlayUntil) < 0;
    }

    bool _lookShowing() const { return _idle && !_levelShowing && _look.count >= 2; }

    // Stores the device's own colours (count 0 removes them) and shows them
    // from the next idle breath on.
    bool _setLook(const fleet::Look& l) {
        Preferences p;
        bool stored = p.begin(NVS_NS, false);
        if (stored) {
            if (l.count)
                stored = p.putBytes(KEY_LOOK, &l, sizeof(l)) == sizeof(l);
            else if (p.isKey(KEY_LOOK))
                stored = p.remove(KEY_LOOK);
            p.end();
        }
        if (!stored) {
            Logger::e("[fleet] own colours could not be stored");
            return false;
        }
        _look = l;
        Logger::i("[fleet] own colours: %u stops", l.count);
        return true;
    }

    // Switched off with a key, on USB with a cell: renderOffLevel()
    // (Timo, 2026-11-09).
    bool _offLevel() const {
        return _ledsSwitchedOff() && _usb() && !_weakNow() &&
               _batteryPercent() != fleet::kNoBattery;
    }

    int16_t _ibatMa() const {
        if (!_managed()) return 0;
        const Bq25628::Adc& a = _power->adc();
        return a.ibatValid ? a.ibatMa : 0;
    }

    // Active and idle (no session, invite, test, update or maintenance AP):
    // kIdlePulseMs of the idle pattern, or of the device's own colours when
    // it has them, then kIdleLevelMs of the battery level, in turn (see
    // kIdlePulseMs). Without a battery only the first.
    void _tickIdleLevel(uint32_t now) {
        bool idle = _led && _ps.state() == fleet::State::Active && !_test.running() &&
                    !_upd.running && !_game->radioQuiet() && !_game->inviteVisible(now) &&
                    !SoftAp::maintenanceOpen() && !_ledsSwitchedOff();
        if (!idle || !_idle) {
            _idle = idle;
            _levelShowing = false;
            _levelNext = now + fleet::kIdlePulseMs;
            return;
        }
        if (_levelShowing) {
            if ((int32_t)(now - _levelUntil) < 0) return;
            _levelShowing = false;
            _levelNext = now + fleet::kIdlePulseMs;
        } else if (_batteryPercent() != fleet::kNoBattery && (int32_t)(now - _levelNext) >= 0) {
            _levelShowing = true;
            _levelUntil = now + fleet::kIdleLevelMs;
        }
    }

    void _show(const game::Rgb* px) {
        for (uint16_t i = 0; i < game::kLedCount; i++) _led->setPixel(i, px[i].r, px[i].g, px[i].b);
        _led->show();
    }

    // ── States ───────────────────────────────────────────────────────────────
    void _apply(fleet::State s) {
        _applied = true;
        bool inactive = s != fleet::State::Active;
        if (inactive != _radioInactive) {
            _radioInactive = inactive;
            if (inactive) {
                _wifi->suspend();
                SoftAp::off();
                _mesh->setFleetOnly(true);
            } else {
                _mesh->setFleetOnly(false);
                _wifi->resume();
            }
        }
        if (inactive) SoftAp::off();
        _power->setLedRailWanted(s != fleet::State::Standby);
        Logger::i("[fleet] state %s", fleet::kStateNames[(uint8_t)s]);
    }

    // Standby: ESP-NOW listens kStandbyWakeWindowMs of every
    // kStandbyWakeIntervalMs (modem sleep while disconnected,
    // CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE).
    // UNVERIFIED: the current saved and that ESP-NOW frames arrive in the
    // window on the ESP32-C5 with this IDF.
    void _setStandbyRadio(bool on) {
        if (on == _standbyRadio) return;
        _standbyRadio = on;
        if (on) {
            esp_wifi_get_ps(&_psBefore);
            esp_wifi_connectionless_module_set_wake_interval(fleet::kStandbyWakeIntervalMs);
            esp_now_set_wake_window(fleet::kStandbyWakeWindowMs);
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        } else {
            esp_now_set_wake_window(65535);  // the maximum, the default (esp_now.h)
            esp_wifi_connectionless_module_set_wake_interval(
                ESP_WIFI_CONNECTIONLESS_INTERVAL_DEFAULT_MODE);
            esp_wifi_set_ps(_psBefore);
        }
    }

    // Deep sleep (src/fleet/DeepSleep.h): standby down to the storage level
    // (on battery), or VBAT low on USB (usbLow). Skipped this pass if the
    // LED rail does not read off.
    void _sleep(uint32_t now, bool usbLow) {
        if (!_managed() || (!usbLow && _usb())) return;
        if (_sleepTried && now - _lastSleepTry < fleet::kSleepRetryMs) return;
        _sleepTried = true;
        _lastSleepTry = now;
        if (!_power->prepareDeepSleep()) return;
        if (usbLow)
            Logger::w(
                "[fleet] VBAT %u mV on USB - deep sleep while charging, timer wake every %lu s",
                _vbatMv(), (unsigned long)fleet::kSleepWakeIntervalS);
        else
            Logger::i("[fleet] battery at %u %%, standby ends - deep sleep, timer wake every %lu s",
                      _batteryPercent(), (unsigned long)fleet::kSleepWakeIntervalS);
        SoftAp::off();
        if (_led) {
            game::Rgb px[game::kLedCount];
            fleet::clearFrame(px);
            _show(px);
        }
        Serial.flush();
        // Still on the short timeout of a key wake: the earlier sleep and
        // this wake count toward ship mode.
        uint32_t carry = _ps.shortWake() ? _sleptCarryS + (now - _bootMs) / 1000 : 0;
        auto& c = Config::get();
        fleet::DeepSleep::enter(c.i2cSdaPin, c.i2cSclPin, _wakePins(), carry);
    }

    // The keys that wake the device from sleep (owner decision): BUZZ, UP,
    // DOWN, LEFT, ENTER and POWER, where their GPIO can wake the chip. On
    // the Buzzerboard that is GPIO 0-5; RIGHT (GPIO12) cannot.
    static uint64_t _wakePins() {
        static const char* const kRoles[] = {ButtonRole::BUZZ,  ButtonRole::UP,
                                             ButtonRole::DOWN,  ButtonRole::LEFT,
                                             ButtonRole::ENTER, ButtonRole::POWER};
        uint64_t mask = 0;
        for (const char* role : kRoles) {
            int8_t i = Config::buttonIndexForRole(role);
            if (i < 0) continue;
            const ButtonHardwareConfig& b = Config::get().buttons[i];
            if (b.viaExpander || !b.activeLow) continue;
            if (esp_sleep_is_valid_wakeup_gpio((gpio_num_t)b.pin)) mask |= 1ULL << b.pin;
        }
        return mask;
    }

    // ── Keys ─────────────────────────────────────────────────────────────────
    // Button actions: on in active state outside a key test; a taken key
    // stays muted until it has been up for kKeyQuietMs, so a stuck key
    // mutes only itself.
    void _tickKeys(uint32_t now) {
        bool on = _ps.state() == fleet::State::Active && !_test.running();
        if (on) {
            for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
                uint16_t bit = (uint16_t)(1u << i);
                if ((_quietMask & bit) && !(_keysDown & bit) &&
                    now - _keyUpMs[i] >= fleet::kKeyQuietMs)
                    _quietMask &= (uint16_t)~bit;
            }
        }
        if ((int8_t)on != _keysOn) {
            _keysOn = on;
            if (_io.keysEnabled) _io.keysEnabled(on);
        }
        if ((int32_t)_quietMask != _mutedSent) {
            _mutedSent = _quietMask;
            if (_io.mutedKeys) _io.mutedKeys(_quietMask);
        }
    }

    // L1 + R1 held kApComboMs: maintenance AP (active, outside a session).
    void _tickCombo(uint32_t now) {
        if (!(_l1Down && _r1Down)) {
            _comboHeld = false;
            _comboFired = false;
            return;
        }
        if (!_comboHeld) {
            _comboHeld = true;
            _comboSince = now;
        }
        if (_comboFired || now - _comboSince < fleet::kApComboMs) return;
        _comboFired = true;
        if (_ps.state() != fleet::State::Active || _game->radioQuiet()) return;
        if (!SoftAp::openMaintenance(fleet::kApMinutesDefault, now))
            Logger::i("[fleet] L1+R1: this board runs its fallback AP instead");
    }

    // ── Charge target ────────────────────────────────────────────────────────
    // Stores the target first and applies it then; false (nothing changed)
    // if NVS does not take it. Lowering it below VBAT on USB trips the
    // charger's battery over-voltage protection for a while, which
    // chargerErrors() does not count (batteryOvpExpected).
    bool _setTarget(fleet::Target t) {
        if (t != _target) {
            Preferences p;
            bool stored = p.begin(NVS_NS, false) && p.putUChar(KEY_TARGET, (uint8_t)t) == 1;
            p.end();
            if (!stored) {
                Logger::e("[fleet] charge target %s could not be stored",
                          fleet::kTargetNames[(uint8_t)t]);
                return false;
            }
            _target = t;
        }
        // A running charge test restores the target when it ends.
        if (_test.kind() != fleet::FactoryTest::Kind::Charge) _power->setChargeVoltageMv(vregMv());
        Logger::i("[fleet] charge target %s (VREG %u mV)", fleet::kTargetNames[(uint8_t)t],
                  vregMv());
        return true;
    }

    // ── Radio ────────────────────────────────────────────────────────────────
    void _onFrameLoop(const game::RxRing::Frame& f, uint32_t now) {
        if (f.data[0] == fleet::kMsgFleetStatus) {
            fleet::FleetStatusMsg m;
            if (_table.allocated() &&
                fleet::decodeFleetFrame(f.data, f.len, fleet::kMsgFleetStatus, m))
                _table.onStatus(f.mac, m, f.rssi, now);
            return;
        }
        fleet::FleetCommandMsg m;
        if (!fleet::decodeFleetFrame(f.data, f.len, fleet::kMsgFleetCommand, m)) return;
        // Only Buzzerboards act on fleet commands, and not in a game session.
        if (!_fleetDevice || _game->radioQuiet() || !_filter.accept(m, now)) return;
        switch ((fleet::Op)m.op) {
            case fleet::Op::Update:
                _startUpdate((fleet::Source)m.arg, m.pr);
                return;
            case fleet::Op::Charge:
                if (m.arg <= (uint8_t)fleet::Target::Event) _setTarget((fleet::Target)m.arg);
                return;
            case fleet::Op::Off:
                if (!_managed() || _usb()) {
                    Logger::i("[fleet] off: ignored (USB power or no charger)");
                    return;
                }
                _power->requestOff("fleet off");
                return;
            case fleet::Op::Ap: {
                if (_ps.state() == fleet::State::Charging) {
                    Logger::i("[fleet] ap: ignored in charging mode");
                    return;
                }
                _ps.activity(now);
                uint8_t minutes =
                    m.arg == 0 || m.arg > fleet::kApMinutesMax ? fleet::kApMinutesDefault : m.arg;
                SoftAp::openMaintenance(minutes, now);
                return;
            }
        }
    }

    fleet::StatusBeacon::Snapshot _snapshot(bool usb) const {
        fleet::StatusBeacon::Snapshot s;
        s.state = _ps.state();
        s.charge = _charge;
        s.target = _target;
        s.usb = usb;
        s.update = _upd.state;
        s.updatePct = _upd.pct;
        s.errors = _errors;
        s.number = _game->number();
        return s;
    }

    // Broadcast; a bridge also lists itself in its own table (ESP-NOW does
    // not deliver a broadcast to its sender), with RSSI 0.
    void _sendStatus(const fleet::StatusBeacon::Snapshot& s, bool usb, uint32_t now) {
        fleet::FleetStatusMsg m;
        memset(&m, 0, sizeof(m));
        m.type = fleet::kMsgFleetStatus;
        m.proto = fleet::kProto;
        m.number = s.number;
        m.state = (uint8_t)s.state;
        m.battery = _batteryPercent();
        m.vbatMv = _managed() ? _vbatMv() : 0;
        m.charge = (uint8_t)s.charge;
        m.target = (uint8_t)s.target;
        m.flags = (usb ? fleet::kFlagUsb : 0) | (Config::wifiCount() > 0 ? fleet::kFlagWifi : 0);
        m.fw = _game->fwBuild();
        m.update = (uint8_t)s.update;
        m.updatePct = s.updatePct;
        m.updateError = (uint8_t)_upd.error;
        m.errors = s.errors;
        _mesh->broadcastFleetFrame((const uint8_t*)&m, sizeof(m));
        if (_table.allocated()) _table.onStatus(_mac, m, 0, now);
    }

    // ── Update all ───────────────────────────────────────────────────────────
    void _startUpdate(fleet::Source src, uint16_t pr) {
        if (_upd.running) return;
        if (Config::wifiCount() == 0) return _updateFailed(fleet::UpdateError::NoWifi);
        if (_power->updateBlockReason()) return _updateFailed(fleet::UpdateError::LowBattery);
        String tag = src == fleet::Source::Pr ? String("pr-") + pr : String();
        _upd = UpdateRun{};
        _upd.running = true;
        _upd.state = fleet::Update::Checking;
        Logger::i("[fleet] update from %s", tag.isEmpty() ? "the latest release" : tag.c_str());
        _wifi->requestTemporary([this, tag]() {
            _upd.launched = true;
            Updater::installSourceAsync(tag);
        });
    }

    void _updateFailed(fleet::UpdateError e) {
        _upd = UpdateRun{};
        _upd.state = fleet::Update::Failed;
        _upd.error = e;
        Logger::w("[fleet] update failed: %s", fleet::kUpdateErrorNames[(uint8_t)e]);
    }

    void _tickUpdate() {
        if (!_upd.running || !_upd.launched) return;
        const Updater::Status& st = Updater::status();
        switch (st.state) {
            case Updater::State::Checking:
                _upd.state = fleet::Update::Checking;
                return;
            case Updater::State::Downloading:
                _upd.state = fleet::Update::Downloading;
                _upd.pct = (uint8_t)constrain(st.progress, 0, 100);
                return;
            case Updater::State::Done:
                _upd.state = fleet::Update::Installing;
                _upd.pct = 100;
                return;
            case Updater::State::Idle:
                Logger::i("[fleet] update: this build is installed already");
                _upd = UpdateRun{};
                break;
            case Updater::State::Error: {
                fleet::UpdateError e = fleet::UpdateError::Failed;
                if (st.error && strcmp(st.error, "not connected to WiFi") == 0)
                    e = fleet::UpdateError::WifiFailed;
                else if (_power->updateBlockReason())
                    e = fleet::UpdateError::LowBattery;
                _updateFailed(e);
                break;
            }
        }
        // Hand the radio back: WifiElection releases its temporary hold in
        // loop(); charging mode and standby have no station connection.
        if (_radioInactive) WiFi.disconnect(false);
    }

    // ── Factory test ─────────────────────────────────────────────────────────
    void _testSample() {
        const Bq25628::Adc& a = _power->adc();
        _test.onAdcSample(a.ibusMa, a.ibatMa, a.ibatValid, a.vbusMv);
    }

    void _testInfo(fleet::FleetLink::TestInfo& t) {
        WiFi.macAddress(t.mac);
        t.fw = _game->fwBuild();
        t.number = _game->number();
        t.chargerFound = _managed();
        if (t.chargerFound) {
            t.chargerRev = _power->revision();
            t.fault = _power->status().fault;
        }
        t.charge = _charge;
        t.adcValid = _managed() && _power->adcAgeMs() < 10000;
        if (t.adcValid) {
            const Bq25628::Adc& a = _power->adc();
            t.vbatMv = a.vbatMv;
            t.vbusMv = a.vbusMv;
            t.ibusMa = a.ibusMa;
            t.ibatValid = a.ibatValid;
            t.ibatMa = a.ibatMa;
            t.tsValid = _power->tsAdcOn();
            t.tsPermille = a.tsPermille;
            t.tsTempValid = _power->tsDeciC(t.tsDeciC);
            t.tdieDeciC = a.tdieDeciC;
        }
        auto& c = Config::get();
        if (c.i2cSdaPin != PIN_UNUSED && c.i2cSclPin != PIN_UNUSED) {
            fleet::ImuReading r = fleet::Imu::read();
            t.imuFound = r.found;
            t.imuId = r.id;
            t.accelValid = r.accelValid;
            for (uint8_t i = 0; i < 3; i++) t.accelG[i] = r.g[i];
        }
    }

    fleet::FleetLink::Io _linkIo() {
        using fleet::FleetLink;
        FleetLink::Io io;
        io.setWifi = [](const FleetLink::WifiNet* nets, uint8_t count) {
            WifiNetwork list[MAX_WIFI_NETWORKS];
            for (uint8_t i = 0; i < count; i++) {
                strlcpy(list[i].ssid, nets[i].ssid, sizeof(list[i].ssid));
                strlcpy(list[i].password, nets[i].pass, sizeof(list[i].password));
            }
            bool ok = Config::replaceWifiNetworks(list, count);
            Logger::i("[fleet] WiFi list replaced over USB: %u network(s)", count);
            return ok;
        };
        io.wifiCount = []() { return Config::wifiCount(); };
        io.setToken = [](const char* token) {
            strlcpy(Config::get().githubToken, token, sizeof(Config::get().githubToken));
            bool ok = Config::save();
            Logger::i("[fleet] GitHub token %s over USB", token[0] ? "set" : "cleared");
            return ok;
        };
        io.hasToken = []() { return Config::get().githubToken[0] != '\0'; };
        io.setTarget = [this](fleet::Target t) { return _setTarget(t); };
        io.target = [this]() { return _target; };
        io.setLook = [this](const fleet::Look& l) { return _setLook(l); };
        io.lookStops = [this]() { return _look.count; };
        io.openAp = [](uint8_t minutes) {
            return SoftAp::openMaintenance(minutes, millis()) ? FleetLink::ApResult::Ok
                                                              : FleetLink::ApResult::Unsupported;
        };
        io.number = [this]() { return _game->number(); };
        io.fwBuild = [this]() { return _game->fwBuild(); };
        io.radioBusy = [this]() { return _game->radioQuiet(); };
        io.nonce = []() { return (uint32_t)esp_random(); };
        // A Buzzerboard bridge applies `charge` at once and `update` once the
        // repeats are out; `off` and `ap` are for the other devices.
        io.sent = [this](const fleet::FleetCommandMsg& m) {
            if (!_fleetDevice) return;
            if ((fleet::Op)m.op == fleet::Op::Charge && m.arg <= (uint8_t)fleet::Target::Event)
                _setTarget((fleet::Target)m.arg);
            if ((fleet::Op)m.op != fleet::Op::Update) return;
            _selfUpdate = true;
            _selfSource = (fleet::Source)m.arg;
            _selfPr = m.pr;
        };
        io.testInfo = [this](FleetLink::TestInfo& t) { _testInfo(t); };
        io.usbPower = [this]() { return _usb(); };
        io.participantBusy = [this]() { return _game->ownsLeds(); };
        io.reboot = []() {
            Logger::i("[fleet] test reboot");
            ESP.restart();
        };
        return io;
    }
};
