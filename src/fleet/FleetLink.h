#pragma once
#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

#include "../game/GameTypes.h"
#include "../game/Link.h"
#include "FactoryTest.h"
#include "FleetConfig.h"
#include "FleetFrames.h"
#include "FleetRadio.h"
#include "FleetTypes.h"

// The fleet package's namespaces on the USB link (docs/usb-link.md):
//   config  any device on USB: WiFi list, charge target, maintenance AP
//   test    any device on USB: factory test
//   fleet   the bridge only (after `link hello`): fleet commands to the mesh,
//           and a `status` event per device from its FleetStatus frames
// Plain C++ plus ArduinoJson; everything device-specific comes in through Io.
namespace fleet {

static constexpr uint8_t kConfigVersion = 0;
static constexpr uint8_t kFleetVersion = 0;
static constexpr uint8_t kTestVersion = 0;

class FleetLink : public game::Link::Extension {
   public:
    using Link = game::Link;
    using ReqId = game::ReqId;

    struct WifiNet {
        const char* ssid;
        const char* pass;
    };

    enum class ApResult : uint8_t { Ok, Unsupported };

    // `test info`: what the device reads about itself.
    struct TestInfo {
        uint8_t mac[6] = {};
        uint32_t fw = 0;
        uint16_t number = 0;
        bool chargerFound = false;
        uint8_t chargerRev = 0;
        uint8_t fault = 0;  // REG0x1F FAULT_Status_0
        Charge charge = Charge::None;
        bool adcValid = false;
        uint16_t vbatMv = 0;
        uint16_t vbusMv = 0;
        int16_t ibusMa = 0;
        bool ibatValid = false;
        int16_t ibatMa = 0;
        bool tsValid = false;  // TS is converted on USB only
        uint16_t tsPermille = 0;
        bool tsTempValid = false;
        int16_t tsDeciC = 0;
        int16_t tdieDeciC = 0;
        bool imuFound = false;
        uint8_t imuId = 0;
        bool accelValid = false;
        float accelG[3] = {};
    };

    struct Io {
        // config
        std::function<bool(const WifiNet* nets, uint8_t count)> setWifi;
        std::function<uint8_t()> wifiCount;
        // Stores the GitHub token for updates ("" clears it); false if it
        // could not be stored.
        std::function<bool(const char* token)> setToken;
        std::function<bool()> hasToken;
        // Stores the target (NVS) and applies it; false if it could not be stored.
        std::function<bool(Target)> setTarget;
        std::function<Target()> target;
        std::function<ApResult(uint8_t minutes)> openAp;
        // Stores the device's own colours (NVS) and shows them; false if they
        // could not be stored.
        std::function<bool(const Look&)> setLook;
        std::function<uint8_t()> lookLeds;
        // Stores the device name (config file); it applies from the next
        // restart. False if it could not be stored.
        std::function<bool(const char*)> setName;
        std::function<const char*()> name;
        std::function<uint16_t()> number;
        std::function<uint32_t()> fwBuild;
        // fleet
        std::function<bool()> radioBusy;  // the bridge's radio belongs to a game session
        std::function<uint32_t()> nonce;
        // A command was queued for sending (the bridge applies `charge`,
        // `brightness` and `update` to itself).
        std::function<void(const FleetCommandMsg&)> sent;
        // test
        std::function<void(TestInfo&)> testInfo;
        std::function<bool()> usbPower;
        std::function<bool()> participantBusy;  // in a session: LEDs and keys belong to it
        std::function<void()> reboot;
    };

    void begin(Link* link, const Io& io, FleetTable* table, CommandSender* sender,
               FactoryTest* test) {
        _link = link;
        _io = io;
        _table = table;
        _sender = sender;
        _test = test;
    }

    // Every loop() pass: test results, the restart after `test reboot`.
    void tick(uint32_t now) {
        _now = now;
        if (_test) {
            if (_link && !_link->active() && _test->running()) _test->abort();
            FactoryTest::Result r;
            if (_test->tick(now, r)) _emitTestResult(r);
        }
        if (_rebootPending && _link && _link->queueEmpty() && (int32_t)(now - _rebootAt) >= 0) {
            _rebootPending = false;
            if (_io.reboot) _io.reboot();
        }
    }

    // A key press during `test keys`: report it. True if the test took it.
    bool onTestKey(game::Key k) {
        if (!_test || !_test->onKey(k)) return false;
        if (!_link || !_link->active()) return true;
        JsonDocument d;
        Link::head(d, "test", "key", ReqId{});
        d["key"] = game::kKeyNames[(uint8_t)k];
        _link->emit(d);
        return true;
    }

    // ── Link::Extension ──────────────────────────────────────────────────────
    Scope scope(const char* ns) const override {
        if (strcmp(ns, "config") == 0 || strcmp(ns, "test") == 0) return Scope::Device;
        if (strcmp(ns, "fleet") == 0) return Scope::Bridge;
        return Scope::None;
    }

    const char* names() const override { return "config, fleet, test"; }

    void addSpaces(JsonObject spaces) override {
        spaces["config"] = kConfigVersion;
        spaces["fleet"] = kFleetVersion;
        spaces["test"] = kTestVersion;
    }

    void onHello() override {
        if (!_table) return;
        _table->allocate();
        _table->markAllDirty();
    }

    void flushTable(Link& link, uint32_t now) override {
        if (!_table) return;
        char line[game::kEventLineMax];
        while (FleetTable::Entry* e = _table->nextDirty()) {
            int len = formatStatus(*e, now, line, sizeof(line));
            if (len <= 0 || len >= (int)sizeof(line)) {
                _table->clearDirty(e);
                continue;
            }
            if (!link.writeNow(line, (size_t)len)) return;
            _table->clearDirty(e);
        }
    }

    void command(Link& link, const char* ns, const char* cmd, JsonObjectConst o, const ReqId& id,
                 uint32_t now) override {
        _now = now;
        if (strcmp(ns, "config") == 0) return _config(link, cmd, o, id);
        if (strcmp(ns, "fleet") == 0) return _fleet(link, cmd, o, id, now);
        _testCmd(link, cmd, id, now);
    }

    // One `fleet status` line with its newline; returns its length.
    static int formatStatus(const FleetTable::Entry& e, uint32_t now, char* out, size_t size) {
        const FleetStatusMsg& s = e.st;
        char mac[18], n[8], bat[8], vbat[8];
        game::formatMac(e.mac, mac);
        _numOrNull(n, s.number != 0, s.number);
        _numOrNull(bat, s.battery != kNoBattery, s.battery);
        _numOrNull(vbat, s.vbatMv != 0, s.vbatMv);
        uint32_t age = (now - e.heardMs) / 1000;
        return snprintf(
            out, size,
            "{\"ns\":\"fleet\",\"ev\":\"status\",\"mac\":\"%s\",\"n\":%s,\"state\":\"%s\","
            "\"bat\":%s,\"vbat\":%s,\"chg\":\"%s\",\"target\":\"%s\",\"usb\":%s,"
            "\"wifi\":%s,\"fw\":\"%08lx\",\"upd\":\"%s\",\"pct\":%u,\"updErr\":\"%s\",\"err\":%u,"
            "\"rssi\":%d,\"age\":%lu}\n",
            mac, n, _name(kStateNames, 3, s.state), bat, vbat, _name(kChargeNames, 5, s.charge),
            _name(kTargetNames, 2, s.target), _b(s.flags & kFlagUsb), _b(s.flags & kFlagWifi),
            (unsigned long)s.fw, _name(kUpdateNames, 5, s.update), s.updatePct,
            s.updateError == (uint8_t)UpdateError::None
                ? ""
                : _name(kUpdateErrorNames, 5, s.updateError),
            s.errors, e.rssi, (unsigned long)age);
    }

   private:
    Link* _link = nullptr;
    Io _io;
    FleetTable* _table = nullptr;
    CommandSender* _sender = nullptr;
    FactoryTest* _test = nullptr;
    uint32_t _now = 0;
    bool _rebootPending = false;
    uint32_t _rebootAt = 0;

    static const char* _b(bool v) { return v ? "true" : "false"; }

    static void _numOrNull(char* out, bool present, long v) {
        if (present)
            snprintf(out, 8, "%ld", v);
        else
            strcpy(out, "null");
    }

    static const char* _name(const char* const* names, uint8_t count, uint8_t i) {
        return i < count ? names[i] : "unknown";
    }

    static const char* _str(JsonObjectConst o, const char* key) {
        return o[key].is<const char*>() ? o[key].as<const char*>() : nullptr;
    }

    static void _ok(JsonDocument& d) { d["ok"] = true; }

    void _badField(Link& link, const ReqId& id, const char* ns, const char* cmd, const char* field,
                   const char* msg) {
        link.error(id, "bad_field", ns, cmd, field, msg);
    }

    // ── config ───────────────────────────────────────────────────────────────
    void _config(Link& link, const char* cmd, JsonObjectConst o, const ReqId& id) {
        if (strcmp(cmd, "wifi") == 0) return _configWifi(link, o, id);
        if (strcmp(cmd, "look") == 0) return _configLook(link, o, id);
        if (strcmp(cmd, "name") == 0) return _configName(link, o, id);
        if (strcmp(cmd, "token") == 0) {
            // Never echoed or logged.
            const char* token = _str(o, "token");
            if (!token || strlen(token) > kTokenMax)
                return _badField(link, id, "config", cmd, "token",
                                 "token must be a string of 0-127 bytes");
            if (!_io.setToken || !_io.setToken(token))
                return link.error(id, "store_failed", "config", cmd, nullptr,
                                  "the token could not be saved");
            JsonDocument d;
            Link::head(d, "config", "token", id);
            _ok(d);
            d["token"] = token[0] != '\0';
            link.emit(d);
            return;
        }
        if (strcmp(cmd, "charge") == 0) {
            int t = nameIndex(_str(o, "target"), kTargetNames, 2);
            if (t < 0)
                return _badField(link, id, "config", cmd, "target",
                                 "target must be store or event");
            if (!_io.setTarget || !_io.setTarget((Target)t))
                return link.error(id, "store_failed", "config", cmd, nullptr,
                                  "the charge target could not be saved");
            JsonDocument d;
            Link::head(d, "config", "charge", id);
            _ok(d);
            d["target"] = kTargetNames[t];
            link.emit(d);
            return;
        }
        if (strcmp(cmd, "ap") == 0) {
            long long minutes = kApMinutesDefault;
            Link::Field f = Link::intField(o, "minutes", 1, kApMinutesMax, minutes);
            if (f != Link::Field::Ok && f != Link::Field::Missing)
                return _badField(link, id, "config", cmd, "minutes", "minutes must be 1-60");
            ApResult r = _io.openAp ? _io.openAp((uint8_t)minutes) : ApResult::Unsupported;
            if (r == ApResult::Unsupported)
                return link.error(id, "unsupported", "config", cmd, nullptr,
                                  "this device runs its fallback AP instead");
            JsonDocument d;
            Link::head(d, "config", "ap", id);
            _ok(d);
            d["minutes"] = minutes;
            link.emit(d);
            return;
        }
        if (strcmp(cmd, "get") == 0) {
            JsonDocument d;
            Link::head(d, "config", "config", id);
            d["wifiCount"] = _io.wifiCount ? _io.wifiCount() : 0;
            d["token"] = _io.hasToken ? _io.hasToken() : false;
            Target t = _io.target ? _io.target() : Target::Store;
            d["target"] = kTargetNames[(uint8_t)t];
            d["look"] = _io.lookLeds ? _io.lookLeds() != 0 : false;
            d["name"] = _io.name ? _io.name() : "";
            uint16_t n = _io.number ? _io.number() : 0;
            if (n)
                d["number"] = n;
            else
                d["number"] = nullptr;
            char fw[9];
            snprintf(fw, sizeof(fw), "%08lx", (unsigned long)(_io.fwBuild ? _io.fwBuild() : 0));
            d["fw"] = fw;
            link.emit(d);
            return;
        }
        link.error(id, "unknown_cmd", "config", cmd, nullptr,
                   "config commands: wifi, token, charge, ap, look, name, get");
    }

    // Replaces the whole list. Passwords are never echoed or logged.
    static bool _hexColor(const char* s, uint8_t rgb[3]) {
        if (!s || strlen(s) != 7 || s[0] != '#') return false;
        for (uint8_t i = 1; i < 7; i++)
            if (!isxdigit((unsigned char)s[i])) return false;
        unsigned long v = strtoul(s + 1, nullptr, 16);
        rgb[0] = (uint8_t)(v >> 16);
        rgb[1] = (uint8_t)(v >> 8);
        rgb[2] = (uint8_t)v;
        return true;
    }

    void _configLook(Link& link, JsonObjectConst o, const ReqId& id) {
        JsonVariantConst v = o["leds"];
        if (!v.is<JsonArrayConst>())
            return _badField(link, id, "config", "look", "leds", "leds must be an array");
        JsonArrayConst arr = v.as<JsonArrayConst>();
        if (arr.size() != 0 && arr.size() != kLookLeds)
            return _badField(link, id, "config", "look", "leds", "0 or 24 colours");
        Look look;
        for (JsonVariantConst c : arr) {
            if (!c.is<const char*>() || !_hexColor(c.as<const char*>(), look.rgb[look.count]))
                return _badField(link, id, "config", "look", "leds", "colour must be #rrggbb");
            look.count++;
        }
        if (!_io.setLook || !_io.setLook(look))
            return link.error(id, "store_failed", "config", "look", nullptr,
                              "the colours could not be saved");
        JsonDocument d;
        Link::head(d, "config", "look", id);
        _ok(d);
        d["leds"] = look.count;
        link.emit(d);
    }

    // One DNS label of 1-kNameMax characters: a-z, 0-9 and '-', not at either end.
    static bool _validName(const char* s) {
        if (!s) return false;
        size_t n = strlen(s);
        if (n == 0 || n > kNameMax || s[0] == '-' || s[n - 1] == '-') return false;
        for (size_t i = 0; i < n; i++)
            if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
                return false;
        return true;
    }

    void _configName(Link& link, JsonObjectConst o, const ReqId& id) {
        const char* name = _str(o, "name");
        if (!_validName(name))
            return _badField(link, id, "config", "name", "name",
                             "name must be 1-31 of a-z, 0-9 and -, not starting or ending with -");
        if (!_io.setName || !_io.setName(name))
            return link.error(id, "store_failed", "config", "name", nullptr,
                              "the name could not be saved");
        JsonDocument d;
        Link::head(d, "config", "name", id);
        _ok(d);
        d["name"] = name;
        link.emit(d);
    }

    void _configWifi(Link& link, JsonObjectConst o, const ReqId& id) {
        JsonVariantConst v = o["networks"];
        if (!v.is<JsonArrayConst>())
            return _badField(link, id, "config", "wifi", "networks", "networks must be an array");
        JsonArrayConst arr = v.as<JsonArrayConst>();
        if (arr.size() > kWifiNetworksMax)
            return _badField(link, id, "config", "wifi", "networks", "at most 5 networks");
        WifiNet nets[kWifiNetworksMax];
        uint8_t count = 0;
        for (JsonVariantConst e : arr) {
            const char* ssid = e["ssid"].is<const char*>() ? e["ssid"].as<const char*>() : nullptr;
            const char* pass = e["pass"].is<const char*>() ? e["pass"].as<const char*>() : nullptr;
            size_t sl = ssid ? strlen(ssid) : 0;
            if (sl < 1 || sl > kSsidMax)
                return _badField(link, id, "config", "wifi", "ssid", "ssid must be 1-32 bytes");
            size_t pl = pass ? strlen(pass) : 0;
            if (!pass || (pl != 0 && (pl < kPassMin || pl > kPassMax)))
                return _badField(link, id, "config", "wifi", "pass",
                                 "pass must be \"\" or 8-63 bytes");
            nets[count++] = WifiNet{ssid, pass};
        }
        if (!_io.setWifi || !_io.setWifi(nets, count))
            return link.error(id, "store_failed", "config", "wifi", nullptr,
                              "the WiFi list could not be saved");
        JsonDocument d;
        Link::head(d, "config", "wifi", id);
        _ok(d);
        d["count"] = count;
        link.emit(d);
    }

    // ── fleet ────────────────────────────────────────────────────────────────
    void _fleet(Link& link, const char* cmd, JsonObjectConst o, const ReqId& id, uint32_t now) {
        FleetCommandMsg m;
        memset(&m, 0, sizeof(m));
        m.type = kMsgFleetCommand;
        m.proto = kProto;
        if (strcmp(cmd, "update") == 0) {
            const char* src = _str(o, "source");
            m.op = (uint8_t)Op::Update;
            if (src && strcmp(src, "release") == 0) {
                if (!o["pr"].isUnbound())
                    return _badField(link, id, "fleet", cmd, "pr", "pr only with source pr");
                m.arg = (uint8_t)Source::Release;
            } else if (src && strcmp(src, "pr") == 0) {
                long long pr;
                if (Link::intField(o, "pr", 1, 65535, pr) != Link::Field::Ok)
                    return _badField(link, id, "fleet", cmd, "pr", "pr must be 1-65535");
                m.arg = (uint8_t)Source::Pr;
                m.pr = (uint16_t)pr;
            } else {
                return _badField(link, id, "fleet", cmd, "source", "source must be release or pr");
            }
        } else if (strcmp(cmd, "charge") == 0) {
            int t = nameIndex(_str(o, "target"), kTargetNames, 2);
            if (t < 0)
                return _badField(link, id, "fleet", cmd, "target", "target must be store or event");
            m.op = (uint8_t)Op::Charge;
            m.arg = (uint8_t)t;
        } else if (strcmp(cmd, "off") == 0) {
            m.op = (uint8_t)Op::Off;
        } else if (strcmp(cmd, "ap") == 0) {
            const char* mac = _str(o, "mac");
            if (!mac || !game::parseMac(mac, m.targets[0]))
                return _badField(link, id, "fleet", cmd, "mac",
                                 "mac must look like 02:00:00:00:00:11");
            m.op = (uint8_t)Op::Ap;
            m.arg = kApMinutesDefault;
            m.targetCount = 1;
        } else if (strcmp(cmd, "brightness") == 0) {
            long long level;
            if (Link::intField(o, "level", 0, 255, level) != Link::Field::Ok)
                return _badField(link, id, "fleet", cmd, "level", "level must be 0-255");
            m.op = (uint8_t)Op::Brightness;
            m.arg = (uint8_t)level;
        } else {
            return link.error(id, "unknown_cmd", "fleet", cmd, nullptr,
                              "fleet commands: update, charge, off, ap, brightness");
        }
        // `brightness` also reaches the devices of a running session.
        if ((Op)m.op != Op::Brightness && _io.radioBusy && _io.radioBusy())
            return link.error(id, "session_active", "fleet", cmd, nullptr,
                              "the radio belongs to the game session");
        m.nonce = _io.nonce ? _io.nonce() : now;
        if (!_sender || !_sender->queue(m, now))
            return link.error(id, "busy", "fleet", cmd, nullptr,
                              "earlier commands are still being repeated");
        if (_io.sent) _io.sent(m);
        JsonDocument d;
        Link::head(d, "fleet", "sent", id);
        link.emit(d);
    }

    // ── test ─────────────────────────────────────────────────────────────────
    void _testCmd(Link& link, const char* cmd, const ReqId& id, uint32_t now) {
        if (strcmp(cmd, "info") == 0) return _testInfo(link, id);
        if (strcmp(cmd, "reboot") == 0) {
            JsonDocument d;
            Link::head(d, "test", "reboot", id);
            _ok(d);
            link.emit(d);
            _rebootPending = true;
            _rebootAt = now + kTestRebootDelayMs;
            return;
        }
        if (strcmp(cmd, "keys-end") == 0) {
            if (_test) _test->endKeys();
            JsonDocument d;
            Link::head(d, "test", "keys-end", id);
            _ok(d);
            link.emit(d);
            return;
        }
        bool leds = strcmp(cmd, "leds") == 0;
        bool keys = strcmp(cmd, "keys") == 0;
        bool charge = strcmp(cmd, "charge") == 0;
        if (!leds && !keys && !charge)
            return link.error(id, "unknown_cmd", "test", cmd, nullptr,
                              "test commands: info, leds, keys, keys-end, charge, reboot");
        if (_io.participantBusy && _io.participantBusy())
            return link.error(id, "session_active", "test", cmd, nullptr,
                              "this device is a participant in a session");
        if (!_test || _test->running())
            return link.error(id, "busy", "test", cmd, nullptr, "another test is running");
        if (charge && !(_io.usbPower && _io.usbPower()))
            return link.error(id, "no_usb", "test", cmd, nullptr, "needs USB power and a charger");
        if (leds) {
            _test->startLeds(id, now);
            return;
        }
        if (charge) {
            _test->startCharge(id, now);
            return;
        }
        _test->startKeys(id, now);
        JsonDocument d;
        Link::head(d, "test", "keys", id);
        _ok(d);
        link.emit(d);
    }

    void _emitTestResult(const FactoryTest::Result& r) {
        if (!_link || !_link->active()) return;
        JsonDocument d;
        if (r.kind == FactoryTest::Kind::Leds) {
            Link::head(d, "test", "leds", r.id);
            _ok(d);
        } else {
            Link::head(d, "test", "charge", r.id);
            d["ok"] = r.samples > 0;
            d["samples"] = r.samples;
            if (r.samples > 0) {
                d["ibus"] = r.ibusMa;
                d["ibat"] = r.ibatMa;
                d["vbus"] = r.vbusMv;
            } else {
                d["ibus"] = nullptr;
                d["ibat"] = nullptr;
                d["vbus"] = nullptr;
            }
        }
        _link->emit(d);
    }

    static float _roundTo(float v, float step) { return roundf(v / step) * step; }

    void _testInfo(Link& link, const ReqId& id) {
        TestInfo t;
        if (_io.testInfo) _io.testInfo(t);
        JsonDocument d;
        Link::head(d, "test", "info", id);
        char mac[18], fw[9];
        game::formatMac(t.mac, mac);
        snprintf(fw, sizeof(fw), "%08lx", (unsigned long)t.fw);
        d["mac"] = mac;
        d["fw"] = fw;
        if (t.number)
            d["n"] = t.number;
        else
            d["n"] = nullptr;
        JsonObject c = d["charger"].to<JsonObject>();
        c["found"] = t.chargerFound;
        if (t.chargerFound) {
            c["rev"] = t.chargerRev;
            c["fault"] = t.fault;
        }
        if (t.adcValid) {
            d["vbat"] = t.vbatMv;
            d["vbus"] = t.vbusMv;
            d["ibus"] = t.ibusMa;
            if (t.ibatValid)
                d["ibat"] = t.ibatMa;
            else
                d["ibat"] = nullptr;
            if (t.tsValid)
                d["ts"] = t.tsPermille / 10.0f;
            else
                d["ts"] = nullptr;
            if (t.tsTempValid)
                d["tsC"] = t.tsDeciC / 10.0f;
            else
                d["tsC"] = nullptr;
            d["tdie"] = t.tdieDeciC / 10.0f;
        } else {
            for (const char* k : {"vbat", "vbus", "ibus", "ibat", "ts", "tsC", "tdie"})
                d[k] = nullptr;
        }
        d["chg"] = kChargeNames[(uint8_t)t.charge];
        if (t.imuFound) {
            JsonObject imu = d["imu"].to<JsonObject>();
            imu["id"] = t.imuId;
            if (t.accelValid) {
                JsonArray g = imu["g"].to<JsonArray>();
                for (float a : t.accelG) g.add(_roundTo(a, 0.001f));
            } else {
                imu["g"] = nullptr;
            }
        } else {
            d["imu"] = nullptr;
        }
        link.emit(d);
    }
};

}  // namespace fleet
