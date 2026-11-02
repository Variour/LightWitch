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
#include "../config/Config.h"
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

// Arduino side of the fleet package (src/fleet/): device states with their
// effects on radio, keys and LEDs, the charge target in NVS, FleetStatus and
// FleetCommand on the mesh, "update all", the maintenance AP, deep sleep and
// the USB namespaces `config`, `fleet` and `test` through the game link.
// main.cpp wires it like every other module. Without a charger the device
// stays active; it still sends FleetStatus and acts on fleet commands.
class FleetManager {
   public:
    struct Io {
        std::function<void(bool enabled)> keysEnabled;  // button actions on or off
    };

    // Before power.begin(): the stored charge target, so the charger starts with it.
    void loadStored() {
        Preferences p;
        if (p.begin(NVS_NS, /*readOnly=*/true)) {
            _target = p.getUChar(KEY_TARGET, 0) == (uint8_t)fleet::Target::Event
                          ? fleet::Target::Event
                          : fleet::Target::Store;
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
        if (Config::wifiCount() == 0) Config::loadWifi();

        WiFi.macAddress(_mac);
        _filter.begin(_mac);
        if (!_rx.allocate(fleet::kRxQueue))
            Logger::e("[fleet] out of memory for the receive queue");
        _ps.begin(now);
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

    // Raw key edge. True: the fleet takes it (charging mode, standby wake,
    // key test), so no other module sees it.
    bool onKey(const char* role, bool pressed, uint32_t ms) {
        game::Key k = game::keyFromName(role);
        if (k == game::Key::L1) _l1Down = pressed;
        if (k == game::Key::R1) _r1Down = pressed;
        if (pressed) {
            _keysDown++;
        } else if (_keysDown > 0) {
            _keysDown--;
            if (_keysDown == 0) _keysUpMs = ms;
        }
        if (_test.kind() == fleet::FactoryTest::Kind::Keys) {
            if (pressed && k != game::Key::None) _link.onTestKey(k);
            _quietKeys = true;
            return true;
        }
        if (!_managed()) return false;
        switch (_ps.state()) {
            case fleet::State::Charging:
                _quietKeys = true;
                return true;
            case fleet::State::Standby:
                if (pressed) _ps.activity(millis());
                _quietKeys = true;
                return true;
            default:
                if (pressed) _ps.activity(millis());
                return _quietKeys;
        }
    }

    // Every loop() pass, also during an update download.
    void tick(uint32_t now) {
        game::RxRing::Frame f;
        while (_rx.pop(f)) _onFrameLoop(f, now);

        bool usb = _usb();
        bool session = _game->radioQuiet();
        if (!session && _game->inviteVisible(now)) _ps.activity(now);
        fleet::PowerState::Inputs in;
        in.managed = _managed();
        in.usb = usb;
        in.hostLink = _game->linkActive() || _usbHost();
        in.session = session;
        in.hold = _upd.running || _test.running();
        in.battery = _batteryPercent();
        fleet::State before = _ps.state();
        fleet::PowerState::Action action = _ps.tick(in, now);
        if (!_applied || _ps.state() != before) _apply(_ps.state());
        if (action == fleet::PowerState::Action::Sleep && !in.hold) _sleep();

        fleet::ChargerFacts facts = _facts();
        _charge = fleet::chargeStatus(facts);
        _errors = fleet::chargerErrors(facts);
        if (_charge != fleet::Charge::Full)
            _fullSince = 0;
        else if (_fullSince == 0)
            _fullSince = now | 1;

        _sender.tick(now);
        _tickUpdate();
        _tickTestSamples();
        _link.tick(now);
        _tickCombo(now);
        SoftAp::tick(now);
        _tickKeys(now);

        // `fleet update` on the bridge: it updates itself once the command's
        // repeats are out.
        if (_selfUpdate && _sender.idle()) {
            _selfUpdate = false;
            _startUpdate(_selfSource, _selfPr);
        }

        if (!session) {
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

    // The fleet draws the light: factory test, update, charging, standby.
    bool ownsLeds() const {
        return _led && (_test.running() || _upd.running || _ps.state() != fleet::State::Active);
    }

    // Full frame while ownsLeds().
    void render(uint32_t now) {
        if (!_led) return;
        _ledsOwned = true;
        game::Rgb px[game::kLedCount];
        if (_test.running())
            _test.frame(now, px);
        else if (_upd.running)
            fleet::renderUpdate(_upd.pct, now, px);
        else if (_ps.state() == fleet::State::Charging)
            fleet::renderCharging(_chargeView(now), now, px);
        else
            fleet::clearFrame(px);
        _show(px);
    }

    // The update display while an update writes the flash (any trigger).
    void renderUpdate(uint32_t now, uint8_t percent) {
        if (!_led) return;
        _ledsOwned = true;
        game::Rgb px[game::kLedCount];
        fleet::renderUpdate(percent, now, px);
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
    uint32_t _fullSince = 0;
    bool _applied = false;
    bool _radioInactive = false;
    bool _standbyRadio = false;
    wifi_ps_type_t _psBefore = WIFI_PS_NONE;
    bool _ledsOwned = false;

    uint8_t _keysDown = 0;
    uint32_t _keysUpMs = 0;
    bool _quietKeys = false;  // button actions stay off until all keys are up for kKeyQuietMs
    int8_t _keysOn = -1;      // last value given to Io::keysEnabled
    bool _l1Down = false;
    bool _r1Down = false;
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

    fleet::ChargerFacts _facts() const {
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
        uint16_t vbat = _vbatMv();
        f.lowBattery = vbat != 0 && vbat < PowerManager::UPDATE_MIN_MV;
        return f;
    }

    fleet::ChargeView _chargeView(uint32_t now) const {
        fleet::ChargeView v;
        v.charge = _charge;
        v.battery = _batteryPercent();
        v.fullForMs = _fullSince ? now - _fullSince : 0;
        return v;
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
        _setStandbyRadio(s == fleet::State::Standby);
        if (s != fleet::State::Active) _quietKeys = true;
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

    // Standby down to the storage level: deep sleep (src/fleet/DeepSleep.h).
    void _sleep() {
        if (!_managed() || _usb()) return;
        Logger::i("[fleet] battery at %u %%, standby ends - deep sleep, timer wake every %lu s",
                  _batteryPercent(), (unsigned long)fleet::kSleepWakeIntervalS);
        SoftAp::off();
        if (_led) {
            game::Rgb px[game::kLedCount];
            fleet::clearFrame(px);
            _show(px);
        }
        _power->prepareDeepSleep();
        Serial.flush();
        auto& c = Config::get();
        fleet::DeepSleep::enter(c.i2cSdaPin, c.i2cSclPin, _wakePins());
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
    void _tickKeys(uint32_t now) {
        if (_quietKeys && _ps.state() == fleet::State::Active && !_test.running() &&
            _keysDown == 0 && now - _keysUpMs >= fleet::kKeyQuietMs)
            _quietKeys = false;
        bool on = _ps.state() == fleet::State::Active && !_test.running() && !_quietKeys;
        if ((int8_t)on == _keysOn) return;
        _keysOn = on;
        if (_io.keysEnabled) _io.keysEnabled(on);
    }

    // L1 + R1 held kApComboMs: maintenance AP (active, outside a session).
    void _tickCombo(uint32_t now) {
        if (!(_l1Down && _r1Down)) {
            _comboSince = 0;
            _comboFired = false;
            return;
        }
        if (_comboSince == 0) _comboSince = now | 1;
        if (_comboFired || now - _comboSince < fleet::kApComboMs) return;
        _comboFired = true;
        if (_ps.state() != fleet::State::Active || _game->radioQuiet()) return;
        if (!SoftAp::openMaintenance(fleet::kApMinutesDefault, now))
            Logger::i("[fleet] L1+R1: this board runs its fallback AP instead");
    }

    // ── Charge target ────────────────────────────────────────────────────────
    void _setTarget(fleet::Target t) {
        if (t != _target) {
            _target = t;
            Preferences p;
            if (p.begin(NVS_NS, false)) {
                p.putUChar(KEY_TARGET, (uint8_t)t);
                p.end();
            }
        }
        // A running charge test restores the target when it ends.
        if (_test.kind() != fleet::FactoryTest::Kind::Charge) _power->setChargeVoltageMv(vregMv());
        Logger::i("[fleet] charge target %s (VREG %u mV)", fleet::kTargetNames[(uint8_t)t],
                  vregMv());
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
        // Devices in a game session ignore fleet commands.
        if (_game->radioQuiet() || !_filter.accept(m, now)) return;
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
    void _tickTestSamples() {
        uint32_t n = _power->adcCount();
        if (n == _lastAdcCount) return;
        _lastAdcCount = n;
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
        io.setTarget = [this](fleet::Target t) { _setTarget(t); };
        io.target = [this]() { return _target; };
        io.openAp = [](uint8_t minutes) {
            return SoftAp::openMaintenance(minutes, millis()) ? FleetLink::ApResult::Ok
                                                              : FleetLink::ApResult::Unsupported;
        };
        io.number = [this]() { return _game->number(); };
        io.fwBuild = [this]() { return _game->fwBuild(); };
        io.radioBusy = [this]() { return _game->radioQuiet(); };
        io.nonce = []() { return (uint32_t)esp_random(); };
        io.sent = [this](const fleet::FleetCommandMsg& m) {
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
