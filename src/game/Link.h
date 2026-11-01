#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <new>

#include "Bridge.h"
#include "GameConfig.h"
#include "GameTypes.h"

namespace game {

// USB link of the bridge (gateway protocol v0): NDJSON lines with the
// namespaces `link` and `game`. Parses commands, drives Bridge, and writes
// events without ever blocking: queued events (at most kEventQueueLines)
// first, then per-device `device`/`value` lines straight from the bridge's
// table, each only once a whole line fits the writer's free space.
//
// Plain C++ plus ArduinoJson; the serial port comes in as a Writer.
class Link : public Bridge::Listener {
   public:
    struct Writer {
        virtual ~Writer() = default;
        virtual int availableForWrite() = 0;
        virtual void write(const char* data, size_t len) = 0;
    };

    struct Io {
        std::function<uint32_t()> fwBuild;
        std::function<uint16_t()> loadNumber;  // 0 = none
        std::function<void(uint16_t)> storeNumber;
        std::function<bool()> participantBusy;  // in a session as participant
        std::function<void()> becameBridge;     // first `link hello` since boot
        std::function<uint32_t()> rxOverflow;
    };

    ~Link() override { delete[] _queue; }

    void begin(Bridge* bridge, Writer* writer, const Io& io, const uint8_t* mac) {
        _bridge = bridge;
        _writer = writer;
        _io = io;
        memcpy(_mac, mac, 6);
    }

    // Log output stays off the port while a link session is open.
    bool active() const { return _active; }
    // False while a new round waits for the old round's values to go out.
    bool readyForInput() const { return !_deferred; }
    uint32_t dropped() const { return _dropped; }

    void feedByte(uint8_t c, uint32_t now) {
        if (c == '\n') {
            size_t len = _len;
            bool tooLong = _discard;
            _len = 0;
            _discard = false;
            if (len > 0 && _buf[len - 1] == '\r') len--;
            if (tooLong || len > kLineMax) {
                if (_active) {
                    _lastRx = now;
                    _error(ReqId{}, "line_too_long", nullptr, nullptr, nullptr,
                           "line longer than 512 bytes");
                }
                return;
            }
            _processLine(_buf, len, now);
            return;
        }
        if (_discard) return;
        if (_len >= sizeof(_buf)) {
            _discard = true;
            return;
        }
        _buf[_len++] = (char)c;
    }

    void tick(uint32_t now) {
        if (_active && !_closing && now - _lastRx >= kLinkTimeoutMs) _close();
        _flush();
        if (_closing && _qCount == 0) _close();
        if (_deferred && (_bridge->dirtyValueCount() == 0 || !_active ||
                          now - _deferredSince >= kRoundFlushMaxMs)) {
            _deferred = false;
            _applyRound(_deferredCmd, _deferredId, now);
            _flush();
        }
    }

    // ── Bridge::Listener ─────────────────────────────────────────────────────
    void onJoin(const Bridge::JoinInfo& j) override {
        if (!_active) return;
        char mac[18], n[8], bat[8];
        formatMac(j.mac, mac);
        _numOrNull(n, j.n != 0, j.n);
        _numOrNull(bat, j.battery != kNoBattery, j.battery);
        char line[kEventLineMax];
        int len =
            snprintf(line, sizeof(line),
                     "{\"ns\":\"game\",\"ev\":\"join\",\"mac\":\"%s\",\"n\":%s,\"fw\":\"%08lx\","
                     "\"proto\":%u,\"fleet\":%u,\"bat\":%s,\"chg\":%s,\"rssi\":%d,"
                     "\"pressed\":%s,\"rejoin\":%s}\n",
                     mac, n, (unsigned long)j.fw, j.proto, j.fleet, bat, _b(j.charging), j.rssi,
                     _b(j.pressed), _b(j.rejoin));
        _enqueueRaw(line, len);
    }

    void onAdmitted(const ReqId& id, const uint8_t* mac, uint16_t n) override {
        if (!_active) return;
        JsonDocument d;
        _head(d, "game", "admitted", id);
        char m[18];
        formatMac(mac, m);
        d["mac"] = m;
        d["n"] = n;
        _enqueue(d);
    }

    void onAdmitTimeout(const ReqId& id, const uint8_t*, uint16_t) override {
        _error(id, "admit_timeout", "game", "admit", nullptr,
               "device did not confirm the admission within 5 s");
    }

    void onSessionEnded(const ReqId& id) override { _sendSession(id, /*forceInactive=*/true); }

   private:
    struct Slot {
        uint16_t len;
        char data[kEventLineMax];
    };

    Bridge* _bridge = nullptr;
    Writer* _writer = nullptr;
    Io _io;
    uint8_t _mac[6] = {};
    char _buf[kLineMax + 1];  // + a trailing '\r'
    size_t _len = 0;
    bool _discard = false;
    bool _active = false;
    bool _closing = false;
    bool _everBridge = false;
    uint32_t _lastRx = 0;
    uint32_t _dropped = 0;

    Slot* _queue = nullptr;  // kEventQueueLines, allocated on the first hello
    uint8_t _qHead = 0;
    uint8_t _qCount = 0;
    uint16_t _cursor = 1;

    bool _deferred = false;
    uint32_t _deferredSince = 0;
    Bridge::RoundCmd _deferredCmd;
    ReqId _deferredId;

    static const char* _b(bool v) { return v ? "true" : "false"; }

    static void _numOrNull(char* out, bool present, long v) {
        if (present)
            snprintf(out, 8, "%ld", v);
        else
            strcpy(out, "null");
    }

    // ── Output ───────────────────────────────────────────────────────────────
    void _close() {
        _active = false;
        _closing = false;
        _qCount = 0;
        _qHead = 0;
    }

    void _enqueueRaw(const char* line, int len) {
        if (!_queue || len <= 0 || len >= (int)kEventLineMax || _qCount >= kEventQueueLines) {
            _dropped++;
            return;
        }
        Slot& s = _queue[(_qHead + _qCount) % kEventQueueLines];
        memcpy(s.data, line, len);
        s.len = (uint16_t)len;
        _qCount++;
    }

    void _enqueue(JsonDocument& d) {
        char line[kEventLineMax];
        size_t n = serializeJson(d, line, sizeof(line) - 1);
        if (n == 0 || n >= sizeof(line) - 1) {
            _dropped++;
            return;
        }
        line[n++] = '\n';
        _enqueueRaw(line, (int)n);
    }

    bool _write(const char* line, size_t len) {
        if (!_writer || _writer->availableForWrite() < (int)len) return false;
        _writer->write(line, len);
        return true;
    }

    void _flush() {
        if (!_active) return;
        while (_qCount > 0) {
            Slot& s = _queue[_qHead];
            if (!_write(s.data, s.len)) return;
            _qHead = (uint8_t)((_qHead + 1) % kEventQueueLines);
            _qCount--;
        }
        if (_closing) return;
        uint16_t maxN = _bridge->maxNumber();
        if (_bridge->dirtyCount() == 0 || maxN == 0) return;
        char line[kEventLineMax];
        for (uint16_t scanned = 0; scanned < maxN && _bridge->dirtyCount() > 0; scanned++) {
            if (_cursor < 1 || _cursor > maxN) _cursor = 1;
            uint16_t n = _cursor;
            const Bridge::Device* dv = _bridge->device(n);
            if (dv->dirtyDevice) {
                char mac[18], bat[8];
                formatMac(dv->mac, mac);
                _numOrNull(bat, dv->battery != kNoBattery, dv->battery);
                int len = snprintf(line, sizeof(line),
                                   "{\"ns\":\"game\",\"ev\":\"device\",\"n\":%u,\"mac\":\"%s\","
                                   "\"fw\":\"%08lx\",\"proto\":%u,\"bat\":%s,\"chg\":%s,"
                                   "\"rssi\":%d,\"round\":%u,\"seq\":%u}\n",
                                   n, mac, (unsigned long)dv->fw, dv->proto, bat, _b(dv->charging),
                                   dv->rssi, dv->lastRound, dv->lastSeq);
                if (!_write(line, len)) return;
                _bridge->clearDirtyDevice(n);
            }
            if (dv->dirtyValue) {
                char bat[8];
                _numOrNull(bat, dv->valueBattery != kNoBattery, dv->valueBattery);
                int len = snprintf(line, sizeof(line),
                                   "{\"ns\":\"game\",\"ev\":\"value\",\"n\":%u,\"round\":%u,"
                                   "\"v\":%u,\"vseq\":%u,\"t\":%ld,\"arr\":%u,\"late\":%s,"
                                   "\"rssi\":%d,\"bat\":%s}\n",
                                   n, _bridge->stateInfo(0).round, dv->value, dv->vseq, (long)dv->t,
                                   dv->arr, _b(dv->late), dv->valueRssi, bat);
                if (!_write(line, len)) return;
                _bridge->clearDirtyValue(n);
            }
            _cursor = (uint16_t)(n % maxN + 1);
        }
    }

    void _head(JsonDocument& d, const char* ns, const char* ev, const ReqId& id) {
        d["ns"] = ns;
        d["ev"] = ev;
        if (id.kind == ReqId::Kind::Num)
            d["id"] = id.num;
        else if (id.kind == ReqId::Kind::Str)
            d["id"] = (const char*)id.str;
    }

    void _error(const ReqId& id, const char* code, const char* refNs, const char* refCmd,
                const char* field, const char* msg) {
        if (!_active) return;
        JsonDocument d;
        _head(d, "link", "error", id);
        d["code"] = code;
        if (refNs) d["refNs"] = refNs;
        if (refCmd) d["refCmd"] = refCmd;
        if (field) d["field"] = field;
        if (msg) d["msg"] = msg;
        _enqueue(d);
    }

    void _sendSession(const ReqId& id, bool forceInactive) {
        if (!_active) return;
        JsonDocument d;
        _head(d, "game", "session", id);
        bool active = _bridge->active() && !forceInactive;
        d["session"] = active || forceInactive ? _bridge->sessionId() : 0;
        d["channel"] = active || forceInactive ? _bridge->channel() : 0;
        d["fleet"] = active || forceInactive ? _bridge->fleet() : 0;
        d["login"] = active && _bridge->login();
        d["active"] = active;
        _enqueue(d);
    }

    void _sendState(const ReqId& id, bool changed, uint32_t now) {
        Bridge::StateInfo s = _bridge->stateInfo(now);
        JsonDocument d;
        _head(d, "game", "state", id);
        d["round"] = s.round;
        d["seq"] = s.seq;
        d["type"] = kRoundTypeNames[(uint8_t)s.type];
        if (s.type == RoundType::Idle)
            d["phase"] = nullptr;
        else
            d["phase"] = kPhaseNames[(uint8_t)s.phase];
        d["changed"] = changed;
        if (s.sinceOpenValid)
            d["sinceOpenMs"] = s.sinceOpenMs;
        else
            d["sinceOpenMs"] = nullptr;
        if (s.closedValid)
            d["closedAtMs"] = s.closedAtMs;
        else
            d["closedAtMs"] = nullptr;
        _enqueue(d);
    }

    // ── Input ────────────────────────────────────────────────────────────────
    enum class Field : uint8_t { Missing, Null, Ok, Bad };

    static Field _int(JsonObjectConst o, const char* key, long long lo, long long hi,
                      long long& out) {
        JsonVariantConst v = o[key];
        if (v.isUnbound()) return Field::Missing;
        if (v.isNull()) return Field::Null;
        if (v.is<bool>() || !v.is<long long>()) return Field::Bad;
        out = v.as<long long>();
        return (out < lo || out > hi) ? Field::Bad : Field::Ok;
    }

    static bool _parseId(JsonObjectConst o, ReqId& id) {
        JsonVariantConst v = o["id"];
        id = ReqId{};
        if (v.isUnbound()) return true;
        if (v.is<const char*>()) {
            const char* s = v.as<const char*>();
            if (strlen(s) > kIdMaxLen) return false;
            id.kind = ReqId::Kind::Str;
            strcpy(id.str, s);
            return true;
        }
        if (!v.is<bool>() && v.is<long long>()) {
            id.kind = ReqId::Kind::Num;
            id.num = v.as<long long>();
            return true;
        }
        return false;
    }

    void _processLine(const char* line, size_t len, uint32_t now) {
        if (_active) _lastRx = now;
        if (len == 0) return;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, line, len);
        if (err || !doc.is<JsonObjectConst>()) {
            if (_active)
                _error(ReqId{}, "bad_json", nullptr, nullptr, nullptr, "not a JSON object");
            return;
        }
        JsonObjectConst o = doc.as<JsonObjectConst>();
        const char* ns = o["ns"].is<const char*>() ? o["ns"].as<const char*>() : nullptr;
        const char* cmd = o["cmd"].is<const char*>() ? o["cmd"].as<const char*>() : nullptr;
        bool isHello = ns && cmd && strcmp(ns, "link") == 0 && strcmp(cmd, "hello") == 0;
        if (!_active && !isHello) return;  // a device becomes the bridge with `link hello`
        if (isHello && !_queue) {
            _queue = new (std::nothrow) Slot[kEventQueueLines];
            if (!_queue) return;
        }
        if (!_active) {
            // Provisional: an error reply still goes out, then the link closes
            // again unless the hello succeeds.
            _active = true;
            _closing = true;
            _lastRx = now;
        }
        ReqId id;
        if (!_parseId(o, id)) {
            _error(ReqId{}, "bad_field", ns, cmd, "id",
                   "id must be an integer or a string of 1-16 chars");
            return;
        }
        if (!ns) {
            _error(id, "bad_field", nullptr, nullptr, "ns", "ns must be a string");
            return;
        }
        if (strcmp(ns, "link") != 0 && strcmp(ns, "game") != 0) {
            // `config` and `scene` are reserved for later and not part of v0.
            _error(id, "unknown_ns", ns, cmd, nullptr, "supported namespaces: link, game");
            return;
        }
        if (!cmd) {
            _error(id, "bad_field", ns, nullptr, "cmd", "cmd must be a string");
            return;
        }
        if (strcmp(ns, "link") == 0)
            _linkCmd(o, cmd, id, now);
        else
            _gameCmd(o, cmd, id, now);
    }

    void _badField(const ReqId& id, const char* cmd, const char* field, const char* msg) {
        _error(id, "bad_field", "game", cmd, field, msg);
    }

    void _linkCmd(JsonObjectConst o, const char* cmd, const ReqId& id, uint32_t now) {
        if (strcmp(cmd, "hello") == 0) {
            long long v;
            Field f = _int(o, "link", 0, 0x7FFFFFFF, v);
            if (f != Field::Ok) {
                _error(id, "bad_field", "link", "hello", "link", "link must be an integer");
                return;
            }
            if (v != kLinkVersion) {
                _error(id, "version", "link", "hello", nullptr, "supported link version: 0");
                return;
            }
            if (!_everBridge) {
                _everBridge = true;
                if (_io.becameBridge) _io.becameBridge();
            }
            _active = true;
            _closing = false;
            _lastRx = now;
            JsonDocument d;
            _head(d, "link", "hello", id);
            d["link"] = kLinkVersion;
            JsonObject spaces = d["spaces"].to<JsonObject>();
            spaces["link"] = kLinkVersion;
            spaces["game"] = kGameVersion;
            char fw[9], mac[18];
            snprintf(fw, sizeof(fw), "%08lx", (unsigned long)(_io.fwBuild ? _io.fwBuild() : 0));
            formatMac(_mac, mac);
            d["fw"] = fw;
            d["mac"] = mac;
            d["proto"] = kProto;
            d["uptimeMs"] = now;
            _enqueue(d);
            // Replay: session, round state, participants, values.
            _sendSession(ReqId{}, false);
            if (_bridge->active()) _sendState(ReqId{}, false, now);
            _bridge->markAllDirty();
            return;
        }
        if (strcmp(cmd, "ping") == 0) {
            JsonDocument d;
            _head(d, "link", "pong", id);
            d["uptimeMs"] = now;
            d["rxFrames"] = _bridge->rxFrames();
            d["txFrames"] = _bridge->txFrames();
            d["rxOverflow"] = _io.rxOverflow ? _io.rxOverflow() : 0;
            d["dropped"] = _dropped;
            _enqueue(d);
            return;
        }
        if (strcmp(cmd, "bye") == 0) {
            JsonDocument d;
            _head(d, "link", "bye", id);
            _enqueue(d);
            _closing = true;
            return;
        }
        _error(id, "unknown_cmd", "link", cmd, nullptr, "link commands: hello, ping, bye");
    }

    void _gameCmd(JsonObjectConst o, const char* cmd, const ReqId& id, uint32_t now) {
        if (strcmp(cmd, "session") == 0) return _cmdSession(o, id, now);
        if (strcmp(cmd, "end") == 0) {
            if (_bridge->end(id, now) != Bridge::Err::None)
                _error(id, "no_session", "game", cmd, nullptr, "no active session");
            return;
        }
        if (strcmp(cmd, "admit") == 0) return _cmdAdmit(o, id, now);
        if (strcmp(cmd, "release") == 0) return _cmdRelease(o, id, now);
        if (strcmp(cmd, "round") == 0) return _cmdRound(o, id, now);
        if (strcmp(cmd, "number") == 0) return _cmdNumber(o, id);
        _error(id, "unknown_cmd", "game", cmd, nullptr,
               "game commands: session, end, admit, release, round, number");
    }

    void _cmdSession(JsonObjectConst o, const ReqId& id, uint32_t now) {
        long long session, channel, fleet = 0;
        if (_int(o, "session", 1, 65535, session) != Field::Ok)
            return _badField(id, "session", "session", "session must be 1-65535");
        if (_int(o, "channel", kMinChannel, kMaxChannel, channel) != Field::Ok)
            return _badField(id, "session", "channel", "channel must be 1-13");
        Field ff = _int(o, "fleet", 0, 65535, fleet);
        if (ff != Field::Ok && ff != Field::Missing)
            return _badField(id, "session", "fleet", "fleet must be 0-65535");
        JsonVariantConst login = o["login"];
        if (!login.is<bool>()) return _badField(id, "session", "login", "login must be a boolean");
        if (_io.participantBusy && _io.participantBusy())
            return _error(id, "session_active", "game", "session", nullptr,
                          "this device is a participant in a session");
        switch (_bridge->session((uint16_t)session, (uint8_t)channel, (uint16_t)fleet,
                                 login.as<bool>(), now)) {
            case Bridge::Err::None:
                return _sendSession(id, false);
            case Bridge::Err::SessionActive:
                return _error(id, "session_active", "game", "session", nullptr,
                              "another session is running; end it first");
            case Bridge::Err::NoMemory:
                return _error(id, "no_session", "game", "session", nullptr,
                              "out of memory for the device table");
            default:
                return _error(id, "no_session", "game", "session", nullptr, "session is ending");
        }
    }

    bool _mac6(JsonObjectConst o, const ReqId& id, const char* cmd, uint8_t* mac) {
        JsonVariantConst v = o["mac"];
        if (!v.is<const char*>() || !parseMac(v.as<const char*>(), mac)) {
            _badField(id, cmd, "mac", "mac must look like 02:00:00:00:00:11");
            return false;
        }
        return true;
    }

    void _cmdAdmit(JsonObjectConst o, const ReqId& id, uint32_t now) {
        if (!_bridge->active() || _bridge->ending())
            return _error(id, "no_session", "game", "admit", nullptr, "no active session");
        uint8_t mac[6];
        if (!_mac6(o, id, "admit", mac)) return;
        long long n;
        if (_int(o, "n", 1, kMaxNumber, n) != Field::Ok)
            return _badField(id, "admit", "n", "n must be 1-999");
        if (_bridge->admit(mac, (uint16_t)n, id, now) == Bridge::Err::Full)
            _error(id, "admit_timeout", "game", "admit", nullptr, "too many pending admits");
    }

    void _cmdRelease(JsonObjectConst o, const ReqId& id, uint32_t now) {
        if (!_bridge->active() || _bridge->ending())
            return _error(id, "no_session", "game", "release", nullptr, "no active session");
        uint8_t mac[6];
        if (!_mac6(o, id, "release", mac)) return;
        _bridge->release(mac, now);
        JsonDocument d;
        _head(d, "game", "released", id);
        char m[18];
        formatMac(mac, m);
        d["mac"] = m;
        _enqueue(d);
    }

    void _cmdNumber(JsonObjectConst o, const ReqId& id) {
        long long n = 0;
        Field f = _int(o, "n", 1, kMaxNumber, n);
        if (f == Field::Bad) return _badField(id, "number", "n", "n must be 1-999 or null");
        if (f == Field::Ok || f == Field::Null) {
            if (_io.storeNumber) _io.storeNumber(f == Field::Ok ? (uint16_t)n : 0);
        }
        uint16_t cur = _io.loadNumber ? _io.loadNumber() : 0;
        JsonDocument d;
        _head(d, "game", "number", id);
        if (cur)
            d["n"] = cur;
        else
            d["n"] = nullptr;
        _enqueue(d);
    }

    // Parses and checks every field of `game round` (gateway protocol v0 and
    // the round format's field rules). On failure it has sent bad_field.
    bool _parseRound(JsonObjectConst o, const ReqId& id, Bridge::RoundCmd& c) {
        const char* cmd = "round";
        long long v;
        if (_int(o, "round", 1, 65535, v) != Field::Ok) {
            _badField(id, cmd, "round", "round must be 1-65535");
            return false;
        }
        c.round = (uint16_t)v;
        int type = indexOfName(o["type"].is<const char*>() ? o["type"].as<const char*>() : nullptr,
                               kRoundTypeNames, kRoundTypeCount);
        if (type < 0) {
            _badField(id, cmd, "type", "type must be idle, race, scale or choice");
            return false;
        }
        c.type = (RoundType)type;
        bool idle = c.type == RoundType::Idle;

        // phase
        JsonVariantConst ph = o["phase"];
        if (idle) {
            if (!ph.isUnbound()) {
                _badField(id, cmd, "phase", "omit phase for idle");
                return false;
            }
            c.phase = Phase::Closed;
        } else {
            int p = indexOfName(ph.is<const char*>() ? ph.as<const char*>() : nullptr, kPhaseNames,
                                kPhaseCount);
            if (p < 0) {
                _badField(id, cmd, "phase", "phase must be announced, open or closed");
                return false;
            }
            c.phase = (Phase)p;
        }

        // keys
        JsonVariantConst keys = o["keys"];
        if (idle) {
            if (!keys.isUnbound()) {
                _badField(id, cmd, "keys", "omit keys for idle");
                return false;
            }
        } else {
            if (!keys.is<JsonObjectConst>()) {
                _badField(id, cmd, "keys", "keys must be an object");
                return false;
            }
            for (JsonPairConst kv : keys.as<JsonObjectConst>()) {
                Key k = keyFromName(kv.key().c_str());
                if (k == Key::L1 || k == Key::L2 || k == Key::R1 || k == Key::R2) {
                    _badField(id, cmd, "keys", "l1, l2, r1, r2 are reserved in v0");
                    return false;
                }
                if ((uint8_t)k > (uint8_t)Key::Enter) {
                    _badField(id, cmd, "keys", "keys: buzz, up, down, left, right, enter");
                    return false;
                }
                int col = indexOfName(
                    kv.value().is<const char*>() ? kv.value().as<const char*>() : nullptr,
                    kColorNames, kColorCount);
                if (col <= 0) {  // "off" is not a key colour
                    _badField(id, cmd, "keys",
                              "colors: white, red, green, yellow, blue, magenta, cyan, orange");
                    return false;
                }
                c.keyMask |= (uint16_t)(1u << (uint8_t)k);
                setKeyColor(c.keyColor, (uint8_t)k, (uint8_t)col);
            }
            auto keyBit = [](Key k) { return (uint16_t)(1u << (uint8_t)k); };
            bool ok;
            const char* rule;
            if (c.type == RoundType::Race) {
                ok = c.keyMask == keyBit(Key::Buzz);
                rule = "race takes exactly buzz";
            } else if (c.type == RoundType::Scale) {
                ok = c.keyMask == (keyBit(Key::Up) | keyBit(Key::Down));
                rule = "scale takes exactly up and down";
            } else {
                uint16_t two = keyBit(Key::Up) | keyBit(Key::Right);
                uint16_t three = two | keyBit(Key::Down);
                uint16_t four = three | keyBit(Key::Left);
                ok = c.keyMask == two || c.keyMask == three || c.keyMask == four;
                rule = "choice takes up, right, down, left in this order, 2-4 keys";
            }
            if (!ok) {
                _badField(id, cmd, "keys", rule);
                return false;
            }
        }

        // scale
        const char* scaleKeys[4] = {"min", "max", "redMax", "greenMin"};
        if (c.type != RoundType::Scale) {
            for (const char* k : scaleKeys)
                if (!o[k].isUnbound()) {
                    _badField(id, cmd, k, "only for scale");
                    return false;
                }
        } else {
            long long mn, mx, rm, gm;
            if (_int(o, "min", 0, 98, mn) != Field::Ok) {
                _badField(id, cmd, "min", "min must be 0-98");
                return false;
            }
            if (_int(o, "max", mn + 1, 99, mx) != Field::Ok) {
                _badField(id, cmd, "max", "max must be > min and <= 99");
                return false;
            }
            if (mx - mn + 1 > kScoreCount) {
                _badField(id, cmd, "max", "max - min + 1 must be <= 24");
                return false;
            }
            if (_int(o, "redMax", mn, mx, rm) != Field::Ok) {
                _badField(id, cmd, "redMax", "redMax must be >= min");
                return false;
            }
            if (_int(o, "greenMin", rm + 1, mx, gm) != Field::Ok) {
                _badField(id, cmd, "greenMin", "greenMin must be > redMax and <= max");
                return false;
            }
            c.min = (uint8_t)mn;
            c.max = (uint8_t)mx;
            c.redMax = (uint8_t)rm;
            c.greenMin = (uint8_t)gm;
        }

        // openInMs
        Field f = _int(o, "openInMs", 0, 5000, v);
        if (f != Field::Missing) {
            if (f != Field::Ok) {
                _badField(id, cmd, "openInMs", "openInMs must be 0-5000");
                return false;
            }
            if (idle || c.phase != Phase::Open) {
                _badField(id, cmd, "openInMs", "only with phase open");
                return false;
            }
            c.hasOpenIn = true;
            c.openInMs = (uint16_t)v;
        }

        // winner
        f = _int(o, "winner", 0, kMaxNumber, v);
        if (f != Field::Missing) {
            if (c.type != RoundType::Race) {
                _badField(id, cmd, "winner", "only for race");
                return false;
            }
            if (f != Field::Ok) {
                _badField(id, cmd, "winner", "winner must be 0-999");
                return false;
            }
            c.winner = (uint16_t)v;
        }

        // correct
        f = _int(o, "correct", 1, 4, v);
        if (f != Field::Missing) {
            if (c.type != RoundType::Choice || c.phase != Phase::Closed) {
                _badField(id, cmd, "correct", "only for choice in phase closed");
                return false;
            }
            uint8_t answers = 0;
            for (Key k : {Key::Up, Key::Right, Key::Down, Key::Left})
                if (c.keyMask & (1u << (uint8_t)k)) answers++;
            if (f != Field::Ok || v > answers) {
                _badField(id, cmd, "correct", "correct must be 1 to the number of answers");
                return false;
            }
            c.correct = (uint8_t)v;
        }
        return true;
    }

    void _cmdRound(JsonObjectConst o, const ReqId& id, uint32_t now) {
        if (!_bridge->active() || _bridge->ending())
            return _error(id, "no_session", "game", "round", nullptr, "no active session");
        Bridge::RoundCmd c;
        if (!_parseRound(o, id, c)) return;
        // New round: the old round's values go out first (at most 200 ms).
        if (_bridge->isNewRound(c.round) && _bridge->dirtyValueCount() > 0) {
            _deferred = true;
            _deferredSince = now;
            _deferredCmd = c;
            _deferredId = id;
            return;
        }
        _applyRound(c, id, now);
    }

    void _applyRound(const Bridge::RoundCmd& c, const ReqId& id, uint32_t now) {
        bool changed = false;
        switch (_bridge->round(c, now, changed)) {
            case Bridge::Err::None:
                return _sendState(id, changed, now);
            case Bridge::Err::RoundFixed:
                return _error(id, "round_fixed", "game", "round", nullptr,
                              "type, keys and scale of a running round are fixed");
            case Bridge::Err::BadPhase:
                return _error(id, "bad_phase", "game", "round", nullptr,
                              "phases only move forward");
            default:
                return _error(id, "no_session", "game", "round", nullptr, "no active session");
        }
    }
};

}  // namespace game
