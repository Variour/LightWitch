#pragma once
#include <stdint.h>
#include <string.h>

#include <functional>

#include "GameConfig.h"
#include "GameFrames.h"
#include "GameTypes.h"

namespace game {

// Participant role: joins a session on a buzz press during an invite, then
// shows the bridge's round state, takes key input and reports its value until
// the bridge's echo matches. Leaving the session always ends in a restart
// into normal operation (round format v0, "Zurück in den Normalbetrieb").
//
// Plain C++: time, randomness, radio, storage and restart come in through Io,
// so this builds and runs on the host.
class Participant {
   public:
    enum class State : uint8_t {
        Normal,        // regular LightWitch device, watching for invites
        RejoinListen,  // booted with a session entry, waiting for that session's bridge
        Joining,       // sending join requests until admitted
        Member,        // admitted quiet participant
        Leaving,       // LEDs off, restart pending
    };

    struct Health {
        uint8_t battery = kNoBattery;
        bool charging = false;
    };

    struct Io {
        std::function<void(const uint8_t*, size_t)> send;
        std::function<void(uint8_t)> setChannel;
        std::function<uint32_t(uint32_t, uint32_t)> randomRange;  // inclusive
        std::function<void(uint16_t)> storeNumber;                // 0 = none
        std::function<void(uint16_t, uint8_t)> storeSession;      // session 0 = clear
        std::function<void()> enterQuiet;  // runtime join: stop normal operation
        std::function<void()> restart;
        std::function<Health()> health;
        std::function<uint32_t()> fwBuild;
    };

    struct Round {
        bool known = false;
        uint16_t id = 0;
        uint16_t seq = 0;
        RoundType type = RoundType::Idle;
        Phase phase = Phase::Closed;
        uint16_t keyMask = 0;
        uint8_t keyColor[5] = {};
        uint8_t min = 0, max = 0, redMax = 0, greenMin = 0;
        uint16_t winner = 0;
        uint8_t correct = 0;
        bool startKnown = false;
        uint32_t startMs = 0;  // local round start (bridge open moment)
    };

    // True if the stored session entry asks for a quiet boot (round format v0,
    // "Neustart mitten in der Sitzung").
    static bool wantsQuietBoot(uint16_t storedSession, uint8_t storedChannel) {
        return storedSession != 0 && storedChannel >= kMinChannel && storedChannel <= kMaxChannel;
    }

    void begin(const Io& io, const uint8_t* mac, uint16_t number, uint16_t storedSession,
               uint8_t storedChannel, uint32_t now) {
        _io = io;
        memcpy(_mac, mac, 6);
        _number = (number >= 1 && number <= kMaxNumber) ? number : 0;
        if (wantsQuietBoot(storedSession, storedChannel)) {
            _session = storedSession;
            _sessionChannel = storedChannel;
            _tune(storedChannel);
            _state = State::RejoinListen;
            _deadline = now + kRejoinListenMs;
        }
    }

    // Whether this device may join on a buzz press: it has a buzz key and is
    // not the bridge.
    void setCanJoin(bool can) { _canJoin = can; }

    void onFrame(const uint8_t* d, size_t len, int8_t rssi, uint32_t now) {
        if (!quizFrameLenOk(d, len)) return;
        QuizHeader h;
        memcpy(&h, d, sizeof(h));
        if (h.proto != kProto || h.origin != 0) return;  // bridge frames only
        if (_state == State::Leaving) return;
        if (_state == State::Normal) {
            if (h.type == kMsgQuizInvite) _onInviteNormal(d, len, now);
            return;
        }
        uint16_t session;
        memcpy(&session, d + sizeof(QuizHeader), 2);
        if (session != _session) return;

        switch (h.type) {
            case kMsgQuizInvite: {
                QuizInviteMsg m;
                decodeFrame(d, len, kMsgQuizInvite, m);
                _followChannel(m.channel);
                _loginOpenUntil = now + kLoginOpenHoldMs;
                break;
            }
            case kMsgQuizRelease: {
                QuizReleaseMsg m;
                decodeFrame(d, len, kMsgQuizRelease, m);
                if (memcmp(m.mac, _mac, 6) == 0 || memcmp(m.mac, kBroadcastMac, 6) == 0) {
                    _leave(now);
                    return;
                }
                break;
            }
            case kMsgQuizChannel: {
                QuizChannelMsg m;
                decodeFrame(d, len, kMsgQuizChannel, m);
                if (m.channel >= kMinChannel && m.channel <= kMaxChannel) {
                    _pendingChannel = m.channel;
                    _switchAt = now + m.switchInMs;
                }
                break;
            }
            default:
                break;
        }
        _gatewayHeard(h.type, rssi, now);

        switch (_state) {
            case State::RejoinListen:
                _startJoin(/*pressed=*/false, /*rejoin=*/true, now);
                break;
            case State::Joining:
                if (h.type == kMsgQuizAdmit) _onAdmit(d, len, now);
                break;
            case State::Member:
                if (h.type == kMsgQuizAdmit)
                    _onAdmit(d, len, now);
                else if (h.type == kMsgQuizState)
                    _onState(d, len, now);
                break;
            default:
                break;
        }
    }

    // Raw key edge. ms is the moment of the edge (millis()). The power key
    // and keys without a role are not part of the game.
    void onKey(Key k, bool pressed, uint32_t ms, uint32_t now) {
        if ((uint8_t)k >= (uint8_t)Key::Power) return;
        _down[(uint8_t)k] = pressed;
        if (pressed) _downAt[(uint8_t)k] = ms;
        if (!pressed) return;
        if (_state == State::Normal) {
            if (k == Key::Buzz && inviteVisible(now)) _joinFromInvite(now);
        } else if (_state == State::Member) {
            _onMemberPress(k, ms, now);
        }
    }

    void tick(uint32_t now) {
        switch (_state) {
            case State::Normal:
                return;
            case State::Leaving:
                if ((int32_t)(now - _deadline) >= 0) {
                    _deadline = now + 0x7FFFFFFF;
                    if (_io.restart) _io.restart();
                }
                return;
            case State::RejoinListen:
                if ((int32_t)(now - _deadline) >= 0) _leave(now);
                return;
            case State::Joining:
                if (_comboHeld(now) || now - _joinStart >= kJoinTimeoutMs) {
                    _leave(now);
                    return;
                }
                _serviceChannelSwitch(now);
                if ((int32_t)(now - _nextJoinAt) >= 0) {
                    _sendStatus(_joinFlags);
                    _nextJoinAt = now + _rand(kJoinRepeatMinMs, kJoinRepeatMaxMs);
                }
                return;
            case State::Member:
                break;
        }
        if (_comboHeld(now) || now - _lastGatewayMs >= kAutoReleaseMs) {
            _leave(now);
            return;
        }
        _serviceChannelSwitch(now);
        _serviceSearch(now);
        _serviceSend(now);
        if ((int32_t)(now - _nextStatusAt) >= 0) {
            _sendStatus(0);
            _nextStatusAt = now + _statusInterval();
        }
    }

    // ── View for rendering and the bridge glue ──────────────────────────────
    State state() const { return _state; }
    bool inSession() const { return _state != State::Normal; }
    bool inviteVisible(uint32_t now) const {
        return _canJoin && _inviteValid && now - _inviteMs < kInviteVisibleMs;
    }
    uint16_t number() const { return _number; }
    uint16_t session() const { return _session; }
    uint8_t channel() const { return _channel; }
    const Round& round() const { return _round; }
    uint8_t value() const { return _val; }
    bool confirmed() const { return _confirmed; }
    uint32_t gatewayAgeMs(uint32_t now) const { return now - _lastGatewayMs; }
    uint32_t admittedAgeMs(uint32_t now) const { return now - _admittedAt; }
    bool searching() const { return _searching; }
    bool loginOpen(uint32_t now) const { return (int32_t)(_loginOpenUntil - now) > 0; }

    // Whether key k accepts input at moment ms (round format v0: free keys in
    // phase open, after the announced start).
    bool keyFree(Key k, uint32_t ms) const {
        if (!_round.known || _round.type == RoundType::Idle || _round.phase != Phase::Open)
            return false;
        if ((uint8_t)k >= kKeySlots || !(_round.keyMask & (1u << (uint8_t)k))) return false;
        return _round.startKnown && (int32_t)(ms - _round.startMs) >= 0;
    }

    // Before the announced race start: open, start known, start in the future.
    bool beforeStart(uint32_t now) const {
        return _round.known && _round.phase == Phase::Open && _round.startKnown &&
               (int32_t)(now - _round.startMs) < 0;
    }

    // Choice answer for a key: UP 1, RIGHT 2, DOWN 3, LEFT 4 (gateway protocol
    // v0, "Erlaubte Tasten"); 0 for other keys.
    static uint8_t choiceAnswer(Key k) {
        switch (k) {
            case Key::Up:
                return 1;
            case Key::Right:
                return 2;
            case Key::Down:
                return 3;
            case Key::Left:
                return 4;
            default:
                return 0;
        }
    }

   private:
    Io _io;
    uint8_t _mac[6] = {};
    State _state = State::Normal;
    bool _canJoin = false;
    uint16_t _number = 0;  // 0 = none
    uint16_t _msgId = 0;

    // Invite seen in normal operation.
    bool _inviteValid = false;
    uint32_t _inviteMs = 0;
    uint16_t _inviteSession = 0;
    uint8_t _inviteChannel = 0;

    // Session.
    uint16_t _session = 0;
    uint8_t _sessionChannel = 0;  // stored session channel
    uint8_t _channel = 0;         // tuned channel
    uint32_t _deadline = 0;
    uint32_t _joinStart = 0;
    uint32_t _nextJoinAt = 0;
    uint8_t _joinFlags = 0;
    uint32_t _admittedAt = 0;
    uint32_t _lastGatewayMs = 0;
    int8_t _lastRssi = 0;
    uint32_t _loginOpenUntil = 0;
    uint32_t _nextStatusAt = 0;
    uint8_t _pendingChannel = 0;
    uint32_t _switchAt = 0;
    bool _searching = false;
    uint32_t _searchNextAt = 0;

    // Round and own value.
    Round _round;
    uint8_t _val = kNoValue;
    uint8_t _vseq = 0;
    int32_t _t = 0;
    bool _confirmed = false;
    bool _sendArmed = false;
    uint32_t _nextSendAt = 0;
    uint32_t _retryMs = 0;

    // Key state for the leave combination.
    bool _down[11] = {};
    uint32_t _downAt[11] = {};

    uint32_t _rand(uint32_t lo, uint32_t hi) {
        return _io.randomRange ? _io.randomRange(lo, hi) : lo;
    }

    void _tune(uint8_t ch) {
        _channel = ch;
        if (_io.setChannel) _io.setChannel(ch);
    }

    void _persistSession() {
        if (_state == State::Member && _io.storeSession)
            _io.storeSession(_session, _sessionChannel);
    }

    void _onInviteNormal(const uint8_t* d, size_t len, uint32_t now) {
        QuizInviteMsg m;
        if (!decodeFrame(d, len, kMsgQuizInvite, m)) return;
        if (m.h.fleet != 0 && m.h.fleet != kDeviceFleet) return;
        if (m.session == 0 || m.channel < kMinChannel || m.channel > kMaxChannel) return;
        _inviteValid = true;
        _inviteMs = now;
        _inviteSession = m.session;
        _inviteChannel = m.channel;
    }

    void _joinFromInvite(uint32_t now) {
        if (_io.enterQuiet) _io.enterQuiet();
        _session = _inviteSession;
        _sessionChannel = _inviteChannel;
        _tune(_inviteChannel);
        _lastGatewayMs = now;
        _startJoin(/*pressed=*/true, /*rejoin=*/false, now);
    }

    void _startJoin(bool pressed, bool rejoin, uint32_t now) {
        _state = State::Joining;
        _joinStart = now;
        _joinFlags = kStatusJoin | (pressed ? kStatusPressed : 0) | (rejoin ? kStatusRejoin : 0);
        _sendStatus(_joinFlags);
        _nextJoinAt = now + _rand(kJoinRepeatMinMs, kJoinRepeatMaxMs);
    }

    void _followChannel(uint8_t ch) {
        if (ch < kMinChannel || ch > kMaxChannel) return;
        if (ch != _channel) _tune(ch);
        if (ch != _sessionChannel) {
            _sessionChannel = ch;
            _persistSession();
        }
    }

    void _gatewayHeard(uint8_t type, int8_t rssi, uint32_t now) {
        _lastGatewayMs = now;
        _lastRssi = rssi;
        if (_searching) {
            _searching = false;
            // An invite names the session channel itself (see _followChannel);
            // any other bridge frame is sent on it.
            if (type != kMsgQuizInvite) _followChannel(_channel);
        }
    }

    void _onAdmit(const uint8_t* d, size_t len, uint32_t now) {
        QuizAdmitMsg m;
        if (!decodeFrame(d, len, kMsgQuizAdmit, m)) return;
        if (memcmp(m.mac, _mac, 6) != 0) return;
        if (m.number < 1 || m.number > kMaxNumber) return;
        if (m.number != _number) {
            _number = m.number;
            if (_io.storeNumber) _io.storeNumber(_number);
        }
        if (_state == State::Joining) {
            _state = State::Member;
            _admittedAt = now;
            _round = Round{};
            _resetValue();
            _persistSession();
        }
        _sendStatus(0);
        _nextStatusAt = now + _statusInterval();
    }

    void _onState(const uint8_t* d, size_t len, uint32_t now) {
        QuizStateMsg m;
        if (!decodeFrame(d, len, kMsgQuizState, m)) return;
        if (m.roundType >= kRoundTypeCount || m.phase >= kPhaseCount) return;
        bool newRound = !_round.known || m.roundId != _round.id;
        if (newRound || seq16Newer(m.stateSeq, _round.seq)) {
            if (newRound) {
                _round = Round{};
                _resetValue();
            }
            _round.known = true;
            _round.id = m.roundId;
            _round.seq = m.stateSeq;
            _round.type = (RoundType)m.roundType;
            _round.phase = (Phase)m.phase;
            _round.keyMask = m.keyMask;
            memcpy(_round.keyColor, m.keyColor, sizeof(_round.keyColor));
            _round.min = m.scaleMin;
            _round.max = m.scaleMax;
            _round.redMax = m.redMax;
            _round.greenMin = m.greenMin;
            _round.winner = m.winner;
            _round.correct = m.correct;
        }
        if (m.roundId != _round.id) return;  // an older round's frame
        // Local round start = receive time - openOffsetMs; keep the smallest,
        // since every delay only makes it larger (gateway protocol v0).
        if (m.roundType != (uint8_t)RoundType::Idle && m.phase != (uint8_t)Phase::Announced) {
            uint32_t cand = now - (uint32_t)m.openOffsetMs;
            if (!_round.startKnown || (int32_t)(cand - _round.startMs) < 0) {
                _round.startMs = cand;
                _round.startKnown = true;
            }
        }
        // Echo: confirm, or resend if the bridge holds something else.
        if (_number == 0 || _val == kNoValue) return;
        if (_number < m.echoFirst || _number >= m.echoFirst + m.echoCount) return;
        uint8_t echo = m.echo[_number - m.echoFirst];
        if (echo == _val) {
            _confirmed = true;
            _sendArmed = false;
        } else if (_confirmed) {
            _confirmed = false;
            _sendArmed = true;
            _retryMs = 0;
            _nextSendAt = now + _rand(kRetryMinMs, kRetryMaxMs);
        }
    }

    void _resetValue() {
        _val = kNoValue;
        _vseq = 0;
        _t = 0;
        _confirmed = false;
        _sendArmed = false;
        _retryMs = 0;
    }

    void _onMemberPress(Key k, uint32_t ms, uint32_t now) {
        if (!keyFree(k, ms)) {
            // Round format v0, "Nummern": a member pressing the buzzer while
            // login is open reports itself again, marked as pressed.
            if (k == Key::Buzz && loginOpen(now)) _sendStatus(kStatusJoin | kStatusPressed);
            return;
        }
        switch (_round.type) {
            case RoundType::Race:
                if (k == Key::Buzz && _val == kNoValue) _setValue(1, ms, now, 0);
                break;
            case RoundType::Choice: {
                uint8_t a = choiceAnswer(k);
                if (a == 0) break;
                if (_val == kNoValue || (kChoiceChangeAllowed && a != _val))
                    _setValue(a, ms, now, 0);
                break;
            }
            case RoundType::Scale: {
                if ((k != Key::Up && k != Key::Down) || _round.max < _round.min) break;
                uint8_t steps = (uint8_t)(_round.max - _round.min + 1);
                uint8_t v;
                if (_val == kNoValue) {
                    // Proposal 2: the first tap sets the middle.
                    v = k == Key::Up ? (uint8_t)(_round.min + steps / 2)
                                     : (uint8_t)(_round.min + (steps - 1) / 2);
                } else if (k == Key::Up) {
                    v = _val < _round.max ? (uint8_t)(_val + 1) : _val;
                } else {
                    v = _val > _round.min ? (uint8_t)(_val - 1) : _val;
                }
                if (v != _val) _setValue(v, ms, now, kScaleSendDelayMs);
                break;
            }
            default:
                break;
        }
    }

    void _setValue(uint8_t v, uint32_t ms, uint32_t now, uint32_t delayMs) {
        _val = v;
        _vseq++;
        _t = (int32_t)(ms - _round.startMs);
        _confirmed = false;
        _sendArmed = true;
        _retryMs = 0;
        _nextSendAt = delayMs == 0 ? now : ms + delayMs;
        _serviceSend(now);
    }

    void _serviceSend(uint32_t now) {
        if (!_sendArmed || _confirmed || (int32_t)(now - _nextSendAt) < 0) return;
        QuizReportMsg m{};
        _fillHeader(m.h, kMsgQuizReport);
        m.session = _session;
        m.roundId = _round.id;
        m.valueSeq = _vseq;
        m.value = _val;
        m.pressOffsetMs = _t;
        m.battery = _health().battery;
        m.rssi = _lastRssi;
        _send(&m, sizeof(m));
        _retryMs = _retryMs == 0 ? _rand(kRetryMinMs, kRetryMaxMs)
                                 : (_retryMs * 2 > kRetryCapMs ? kRetryCapMs : _retryMs * 2);
        _nextSendAt = now + _retryMs;
    }

    void _serviceChannelSwitch(uint32_t now) {
        if (_pendingChannel == 0 || (int32_t)(now - _switchAt) < 0) return;
        uint8_t ch = _pendingChannel;
        _pendingChannel = 0;
        _followChannel(ch);
    }

    void _serviceSearch(uint32_t now) {
        if (!_searching) {
            if (now - _lastGatewayMs < kNoGatewaySearchMs) return;
            _searching = true;
            _searchNextAt = now;
        }
        if ((int32_t)(now - _searchNextAt) < 0) return;
        _tune((uint8_t)(_channel % kMaxChannel + 1));
        _searchNextAt = now + kSearchDwellMs;
    }

    bool _comboHeld(uint32_t now) const {
        const uint8_t b = (uint8_t)Key::Buzz, e = (uint8_t)Key::Enter;
        if (!_down[b] || !_down[e]) return false;
        uint32_t since = (int32_t)(_downAt[b] - _downAt[e]) > 0 ? _downAt[b] : _downAt[e];
        return (int32_t)(now - since) >= (int32_t)kLeaveComboMs;
    }

    void _leave(uint32_t now) {
        if (_io.storeSession) _io.storeSession(0, 0);
        _state = State::Leaving;
        _deadline = now + kLeaveRestartDelayMs;
    }

    uint32_t _statusInterval() {
        return _rand(kStatusPeriodMs - kStatusJitterMs, kStatusPeriodMs + kStatusJitterMs);
    }

    Health _health() { return _io.health ? _io.health() : Health{}; }

    void _fillHeader(QuizHeader& h, uint8_t type) {
        h.type = type;
        h.proto = kProto;
        h.fleet = kDeviceFleet;
        h.origin = _number == 0 ? kNoNumber : _number;
        h.msgId = _msgId++;
        h.ttl = 0;
        h.hops = 0;
        h.dist = 1;
        h.flags = 0;
    }

    void _sendStatus(uint8_t flags) {
        QuizStatusMsg m{};
        _fillHeader(m.h, kMsgQuizStatus);
        m.session = _session;
        memcpy(m.mac, _mac, 6);
        m.fwBuild = _io.fwBuild ? _io.fwBuild() : 0;
        Health hl = _health();
        m.battery = hl.battery;
        m.flags = (uint8_t)(flags | (hl.charging ? kStatusCharging : 0));
        m.rssi = _lastRssi;
        m.lastRoundId = _round.known ? _round.id : 0;
        m.lastStateSeq = _round.known ? _round.seq : 0;
        _send(&m, sizeof(m));
    }

    void _send(const void* m, size_t len) {
        if (_io.send) _io.send((const uint8_t*)m, len);
    }
};

}  // namespace game
