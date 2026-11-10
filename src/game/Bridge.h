#pragma once
#include <stdint.h>
#include <string.h>

#include <functional>
#include <new>

#include "GameConfig.h"
#include "GameFrames.h"
#include "GameTypes.h"
#include "Lage.h"

namespace game {

// Bridge (gateway) role, radio side: repeats the session and round state the
// console sets, invites while login is open, admits and releases devices,
// keeps the latest value per device for the echo, and hands everything to
// the USB link through Listener and the device table (gateway protocol v0).
// In a move round it passes each key press on once, through a queue the
// link drains (firmware spec "Lage v0").
//
// Plain C++: radio and channel come in through Io, so this runs on the host.
class Bridge {
   public:
    struct Io {
        std::function<void(const uint8_t*, size_t)> send;
        std::function<void(uint8_t)> setChannel;
        std::function<void()> enterQuiet;  // first session: radio belongs to the game
    };

    struct JoinInfo {
        uint8_t mac[6];
        uint16_t n;  // 0 = no stored number
        uint32_t fw;
        uint8_t proto;
        uint16_t fleet;
        uint8_t battery;
        bool charging;
        int8_t rssi;
        bool pressed;
        bool rejoin;
        bool named;    // plays with the animal name (kStatusNamed)
        uint8_t lage;  // Lage, 0 = unknown
    };

    struct Listener {
        virtual ~Listener() = default;
        virtual void onJoin(const JoinInfo& j) = 0;
        virtual void onAdmitted(const ReqId& id, const uint8_t* mac, uint16_t n) = 0;
        virtual void onAdmitTimeout(const ReqId& id, const uint8_t* mac, uint16_t n) = 0;
        virtual void onSessionEnded(const ReqId& id) = 0;
    };

    // Per device number, the latest value of the running round and the last status.
    struct Device {
        uint8_t mac[6];
        bool macKnown;
        bool hasValue;
        bool vseqReset;  // accept the next report regardless of valueSeq
        bool late;
        uint8_t value;
        uint8_t vseq;
        int8_t valueRssi;
        uint8_t valueBattery;
        uint16_t arr;
        int32_t t;
        bool hasStatus;
        bool charging;
        uint8_t proto;
        uint8_t battery;
        int8_t rssi;
        uint32_t fw;
        uint16_t lastRound;
        uint16_t lastSeq;
        uint8_t lage;       // from the last status, 0 = unknown
        bool named;         // from the last status (kStatusNamed)
        uint8_t valueLage;  // the value's input orientation, 0 = unknown
        uint8_t keySeq;     // move round: last kseq passed on, kNoKeySeq = none
        bool answerTaken;   // race: the value is the answering device's answer
        bool dirtyValue;
        bool dirtyDevice;
    };

    // A key press of a move round waiting for its USB line.
    struct KeyEvent {
        uint16_t n;
        uint16_t round;
        uint8_t kseq;
        uint8_t key;   // Key
        uint8_t lage;  // Lage, 0 = unknown: key is the physical one
    };

    struct RoundCmd {
        uint16_t round = 0;
        RoundType type = RoundType::Idle;
        Phase phase = Phase::Closed;
        uint16_t keyMask = 0;
        uint8_t keyColor[5] = {};
        uint8_t min = 0, max = 0, redMax = 0, greenMin = 0;
        bool hasOpenIn = false;
        uint16_t openInMs = 0;
        uint16_t winner = 0;
        uint8_t correct = 0;
        Hold hold = Hold::Frei;
        bool firstOnly = false;    // only each device's first answer counts
        bool winnerWrong = false;  // the quizmaster rated the winner's answer wrong
        bool rainbow = false;      // scale of 24 steps shown as the rainbow
    };

    struct StateInfo {
        uint16_t round;
        uint16_t seq;
        RoundType type;
        Phase phase;
        bool sinceOpenValid;
        int32_t sinceOpenMs;
        bool closedValid;
        int32_t closedAtMs;
    };

    enum class Err : uint8_t {
        None,
        NoSession,
        SessionActive,
        RoundFixed,
        BadPhase,
        NoMemory,
        Full
    };

    ~Bridge() {
        delete[] _devs;
        delete[] _admits;
        delete[] _keys;
    }

    void begin(const Io& io, Listener* listener) {
        _io = io;
        _listener = listener;
    }

    // ── Commands (fields already validated by the link) ──────────────────────
    Err session(uint16_t id, uint8_t channel, uint16_t fleet, bool login, uint32_t now) {
        if (_ending) return Err::NoSession;
        if (_active && id != _session) return Err::SessionActive;
        if (!_active) {
            if (!_devs) _devs = new (std::nothrow) Device[kMaxNumber + 1]();
            if (!_admits) _admits = new (std::nothrow) PendingAdmit[kMaxPendingAdmits]();
            if (!_keys) _keys = new (std::nothrow) KeyEvent[kKeyQueueBridge]();
            if (!_devs || !_admits || !_keys) return Err::NoMemory;
            if (!_quiet) {
                _quiet = true;
                if (_io.enterQuiet) _io.enterQuiet();
            }
            _clearTable();
            _keyHead = 0;
            _keyCount = 0;
            _active = true;
            _session = id;
            _channel = channel;
            _fleet = fleet;
            _login = login;
            _pendingChannel = 0;
            _admitCount = 0;
            _releaseCount = 0;
            _offChannel = false;
            _tune(channel);
            _round = RoundModel{};
            _markStateChanged(now);
            _nextInviteAt = now;
            _nextHopAt = now + kInviteHopPeriodMs;
            _hopIdx = channel;
            return Err::None;
        }
        _fleet = fleet;
        if (login && !_login) {
            _nextInviteAt = now;
            _nextHopAt = now + kInviteHopPeriodMs;
        }
        _login = login;
        if (channel != _channel && channel != _pendingChannel) {
            _pendingChannel = channel;
            _announceUntil = now + kChannelAnnounceMs;
            _nextAnnounceAt = now;
        } else if (channel == _channel) {
            _pendingChannel = 0;
        }
        return Err::None;
    }

    Err end(const ReqId& id, uint32_t now) {
        if (!_active || _ending) return Err::NoSession;
        _ending = true;
        _endId = id;
        _endUntil = now + kEndReleaseMs;
        _nextEndReleaseAt = now;
        _login = false;
        _pendingChannel = 0;
        return Err::None;
    }

    Err admit(const uint8_t* mac, uint16_t n, const ReqId& id, uint32_t now) {
        if (!_active || _ending) return Err::NoSession;
        PendingAdmit* slot = nullptr;
        for (uint16_t i = 0; i < _admitCount; i++)
            if (memcmp(_admits[i].mac, mac, 6) == 0) slot = &_admits[i];
        if (!slot) {
            if (_admitCount >= kMaxPendingAdmits) return Err::Full;
            slot = &_admits[_admitCount++];
        }
        memcpy(slot->mac, mac, 6);
        slot->n = n;
        slot->id = id;
        slot->startedAt = now;
        slot->nextAt = now;
        _serviceAdmits(now);
        return Err::None;
    }

    Err release(const uint8_t* mac, uint32_t now) {
        if (!_active || _ending) return Err::NoSession;
        PendingRelease* slot = nullptr;
        for (uint8_t i = 0; i < _releaseCount; i++)
            if (memcmp(_releases[i].mac, mac, 6) == 0) slot = &_releases[i];
        if (!slot && _releaseCount < kMaxPendingReleases) slot = &_releases[_releaseCount++];
        if (!slot) {  // full: reuse the one closest to its end
            slot = &_releases[0];
            for (uint8_t i = 1; i < _releaseCount; i++)
                if ((int32_t)(_releases[i].until - slot->until) < 0) slot = &_releases[i];
        }
        memcpy(slot->mac, mac, 6);
        slot->until = now + kReleaseOneMs;
        slot->nextAt = now;
        for (uint16_t i = 0; i < _admitCount; i++)
            if (memcmp(_admits[i].mac, mac, 6) == 0) _removeAdmit(i--);
        for (uint16_t n = 1; n <= _maxNumber; n++)
            if (_devs[n].macKnown && memcmp(_devs[n].mac, mac, 6) == 0) _clearDevice(n);
        _serviceReleases(now);
        return Err::None;
    }

    bool isNewRound(uint16_t roundId) const { return _active && roundId != _round.id; }

    Err round(const RoundCmd& c, uint32_t now, bool& changed) {
        changed = false;
        if (!_active || _ending) return Err::NoSession;
        RoundModel& r = _round;
        if (c.round != r.id) {
            r = RoundModel{};
            r.id = c.round;
            r.seq = 1;
            r.type = c.type;
            r.phase = c.type == RoundType::Idle ? Phase::Closed : c.phase;
            r.keyMask = c.keyMask;
            memcpy(r.keyColor, c.keyColor, sizeof(r.keyColor));
            r.min = c.min;
            r.max = c.max;
            r.redMax = c.redMax;
            r.greenMin = c.greenMin;
            r.winner = c.winner;
            r.correct = c.correct;
            r.hold = c.hold;
            r.firstOnly = c.firstOnly;
            r.winnerWrong = c.winnerWrong;
            r.rainbow = c.rainbow;
            if (r.type != RoundType::Idle) _enterPhase(r.phase, c, now);
            for (uint16_t n = 1; n <= _maxNumber; n++) _clearValue(n);
            _arrivals = 0;
            changed = true;
            _markStateChanged(now);
            return Err::None;
        }
        if (c.type != r.type || c.keyMask != r.keyMask ||
            memcmp(c.keyColor, r.keyColor, sizeof(r.keyColor)) != 0 || c.min != r.min ||
            c.max != r.max || c.redMax != r.redMax || c.greenMin != r.greenMin ||
            c.hold != r.hold || c.firstOnly != r.firstOnly || c.rainbow != r.rainbow)
            return Err::RoundFixed;
        Phase phase = r.type == RoundType::Idle ? Phase::Closed : c.phase;
        if ((uint8_t)phase < (uint8_t)r.phase) return Err::BadPhase;
        if (phase == r.phase && c.winner == r.winner && c.correct == r.correct &&
            c.winnerWrong == r.winnerWrong)
            return Err::None;
        if (phase != r.phase) _enterPhase(phase, c, now);
        r.phase = phase;
        r.winner = c.winner;
        r.correct = c.correct;
        r.winnerWrong = c.winnerWrong;
        r.seq++;
        changed = true;
        _markStateChanged(now);
        return Err::None;
    }

    // The race device that answers now (n 0 = none) and its answer time.
    Err answering(uint16_t n, uint16_t ms, uint32_t now) {
        if (!_active || _ending) return Err::NoSession;
        RoundModel& r = _round;
        if (r.type != RoundType::Race) return Err::BadPhase;
        if (n == 0) ms = 0;
        if (n == r.answering && ms == r.answerMs) return Err::None;
        r.answering = n;
        r.answerMs = ms;
        r.answerAt = now;
        r.seq++;
        _markStateChanged(now);
        return Err::None;
    }

    // A countdown for every device without an answer (ms 0 = none): an open
    // choice or scale.
    Err countdown(uint16_t ms, uint32_t now) {
        if (!_active || _ending) return Err::NoSession;
        RoundModel& r = _round;
        if ((r.type != RoundType::Choice && r.type != RoundType::Scale) || r.phase != Phase::Open)
            return Err::BadPhase;
        if (r.answering == 0 && ms == r.answerMs) return Err::None;
        r.answering = 0;
        r.answerMs = ms;
        r.answerAt = now;
        r.seq++;
        _markStateChanged(now);
        return Err::None;
    }

    // ── Radio ────────────────────────────────────────────────────────────────
    void onFrame(const uint8_t* d, size_t len, int8_t rssi, uint32_t now) {
        if (!quizFrameLenOk(d, len)) return;
        _rxFrames++;
        if (!_active || _ending) return;
        QuizHeader h;
        memcpy(&h, d, sizeof(h));
        if (h.origin == 0) return;  // another bridge
        uint16_t session;
        memcpy(&session, d + sizeof(QuizHeader), 2);
        if (session != _session) return;
        if (h.type == kMsgQuizReport && h.proto == kProto) {
            QuizReportMsg m;
            decodeFrame(d, len, kMsgQuizReport, m);
            _onReport(m, rssi);
        } else if (h.type == kMsgQuizStatus) {
            QuizStatusMsg m;
            decodeFrame(d, len, kMsgQuizStatus, m);
            _onStatus(m, rssi);
        } else if (h.type == kMsgQuizKeys && h.proto == kProto) {
            QuizKeysMsg m;
            decodeFrame(d, len, kMsgQuizKeys, m);
            _onKeys(m);
        }
        (void)now;
    }

    void tick(uint32_t now) {
        if (!_active) return;
        if (_offChannel) {
            if ((int32_t)(now - _hopReturnAt) < 0) return;
            _offChannel = false;
            _tune(_channel);
        }
        if (_ending) {
            if ((int32_t)(now - _endUntil) >= 0) {
                _active = false;
                _ending = false;
                _clearTable();
                _round = RoundModel{};
                if (_listener) _listener->onSessionEnded(_endId);
                return;
            }
            if ((int32_t)(now - _nextEndReleaseAt) >= 0) {
                _sendRelease(kBroadcastMac);
                _nextEndReleaseAt = now + kBurstRepeatMs;
            }
            return;
        }
        if (_pendingChannel) {
            if ((int32_t)(now - _announceUntil) >= 0) {
                _channel = _pendingChannel;
                _pendingChannel = 0;
                _tune(_channel);
                _markStateChanged(now);
            } else if ((int32_t)(now - _nextAnnounceAt) >= 0) {
                QuizChannelMsg m{};
                _fillHeader(m.h, kMsgQuizChannel);
                m.session = _session;
                m.channel = _pendingChannel;
                m.switchInMs = (uint16_t)(_announceUntil - now);
                _send(&m, sizeof(m));
                _nextAnnounceAt = now + kBurstRepeatMs;
            }
        }
        if ((int32_t)(now - _nextStateAt) >= 0) _sendState(now);
        _serviceAdmits(now);
        _serviceReleases(now);
        if (_login && !_pendingChannel) {
            if ((int32_t)(now - _nextInviteAt) >= 0) {
                _sendInvite();
                _nextInviteAt = now + kInviteSessionPeriodMs;
            }
            if ((int32_t)(now - _nextHopAt) >= 0) {
                // Proposal 13: visit the next foreign channel, invite twice, return.
                _hopIdx = (uint8_t)(_hopIdx % kMaxChannel + 1);
                if (_hopIdx == _channel) _hopIdx = (uint8_t)(_hopIdx % kMaxChannel + 1);
                _tune(_hopIdx);
                for (uint8_t i = 0; i < kInvitesPerHop; i++) _sendInvite();
                _offChannel = true;
                _hopReturnAt = now + kInviteHopDwellMs;
                _nextHopAt = now + kInviteHopPeriodMs;
            }
        }
    }

    // ── State for the link ───────────────────────────────────────────────────
    bool active() const { return _active; }
    bool ending() const { return _ending; }
    bool quiet() const { return _quiet; }
    uint16_t sessionId() const { return _session; }
    uint8_t channel() const { return _pendingChannel ? _pendingChannel : _channel; }
    uint16_t fleet() const { return _fleet; }
    bool login() const { return _login; }
    bool offChannel() const { return _offChannel; }
    uint16_t maxNumber() const { return _maxNumber; }
    uint32_t rxFrames() const { return _rxFrames; }
    uint32_t txFrames() const { return _txFrames; }
    uint32_t dirtyCount() const { return _dirtyCount; }
    uint32_t dirtyValueCount() const { return _dirtyValues; }
    const Device* device(uint16_t n) const {
        return (_devs && n >= 1 && n <= kMaxNumber) ? &_devs[n] : nullptr;
    }

    StateInfo stateInfo(uint32_t now) const {
        StateInfo s{};
        s.round = _round.id;
        s.seq = _round.seq;
        s.type = _round.type;
        s.phase = _round.phase;
        s.sinceOpenValid = _round.opened;
        s.sinceOpenMs = _round.opened ? (int32_t)(now - _round.openAt) : 0;
        s.closedValid = _round.closedValid;
        s.closedAtMs = _round.closedAtMs;
        return s;
    }

    // Key presses waiting for their USB line, oldest first.
    uint8_t keyCount() const { return _keyCount; }
    const KeyEvent& keyFront() const { return _keys[_keyHead]; }
    void popKey() {
        _keyHead = (uint8_t)((_keyHead + 1) % kKeyQueueBridge);
        _keyCount--;
    }

    void clearDirtyValue(uint16_t n) {
        if (_devs[n].dirtyValue) {
            _devs[n].dirtyValue = false;
            _dirtyValues--;
            _dirtyCount--;
        }
    }
    void clearDirtyDevice(uint16_t n) {
        if (_devs[n].dirtyDevice) {
            _devs[n].dirtyDevice = false;
            _dirtyCount--;
        }
    }

    // Link (re)connected: report every participant and every value again.
    void markAllDirty() {
        if (!_devs) return;
        for (uint16_t n = 1; n <= _maxNumber; n++) {
            if (_devs[n].macKnown && _devs[n].hasStatus) _setDirtyDevice(n);
            if (_devs[n].hasValue) _setDirtyValue(n);
        }
    }

   private:
    struct RoundModel {
        uint16_t id = 0;
        uint16_t seq = 1;
        RoundType type = RoundType::Idle;
        Phase phase = Phase::Closed;
        uint16_t keyMask = 0;
        uint8_t keyColor[5] = {};
        uint8_t min = 0, max = 0, redMax = 0, greenMin = 0;
        uint16_t winner = 0;
        uint8_t correct = 0;
        Hold hold = Hold::Frei;
        bool firstOnly = false;
        bool winnerWrong = false;
        bool rainbow = false;
        uint16_t answering = 0;  // race: the number answering now
        uint16_t answerMs = 0;
        uint32_t answerAt = 0;  // bridge time its answer time began
        bool opened = false;
        uint32_t openAt = 0;  // bridge time of the round start (open + openInMs)
        bool closedValid = false;
        int32_t closedAtMs = 0;
    };
    struct PendingAdmit {
        uint8_t mac[6];
        uint16_t n;
        ReqId id;
        uint32_t startedAt;
        uint32_t nextAt;
    };
    struct PendingRelease {
        uint8_t mac[6];
        uint32_t until;
        uint32_t nextAt;
    };

    Io _io;
    Listener* _listener = nullptr;
    Device* _devs = nullptr;  // index = number, 1..kMaxNumber
    bool _quiet = false;
    bool _active = false;
    bool _ending = false;
    uint16_t _session = 0;
    uint8_t _channel = 0;
    uint16_t _fleet = 0;
    bool _login = false;
    uint16_t _msgId = 0;
    uint16_t _maxNumber = 0;
    uint16_t _arrivals = 0;
    uint32_t _rxFrames = 0;
    uint32_t _txFrames = 0;
    uint32_t _dirtyCount = 0;
    uint32_t _dirtyValues = 0;
    RoundModel _round;

    // State broadcast schedule.
    uint32_t _nextStateAt = 0;
    uint32_t _lastStateAt = 0;
    bool _stateSentOnce = false;
    uint8_t _burstStep = 0;
    uint32_t _burstAnchor = 0;

    // Invites and channel hops.
    uint32_t _nextInviteAt = 0;
    uint32_t _nextHopAt = 0;
    uint8_t _hopIdx = 0;
    bool _offChannel = false;
    uint32_t _hopReturnAt = 0;

    // Channel change announcement.
    uint8_t _pendingChannel = 0;
    uint32_t _announceUntil = 0;
    uint32_t _nextAnnounceAt = 0;

    // Session end.
    ReqId _endId;
    uint32_t _endUntil = 0;
    uint32_t _nextEndReleaseAt = 0;

    PendingAdmit* _admits = nullptr;  // kMaxPendingAdmits, allocated with the device table
    uint16_t _admitCount = 0;
    KeyEvent* _keys = nullptr;  // kKeyQueueBridge, allocated with the device table
    uint8_t _keyHead = 0;
    uint8_t _keyCount = 0;
    PendingRelease _releases[kMaxPendingReleases];
    uint8_t _releaseCount = 0;

    void _tune(uint8_t ch) {
        if (_io.setChannel) _io.setChannel(ch);
    }

    void _enterPhase(Phase phase, const RoundCmd& c, uint32_t now) {
        RoundModel& r = _round;
        if (phase == Phase::Open && !r.opened) {
            // The clock "since round start" starts at open + openInMs, once.
            r.opened = true;
            r.openAt = now + (c.hasOpenIn ? c.openInMs : 0);
        }
        if (phase == Phase::Closed && r.opened && !r.closedValid) {
            r.closedValid = true;
            r.closedAtMs = (int32_t)(now - r.openAt);
        }
    }

    void _markStateChanged(uint32_t now) {
        _burstStep = 0;
        _nextStateAt = now;
        if (_stateSentOnce && (int32_t)(_lastStateAt + kStateMinGapMs - now) > 0)
            _nextStateAt = _lastStateAt + kStateMinGapMs;
    }

    void _sendState(uint32_t now) {
        QuizStateMsg m{};
        _fillHeader(m.h, kMsgQuizState);
        m.h.flags = (uint8_t)((_round.firstOnly ? kStateFirstOnly : 0) |
                              (_round.winnerWrong ? kStateWinnerWrong : 0) |
                              (_round.rainbow ? kStateRainbow : 0));
        m.session = _session;
        m.roundId = _round.id;
        m.stateSeq = _round.seq;
        m.roundType = (uint8_t)_round.type;
        m.phase = (uint8_t)_round.phase;
        m.keyMask = _round.keyMask;
        memcpy(m.keyColor, _round.keyColor, sizeof(m.keyColor));
        m.scaleMin = _round.min;
        m.scaleMax = _round.max;
        m.redMax = _round.redMax;
        m.greenMin = _round.greenMin;
        m.winner = _round.winner;
        m.correct = _round.correct;
        m.hold = (uint8_t)_round.hold;
        m.turn = _round.answering;
        m.turnMs = _round.answerMs;
        bool move = _round.type == RoundType::Move;
        uint16_t first = 1;
        do {
            uint16_t remaining = _maxNumber >= first ? (uint16_t)(_maxNumber - first + 1) : 0;
            uint8_t count = remaining > kEchoPageSize ? kEchoPageSize : (uint8_t)remaining;
            m.echoFirst = first;
            m.echoCount = count;
            for (uint8_t i = 0; i < count; i++) {
                const Device& dv = _devs[first + i];
                if (move)
                    m.echo[i] = dv.keySeq;
                else
                    m.echo[i] = !dv.hasValue     ? kNoValue
                                : dv.answerTaken ? (uint8_t)(kEchoAnswer | dv.value)
                                                 : dv.value;
            }
            // Written as late as possible before sending (gateway protocol v0).
            m.openOffsetMs = _round.opened ? (int32_t)(now - _round.openAt) : 0;
            uint32_t used = now - _round.answerAt;
            m.turnLeftMs = used < _round.answerMs ? (uint16_t)(_round.answerMs - used) : 0;
            _send(&m, stateFrameLen(m));
            first = (uint16_t)(first + kEchoPageSize);
        } while (first <= _maxNumber);

        _lastStateAt = now;
        _stateSentOnce = true;
        uint32_t period = (_round.type != RoundType::Idle && _round.phase == Phase::Open)
                              ? kStatePeriodOpenMs
                              : kStatePeriodIdleMs;
        switch (_burstStep) {
            case 0:
                _burstAnchor = now;
                _nextStateAt = now + kStateRepeat1Ms;
                _burstStep = 1;
                break;
            case 1:
                _nextStateAt = _burstAnchor + kStateRepeat2Ms;
                _burstStep = 2;
                break;
            default:
                _nextStateAt = now + period;
                _burstStep = 3;
                break;
        }
    }

    void _sendInvite() {
        QuizInviteMsg m{};
        _fillHeader(m.h, kMsgQuizInvite);
        m.session = _session;
        m.channel = _channel;
        _send(&m, sizeof(m));
    }

    void _sendRelease(const uint8_t* mac) {
        QuizReleaseMsg m{};
        _fillHeader(m.h, kMsgQuizRelease);
        m.session = _session;
        memcpy(m.mac, mac, 6);
        _send(&m, sizeof(m));
    }

    void _serviceAdmits(uint32_t now) {
        for (uint16_t i = 0; i < _admitCount; i++) {
            PendingAdmit& a = _admits[i];
            if (now - a.startedAt >= kAdmitTimeoutMs) {
                PendingAdmit gone = a;
                _removeAdmit(i--);
                if (_listener) _listener->onAdmitTimeout(gone.id, gone.mac, gone.n);
                continue;
            }
            if (_offChannel || (int32_t)(now - a.nextAt) < 0) continue;
            QuizAdmitMsg m{};
            _fillHeader(m.h, kMsgQuizAdmit);
            m.session = _session;
            memcpy(m.mac, a.mac, 6);
            m.number = a.n;
            _send(&m, sizeof(m));
            a.nextAt = now + kAdmitRepeatMs;
        }
    }

    void _serviceReleases(uint32_t now) {
        for (uint8_t i = 0; i < _releaseCount; i++) {
            PendingRelease& r = _releases[i];
            if ((int32_t)(now - r.until) >= 0) {
                _releases[i--] = _releases[--_releaseCount];
                continue;
            }
            if (_offChannel || (int32_t)(now - r.nextAt) < 0) continue;
            _sendRelease(r.mac);
            r.nextAt = now + kBurstRepeatMs;
        }
    }

    void _removeAdmit(uint16_t i) { _admits[i] = _admits[--_admitCount]; }

    void _onReport(const QuizReportMsg& m, int8_t rssi) {
        uint16_t n = m.h.origin;
        if (n < 1 || n > kMaxNumber) return;
        if (m.roundId != _round.id || _round.type == RoundType::Idle ||
            _round.type == RoundType::Move)
            return;
        _noteNumber(n);
        Device& dv = _devs[n];
        // The answering race device's answer key (1-5): its first answer,
        // late after its answer time.
        if (_round.type == RoundType::Race && n == _round.answering && dv.hasValue) {
            if (dv.answerTaken || m.value < 1 || m.value > 5) return;
            if (!dv.vseqReset && !seq8Newer(m.valueSeq, dv.vseq)) return;
            dv.answerTaken = true;
            dv.vseqReset = false;
            dv.value = m.value;
            dv.vseq = m.valueSeq;
            dv.t = m.pressOffsetMs;
            dv.late = _round.opened && m.pressOffsetMs > (int32_t)(_round.answerAt +
                                                                   _round.answerMs - _round.openAt);
            dv.valueRssi = rssi;
            dv.valueBattery = m.battery;
            dv.valueLage = m.lage < kLageCount ? m.lage : 0;
            _setDirtyValue(n);
            return;
        }
        // firstOnly: the first value stays, with its time.
        if (_round.firstOnly && dv.hasValue) return;
        if (dv.hasValue && !dv.vseqReset && !seq8Newer(m.valueSeq, dv.vseq)) return;
        if (!dv.hasValue) dv.arr = ++_arrivals;
        dv.hasValue = true;
        dv.vseqReset = false;
        dv.value = m.value;
        dv.vseq = m.valueSeq;
        dv.t = m.pressOffsetMs;
        // Late: the input lay after the moment of closing (gateway protocol v0).
        dv.late = _round.closedValid && m.pressOffsetMs > _round.closedAtMs;
        dv.valueRssi = rssi;
        dv.valueBattery = m.battery;
        dv.valueLage = m.lage < kLageCount ? m.lage : 0;
        _setDirtyValue(n);
    }

    // Passes on each press newer than the last one passed on, in order, while
    // the queue has room; the rest come again with the device's next repeat.
    void _onKeys(const QuizKeysMsg& m) {
        uint16_t n = m.h.origin;
        if (n < 1 || n > kMaxNumber) return;
        if (m.roundId != _round.id || _round.type != RoundType::Move) return;
        for (uint8_t i = 0; i < m.count; i++)
            if (m.keys[i].kseq == kNoKeySeq || !_isDirection((Key)m.keys[i].key) ||
                m.keys[i].lage >= kLageCount)
                return;
        _noteNumber(n);
        Device& dv = _devs[n];
        for (uint8_t i = 0; i < m.count; i++) {
            const QuizKey& p = m.keys[i];
            if (dv.keySeq != kNoKeySeq && !seq8Newer(p.kseq, dv.keySeq)) continue;
            if (_keyCount >= kKeyQueueBridge) return;
            _keys[(_keyHead + _keyCount) % kKeyQueueBridge] =
                KeyEvent{n, _round.id, p.kseq, p.key, p.lage};
            _keyCount++;
            dv.keySeq = p.kseq;
        }
    }

    static bool _isDirection(Key k) {
        return k == Key::Up || k == Key::Right || k == Key::Down || k == Key::Left;
    }

    void _onStatus(const QuizStatusMsg& m, int8_t rssi) {
        uint16_t n = m.h.origin;
        bool hasNumber = n >= 1 && n <= kMaxNumber;
        uint8_t lage = m.lage < kLageCount ? m.lage : 0;
        if (m.flags & kStatusJoin) {
            if (hasNumber && n <= _maxNumber && _devs[n].macKnown &&
                memcmp(_devs[n].mac, m.mac, 6) == 0) {
                // A device rejoining after a reboot restarts its valueSeq.
                if (m.flags & kStatusRejoin) _devs[n].vseqReset = true;
                // A joining device (it holds no round) restarts its press
                // counter; a member's own report during login does not.
                if (m.lastRoundId != _round.id) _devs[n].keySeq = kNoKeySeq;
            }
            if (!_listener) return;
            JoinInfo j{};
            memcpy(j.mac, m.mac, 6);
            j.n = hasNumber ? n : 0;
            j.fw = m.fwBuild;
            j.proto = m.h.proto;
            j.fleet = m.h.fleet;
            j.battery = m.battery;
            j.charging = (m.flags & kStatusCharging) != 0;
            j.rssi = rssi;
            j.pressed = (m.flags & kStatusPressed) != 0;
            j.rejoin = (m.flags & kStatusRejoin) != 0;
            j.named = (m.flags & kStatusNamed) != 0;
            j.lage = lage;
            _listener->onJoin(j);
            return;
        }
        if (!hasNumber) return;
        _noteNumber(n);
        // The MAC owns only this number now (renumbered or replaced device).
        for (uint16_t k = 1; k <= _maxNumber; k++)
            if (k != n && _devs[k].macKnown && memcmp(_devs[k].mac, m.mac, 6) == 0) _clearDevice(k);
        Device& dv = _devs[n];
        if (dv.macKnown && memcmp(dv.mac, m.mac, 6) != 0) _clearDevice(n);  // another device
        memcpy(dv.mac, m.mac, 6);
        dv.macKnown = true;
        dv.hasStatus = true;
        dv.fw = m.fwBuild;
        dv.proto = m.h.proto;
        dv.battery = m.battery;
        dv.charging = (m.flags & kStatusCharging) != 0;
        dv.rssi = rssi;
        dv.lastRound = m.lastRoundId;
        dv.lastSeq = m.lastStateSeq;
        dv.lage = lage;
        dv.named = (m.flags & kStatusNamed) != 0;
        _setDirtyDevice(n);
        for (uint16_t i = 0; i < _admitCount; i++) {
            PendingAdmit& a = _admits[i];
            if (memcmp(a.mac, m.mac, 6) != 0 || a.n != n) continue;
            PendingAdmit done = a;
            _removeAdmit(i);
            if (_listener) _listener->onAdmitted(done.id, done.mac, done.n);
            break;
        }
    }

    void _noteNumber(uint16_t n) {
        if (n <= _maxNumber) return;
        for (uint16_t k = (uint16_t)(_maxNumber + 1); k <= n; k++) _devs[k] = Device{};
        _devs[n].value = kNoValue;
        _maxNumber = n;
    }

    void _setDirtyValue(uint16_t n) {
        if (_devs[n].dirtyValue) return;
        _devs[n].dirtyValue = true;
        _dirtyValues++;
        _dirtyCount++;
    }
    void _setDirtyDevice(uint16_t n) {
        if (_devs[n].dirtyDevice) return;
        _devs[n].dirtyDevice = true;
        _dirtyCount++;
    }

    void _clearValue(uint16_t n) {
        clearDirtyValue(n);
        Device& dv = _devs[n];
        dv.hasValue = false;
        dv.vseqReset = false;
        dv.late = false;
        dv.value = kNoValue;
        dv.vseq = 0;
        dv.arr = 0;
        dv.t = 0;
        dv.valueLage = 0;
        dv.keySeq = kNoKeySeq;
        dv.answerTaken = false;
    }

    void _clearDevice(uint16_t n) {
        _clearValue(n);
        clearDirtyDevice(n);
        _devs[n] = Device{};
        _devs[n].value = kNoValue;
    }

    void _clearTable() {
        _maxNumber = 0;
        _dirtyCount = 0;
        _dirtyValues = 0;
        _arrivals = 0;
    }

    void _fillHeader(QuizHeader& h, uint8_t type) {
        h.type = type;
        h.proto = kProto;
        h.fleet = _fleet;
        h.origin = 0;
        h.msgId = 0;  // stamped per transmission in _send
        h.ttl = 0;
        h.hops = 0;
        h.dist = 0;
        h.flags = 0;
    }

    void _send(void* m, size_t len) {
        ((QuizHeader*)m)->msgId = _msgId++;
        _txFrames++;
        if (_io.send) _io.send((const uint8_t*)m, len);
    }
};

}  // namespace game
