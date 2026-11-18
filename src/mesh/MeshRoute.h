#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <functional>

#include "../game/GameFrames.h"

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#endif

// Multi-hop forwarding for the game session (funk-konzept, "Nachbarn und
// Baum", "Weiterleiten", "Sendeleistung"). Plain C++: the radio comes in
// through Io, so this runs on the host.
//
// - Every node sends a short beacon: root, hops and path cost to the root,
//   its parent, its transmit power. Each node picks the neighbour with the
//   cheapest path as its parent (cost from path loss and beacon loss, with
//   hysteresis). The root is the device that bridges the session for the
//   console, or the device that started the mesh test.
// - Toward the root (quiz frames with origin != 0, topology reports): unicast
//   to the parent, which ESP-NOW acknowledges and repeats. Without a parent,
//   or after a failed unicast, the frame goes out as a broadcast as before.
// - From the root (quiz frames with origin 0, the trace): broadcast. A node
//   with children sends each new frame on once, hops + 1. Downstream quiz
//   frames reach the game only in msgId order, so a late relayed copy never
//   rolls the round back.
// - Transmit power: just loud enough for the parent and the children with a
//   reserve (kTargetRssi), full power without a route, while helping a lost
//   neighbour and while the session's login is open.
// - Radio discipline: while a round is open, no topology reports and the
//   slowest beacon period (outside the mesh test).
// - Mesh test: the root sets a test mode in its beacons, every node takes it
//   over from its neighbours and shows its place in the tree on its LEDs
//   (MeshTestRender.h). Nodes report their links upward; the root keeps the
//   table for the hub display and the USB link.
namespace meshnet {

// MsgType values (src/mesh/MeshTypes.h mirrors them).
static constexpr uint8_t kMsgBeacon = 50;
static constexpr uint8_t kMsgTopo = 51;
static constexpr uint8_t kMsgTrace = 53;
static constexpr uint8_t kVersion = 1;

inline bool isMeshType(uint8_t t) { return t == kMsgBeacon || t == kMsgTopo || t == kMsgTrace; }

static constexpr uint8_t kNoHops = 0xFF;
static constexpr uint16_t kNoCost = 0xFFFF;
static constexpr uint8_t kMaxHops = 6;
static constexpr uint8_t kNoBattery = 0xFF;

// Beacon flags.
static constexpr uint8_t kFlagMobile = 1 << 0;    // never a parent (teacher device)
static constexpr uint8_t kFlagPowerMax = 1 << 1;  // root: whole mesh at full power
static constexpr uint8_t kFlagUsb = 1 << 2;       // on USB power
static constexpr uint8_t kFlagRoundOpen = 1 << 3;

enum class TestMode : uint8_t { Off = 0, Tree = 1, Slow = 2 };

struct __attribute__((packed)) BeaconMsg {
    uint8_t type;  // kMsgBeacon
    uint8_t ver;   // kVersion
    uint16_t rootTag;
    uint16_t rootSeq;  // the root's beacon counter, passed on unchanged
    uint8_t hops;      // 0 = root, kNoHops = no route
    uint8_t flags;
    uint16_t cost;  // path cost to the root, 1/16 ETX
    uint8_t parent[6];
    int8_t txDbm;  // power this beacon went out with
    uint8_t children;
    uint8_t battery;     // percent, kNoBattery = unknown
    uint8_t intervalDs;  // own beacon period, 100 ms
    uint16_t seq;        // per sender
    uint16_t number;     // game number, 0 = none
    uint16_t testNonce;  // newer nonce wins
    uint8_t testMode;    // TestMode
    uint16_t testLeftS;  // seconds left
};
static_assert(sizeof(BeaconMsg) == 29, "BeaconMsg layout");

struct __attribute__((packed)) TopoMsg {
    uint8_t type;  // kMsgTopo
    uint8_t ver;
    uint8_t origin[6];
    uint16_t seq;
    uint8_t fwd;  // relays so far
    uint16_t number;
    uint8_t parent[6];
    uint8_t hops;
    uint16_t cost;
    int8_t parentRssi;  // parent's beacons as received
    int8_t txDbm;
    uint8_t neighbors;
    uint8_t children;
    uint8_t battery;
    uint8_t flags;
    uint16_t sent;      // own frames since boot
    uint16_t relayed;   // frames sent on for others
    uint16_t failed;    // unacknowledged unicasts
    uint16_t switches;  // parent changes
};
static_assert(sizeof(TopoMsg) == 36, "TopoMsg layout");

struct __attribute__((packed)) TraceMsg {
    uint8_t type;  // kMsgTrace
    uint8_t ver;
    uint16_t rootTag;
    uint16_t seq;
    uint8_t hops;
};
static_assert(sizeof(TraceMsg) == 7, "TraceMsg layout");

inline bool meshFrameLenOk(const uint8_t* d, size_t len) {
    if (len < 2 || d[1] != kVersion) return false;
    switch (d[0]) {
        case kMsgBeacon:
            return len == sizeof(BeaconMsg);
        case kMsgTopo:
            return len == sizeof(TopoMsg);
        case kMsgTrace:
            return len == sizeof(TraceMsg);
        default:
            return false;
    }
}

// ── Tunables (proposals, to be checked on devices) ───────────────────────────
static constexpr uint32_t kBeaconMinMs = 500;
static constexpr uint32_t kBeaconMaxMs = 4000;
static constexpr uint32_t kBeaconTestMs = 1000;
static constexpr uint32_t kRootStaleMs = 20000;  // no new root beacon: route gone
static constexpr uint32_t kParentMinTimeoutMs = 3000;
static constexpr uint32_t kParentQuietMs = 2500;  // parent silent while the root sends
static constexpr uint32_t kSwitchGapMs = 2000;    // between two parent changes
static constexpr uint16_t kSwitchMargin = 12;     // 0.75 ETX better before changing
static constexpr uint8_t kAckFailsDrop = 3;       // unacknowledged unicasts in a row
static constexpr uint32_t kAckPenaltyMs = 5000;
static constexpr int8_t kTargetRssi = -67;  // wanted level at the far end, about 28 dB reserve
static constexpr int8_t kMinDbm = 2;
static constexpr int8_t kFailBoostDb = 6;
static constexpr uint32_t kFailBoostMs = 15000;
static constexpr uint32_t kHelperMs = 5000;  // full power after hearing a lost neighbour
static constexpr uint32_t kPowerStepDownMs = 1000;
static constexpr uint32_t kTopoTestMs = 2000;
static constexpr uint32_t kTopoIdleMs = 30000;
static constexpr uint32_t kTraceTreeMs = 3000;
static constexpr uint32_t kTraceSlowMs = 6000;
static constexpr uint32_t kSlowHopMs = 800;  // slow motion: hold per hop
static constexpr uint32_t kLoginHoldMs = 2500;
static constexpr uint32_t kRoundOpenHoldMs = 2500;
static constexpr uint32_t kDedupMs = 4000;
static constexpr uint32_t kNodeLostMs = 3 * kTopoTestMs + 1000;
static constexpr uint8_t kMaxNeighbors = 16;
static constexpr uint8_t kMaxNodes = 64;
static constexpr uint8_t kRxSlots = 32;

inline bool seqNewer(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d >= 1 && d <= 0x7FFF;
}

class Router {
   public:
    struct Io {
        std::function<void(const uint8_t* d, size_t len)> broadcast;
        // False if the frame could not be queued.
        std::function<bool(const uint8_t* mac, const uint8_t* d, size_t len)> unicast;
        std::function<void(const uint8_t* d, size_t len, int8_t rssi)> deliverQuiz;
        std::function<void(int8_t dbm)> setTxPower;
        std::function<uint32_t(uint32_t lo, uint32_t hi)> randomRange;
        // A known parent changed (old may be all zero): add/remove the ESP-NOW peer.
        std::function<void(const uint8_t* oldMac, const uint8_t* newMac)> parentChanged;
    };

    struct Self {
        uint16_t number = 0;
        uint8_t battery = kNoBattery;
        bool usb = false;
        bool mobile = false;
    };

    struct Neighbor {
        bool used;
        uint8_t mac[6];
        uint16_t rootTag;
        uint16_t rootSeq;
        uint8_t hops;
        uint8_t flags;
        uint16_t cost;
        uint8_t parent[6];
        int8_t tx;
        uint8_t children;
        uint8_t battery;
        uint16_t intervalMs;
        uint16_t seq;
        uint16_t number;
        int16_t pl4;  // path loss, quarter dB (EWMA)
        int8_t rssi;  // last beacon
        uint8_t prr;  // beacon reception, 0-255 (EWMA)
        uint8_t ackFails;
        uint32_t heardMs;   // any frame
        uint32_t beaconMs;  // last beacon
        uint32_t penaltyUntil;
    };

    // Root table: one entry per node that reported (or was heard) recently.
    struct Node {
        bool used;
        uint8_t mac[6];
        uint16_t number;
        uint8_t parent[6];
        uint8_t hops;  // kNoHops = no route of its own
        uint16_t cost;
        int8_t parentRssi;
        int8_t txDbm;
        uint8_t neighbors;
        uint8_t children;
        uint8_t battery;
        uint8_t flags;
        uint16_t sent, relayed, failed, switches;
        uint32_t heardMs;
        bool dirty;  // for the USB link
    };

    enum class Act : uint8_t { TxOwn, RxDown, FwdDown, FwdUp, Trace };
    struct Activity {
        Act kind;
        uint32_t at;
    };

    void begin(const Io& io, const uint8_t* mac, int8_t maxDbm, uint32_t now) {
        _io = io;
        memcpy(_mac, mac, 6);
        _maxDbm = maxDbm;
        _txDbm = maxDbm;
        _rootTagOwn = (uint16_t)_rand(1, 0xFFFE);
        _nextBeaconAt = now + _rand(50, 400);
        _nextTopoAt = now + _rand(2000, 5000);
        _interval = kBeaconMinMs;
        _begun = true;
    }

    // ── Inputs ───────────────────────────────────────────────────────────────
    // ESP-NOW receive callback (WiFi task): copy only.
    void push(const uint8_t* mac, bool toMe, const uint8_t* d, size_t len, int8_t rssi,
              uint32_t rxMs) {
        if (len == 0 || len > game::kMaxFrameLen) return;
        _lock();
        if (_rxCount >= kRxSlots) {
            _rxOverflow++;
            _unlock();
            return;
        }
        Rx& r = _rx[(_rxHead + _rxCount) % kRxSlots];
        memcpy(r.mac, mac, 6);
        r.toMe = toMe;
        r.len = (uint8_t)len;
        r.rssi = rssi;
        r.at = rxMs;
        memcpy(r.data, d, len);
        _rxCount++;
        _unlock();
    }

    // ESP-NOW send callback (WiFi task): unicast results, in send order.
    void pushSent(const uint8_t* mac, bool ok) {
        if (!mac || (mac[0] & 1)) return;  // broadcast
        _lock();
        if (_sentCount < kSentSlots) {
            Sent& s = _sent[(_sentHead + _sentCount) % kSentSlots];
            memcpy(s.mac, mac, 6);
            s.ok = ok;
            _sentCount++;
        }
        _unlock();
    }

    // A quiz frame from this device's game (participant or bridge).
    void sendQuiz(const uint8_t* d, size_t len, uint32_t now) {
        if (!game::quizFrameLenOk(d, len)) return;
        game::QuizHeader h;
        memcpy(&h, d, sizeof(h));
        _noteQuiz(d, len, now);
        _sent_++;
        _act(Act::TxOwn, now);
        if (h.origin == 0) {
            // Bridge: its own copy counts as delivered, so a relayed echo is old.
            _downSeen = true;
            _downLast = h.msgId;
            _downAt = now;
            _bcast(d, len);
        } else {
            _remember(_hashUp(d, len), now);
            _sendUp(d, len, now);
        }
    }

    void setSelf(const Self& s) { _self = s; }
    // This device bridges the session for the console.
    void setRoot(bool root) { _bridgeRoot = root; }
    // Power may go below full: no WiFi connection, the session holds the radio.
    void setAdaptiveAllowed(bool on) { _adaptiveAllowed = on; }
    // USB link `mesh power`: full power everywhere (carried in the root's beacons).
    void setPowerMax(bool on) { _powerMaxOwn = on; }

    // Starts (or ends, mode Off) the mesh test for the whole mesh from here.
    void startTest(TestMode mode, uint16_t seconds, uint32_t now) {
        _testNonce = (uint16_t)(_testNonceSeen + 1);
        _testNonceSeen = _testNonce;
        _testMode = mode;
        _testUntil = now + (uint32_t)seconds * 1000;
        _testInitiator = mode != TestMode::Off;
        _resetTrickle(now);
        if (_testInitiator) {
            _nextTraceAt = now + 1000;
            _nextTopoAt = now + _rand(200, 800);
        }
    }

    void tick(uint32_t now) {
        if (!_begun) return;
        _now = now;
        _drainSent();
        Rx r;
        while (_popRx(r)) _onFrame(r, now);
        if (_testMode != TestMode::Off && (int32_t)(now - _testUntil) >= 0) {
            _testMode = TestMode::Off;
            _testInitiator = false;
        }
        _expire(now);
        _selectParent(now);
        _updatePower(now);
        _servicePending(now);
        if ((int32_t)(now - _nextBeaconAt) >= 0) _sendBeacon(now);
        _serviceTopo(now);
        _serviceTrace(now);
    }

    // ── State ────────────────────────────────────────────────────────────────
    bool isRoot() const { return _bridgeRoot || _testInitiator; }
    bool hasRoute() const { return isRoot() || _parent >= 0; }
    uint8_t hops() const { return isRoot() ? 0 : _parent >= 0 ? _hops : kNoHops; }
    uint16_t cost() const { return isRoot() ? 0 : _parent >= 0 ? _cost : kNoCost; }
    const Neighbor* parent() const { return _parent >= 0 ? &_nb[_parent] : nullptr; }
    int8_t txDbm() const { return _txDbm; }
    int8_t maxDbm() const { return _maxDbm; }
    bool powerMax() const { return _powerMaxMesh(); }
    uint8_t children() const { return _children; }
    uint8_t neighborCount() const {
        uint8_t n = 0;
        for (const Neighbor& x : _nb) n += x.used ? 1 : 0;
        return n;
    }
    const Neighbor* neighbors() const { return _nb; }
    bool roundOpen(uint32_t now) const {
        return _roundOpenSeen && now - _roundOpenAt < kRoundOpenHoldMs;
    }
    TestMode testMode() const { return _testMode; }
    bool testActive() const { return _testMode != TestMode::Off; }
    bool testSlow() const { return _testMode == TestMode::Slow; }
    uint32_t testLeftMs(uint32_t now) const {
        return testActive() && (int32_t)(_testUntil - now) > 0 ? _testUntil - now : 0;
    }
    uint16_t sentCount() const { return _sent_; }
    uint16_t relayedCount() const { return _relayed; }
    uint16_t failedCount() const { return _failed; }
    uint16_t switchCount() const { return _switches; }
    uint32_t rxOverflow() const { return _rxOverflow; }
    const uint8_t* mac() const { return _mac; }

    // Latest activities, newest first (i = 0..kActs-1); at == 0: none.
    static constexpr uint8_t kActs = 8;
    const Activity& activity(uint8_t i) const {
        return _acts[(uint8_t)(_actHead + kActs - 1 - i) % kActs];
    }

    // Root table.
    const Node* nodes() const { return _nodes; }
    Node* nodesMut() { return _nodes; }
    bool nodeLost(const Node& n, uint32_t now) const {
        uint32_t limit = testActive() ? kNodeLostMs : 2 * kTopoIdleMs + 5000;
        return now - n.heardMs > limit;
    }

   private:
    struct Rx {
        uint8_t mac[6];
        bool toMe;
        uint8_t len;
        int8_t rssi;
        uint32_t at;
        uint8_t data[game::kMaxFrameLen];
    };
    static constexpr uint8_t kSentSlots = 16;
    struct Sent {
        uint8_t mac[6];
        bool ok;
    };
    // A frame waiting for its unicast result (fallback: broadcast).
    struct Pending {
        bool used;
        uint8_t mac[6];
        uint8_t len;
        uint8_t data[game::kMaxFrameLen];
    };
    static constexpr uint8_t kPendingSlots = 8;
    // A frame held for slow motion.
    struct Held {
        bool used;
        bool up;
        uint32_t due;
        uint8_t len;
        uint8_t data[sizeof(TopoMsg)];
    };
    static constexpr uint8_t kHeldSlots = 6;

    Io _io;
    Self _self;
    bool _begun = false;
    uint8_t _mac[6] = {};
    uint32_t _now = 0;

    Rx _rx[kRxSlots];
    uint8_t _rxHead = 0, _rxCount = 0;
    uint32_t _rxOverflow = 0;
    Sent _sent[kSentSlots];
    uint8_t _sentHead = 0, _sentCount = 0;
    Pending _pending[kPendingSlots] = {};
    uint8_t _pendHead = 0, _pendCount = 0;
    Held _held[kHeldSlots] = {};

    Neighbor _nb[kMaxNeighbors] = {};
    int8_t _parent = -1;
    uint8_t _hops = kNoHops;
    uint16_t _cost = kNoCost;
    uint16_t _rootTag = 0;  // of the route in use
    uint16_t _rootSeq = 0;
    uint32_t _rootSeqAt = 0;
    uint16_t _rootTagOwn = 0;
    uint16_t _rootSeqOwn = 0;
    uint32_t _lastSwitch = 0;
    uint8_t _children = 0;
    bool _bridgeRoot = false;
    bool _parentPowerMax = false;

    // Power.
    bool _adaptiveAllowed = false;
    bool _powerMaxOwn = false;
    int8_t _maxDbm = 20;
    int8_t _txDbm = 20;
    int8_t _applied = 127;
    uint32_t _appliedAt = 0;
    uint32_t _boostUntil = 0;
    uint32_t _helperUntil = 0;
    uint32_t _lastStepDown = 0;

    // Beacons.
    uint32_t _nextBeaconAt = 0;
    uint32_t _interval = kBeaconMinMs;
    uint16_t _beaconSeq = 0;
    uint32_t _lastTrickleReset = 0;

    // Game traffic seen.
    bool _downSeen = false;
    uint16_t _downLast = 0;
    uint32_t _downAt = 0;
    bool _roundOpenSeen = false;
    uint32_t _roundOpenAt = 0;
    bool _loginSeen = false;
    uint32_t _loginAt = 0;
    struct Seen {
        uint32_t hash;
        uint32_t at;
    };
    static constexpr uint8_t kSeenSlots = 48;
    Seen _seen[kSeenSlots] = {};
    uint8_t _seenNext = 0;

    // Test.
    TestMode _testMode = TestMode::Off;
    uint16_t _testNonce = 0;
    uint16_t _testNonceSeen = 0;
    uint32_t _testUntil = 0;
    bool _testInitiator = false;
    uint32_t _nextTraceAt = 0;
    uint16_t _traceSeq = 0;
    uint16_t _traceLast = 0;
    bool _traceSeen = false;
    uint32_t _nextTopoAt = 0;
    uint16_t _topoSeq = 0;

    // Counters.
    uint16_t _sent_ = 0, _relayed = 0, _failed = 0, _switches = 0;

    Activity _acts[kActs] = {};
    uint8_t _actHead = 0;

    Node _nodes[kMaxNodes] = {};

#if defined(ESP_PLATFORM)
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    void _lock() { portENTER_CRITICAL(&_mux); }
    void _unlock() { portEXIT_CRITICAL(&_mux); }
#else
    void _lock() {}
    void _unlock() {}
#endif

    uint32_t _rand(uint32_t lo, uint32_t hi) {
        return _io.randomRange ? _io.randomRange(lo, hi) : lo;
    }

    bool _popRx(Rx& out) {
        _lock();
        if (_rxCount == 0) {
            _unlock();
            return false;
        }
        out = _rx[_rxHead];
        _rxHead = (uint8_t)((_rxHead + 1) % kRxSlots);
        _rxCount--;
        _unlock();
        return true;
    }

    void _act(Act k, uint32_t now) {
        _acts[_actHead] = {k, now ? now : 1};
        _actHead = (uint8_t)((_actHead + 1) % kActs);
    }

    bool _powerMaxMesh() const { return _powerMaxOwn || (!isRoot() && _parentPowerMax); }

    // ── Receive ──────────────────────────────────────────────────────────────
    void _onFrame(const Rx& r, uint32_t now) {
        if (memcmp(r.mac, _mac, 6) == 0) return;
        Neighbor* n = _find(r.mac);
        if (n) n->heardMs = now;
        uint8_t type = r.data[0];
        if (type == kMsgBeacon) {
            if (!meshFrameLenOk(r.data, r.len)) return;
            BeaconMsg b;
            memcpy(&b, r.data, sizeof(b));
            _onBeacon(r.mac, b, r.rssi, now);
            return;
        }
        if (type == kMsgTopo) {
            if (!meshFrameLenOk(r.data, r.len)) return;
            _onTopo(r, now);
            return;
        }
        if (type == kMsgTrace) {
            if (!meshFrameLenOk(r.data, r.len)) return;
            _onTrace(r, now);
            return;
        }
        if (!game::quizFrameLenOk(r.data, r.len)) return;
        game::QuizHeader h;
        memcpy(&h, r.data, sizeof(h));
        if (h.origin == 0)
            _onQuizDown(r, h, now);
        else
            _onQuizUp(r, h, now);
    }

    void _onQuizDown(const Rx& r, const game::QuizHeader& h, uint32_t now) {
        // In msgId order only: a frame not newer than the last delivered one
        // is a copy or a late relay. A big step back or a long pause is a
        // restarted bridge.
        if (_downSeen && now - _downAt < 3000) {
            uint16_t back = (uint16_t)(_downLast - h.msgId);
            if (!seqNewer(h.msgId, _downLast) && back < 256) return;
        }
        _downSeen = true;
        _downLast = h.msgId;
        _downAt = now;
        _noteQuiz(r.data, r.len, now);
        if (_parent >= 0 && memcmp(r.mac, _nb[_parent].mac, 6) == 0) _act(Act::RxDown, now);
        if (_io.deliverQuiz) _io.deliverQuiz(r.data, r.len, r.rssi);
        if (_children == 0 || isRoot() || h.hops >= kMaxHops) return;
        uint8_t out[game::kMaxFrameLen];
        memcpy(out, r.data, r.len);
        out[offsetof(game::QuizHeader, hops)] = (uint8_t)(h.hops + 1);
        if (h.type == game::kMsgQuizState) {
            // Transparent clock: add the time this frame spent here.
            int32_t off;
            memcpy(&off, out + offsetof(game::QuizStateMsg, openOffsetMs), 4);
            if (off != 0) {
                off += (int32_t)(now - r.at) + 1;
                memcpy(out + offsetof(game::QuizStateMsg, openOffsetMs), &off, 4);
            }
        }
        _relayed++;
        _act(Act::FwdDown, now);
        _bcast(out, r.len);
    }

    void _onQuizUp(const Rx& r, const game::QuizHeader& h, uint32_t now) {
        uint32_t key = _hashUp(r.data, r.len);
        if (_seenRecently(key, now)) return;
        _remember(key, now);
        if (_io.deliverQuiz) _io.deliverQuiz(r.data, r.len, r.rssi);
        if (isRoot() || !_forwardsUp(r)) return;
        if (h.hops >= kMaxHops) return;
        uint8_t out[game::kMaxFrameLen];
        memcpy(out, r.data, r.len);
        out[offsetof(game::QuizHeader, hops)] = (uint8_t)(h.hops + 1);
        _relayed++;
        _act(Act::FwdUp, now);
        _sendUp(out, r.len, now);
    }

    // Sent on toward the root: unicast to this device, or a broadcast from a child.
    bool _forwardsUp(const Rx& r) const {
        if (_parent < 0) return false;
        if (r.toMe) return true;
        const Neighbor* n = _findC(r.mac);
        return n && memcmp(n->parent, _mac, 6) == 0 && _fresh(*n, _now);
    }

    void _onBeacon(const uint8_t* mac, const BeaconMsg& b, int8_t rssi, uint32_t now) {
        Neighbor* n = _find(mac);
        if (!n) n = _alloc(mac, now);
        if (!n) return;
        int16_t pl4 = (int16_t)(((int16_t)b.txDbm - rssi) * 4);
        if (n->beaconMs == 0) {
            n->pl4 = pl4;
            n->prr = 200;
        } else {
            n->pl4 = (int16_t)(n->pl4 + (pl4 - n->pl4) / 4);
            uint16_t gap = (uint16_t)(b.seq - n->seq);
            for (uint16_t i = 1; i < gap && i < 10; i++) n->prr = (uint8_t)(n->prr - n->prr / 8);
            n->prr = (uint8_t)(n->prr + (255 - n->prr) / 8);
        }
        n->rootTag = b.rootTag;
        n->rootSeq = b.rootSeq;
        n->hops = b.hops;
        n->flags = b.flags;
        n->cost = b.cost;
        memcpy(n->parent, b.parent, 6);
        n->tx = b.txDbm;
        n->children = b.children;
        n->battery = b.battery;
        n->intervalMs = (uint16_t)(b.intervalDs * 100);
        n->seq = b.seq;
        n->number = b.number;
        n->rssi = rssi;
        n->heardMs = now;
        n->beaconMs = now;

        if (b.hops != kNoHops && b.rootTag == _rootTag && seqNewer(b.rootSeq, _rootSeq)) {
            _rootSeq = b.rootSeq;
            _rootSeqAt = now;
        }
        if (_parent >= 0 && n == &_nb[_parent]) _parentPowerMax = (b.flags & kFlagPowerMax) != 0;

        // Wegsuche: a neighbour without a route; answer loud and soon.
        if (b.hops == kNoHops && hasRoute() && !roundOpen(now)) {
            _helperUntil = now + kHelperMs;
            if (now - _lastTrickleReset > 1000) _resetTrickle(now);
        }

        // Test mode: the newer nonce wins.
        if (seqNewer(b.testNonce, _testNonceSeen) || (b.testNonce != 0 && _testNonceSeen == 0)) {
            _testNonceSeen = b.testNonce;
            _testNonce = b.testNonce;
            _testMode = (TestMode)(b.testMode <= 2 ? b.testMode : 0);
            _testUntil = now + (uint32_t)b.testLeftS * 1000;
            _testInitiator = false;
            _nextTopoAt = now + _rand(100, 1500);
            _resetTrickle(now);
        }

        // Root table: the root also lists its direct neighbours.
        if (isRoot()) {
            Node* nd = _node(mac, now);
            if (nd && (nd->heardMs == 0 || now - nd->heardMs > 2 * kTopoTestMs)) {
                nd->number = b.number;
                memcpy(nd->parent, b.parent, 6);
                nd->hops = b.hops == kNoHops ? kNoHops : (uint8_t)(b.hops);
                nd->cost = b.cost;
                nd->parentRssi = memcmp(b.parent, _mac, 6) == 0 ? rssi : nd->parentRssi;
                nd->txDbm = b.txDbm;
                nd->children = b.children;
                nd->battery = b.battery;
                nd->flags = b.flags;
                nd->heardMs = now;
                nd->dirty = true;
            }
        }
    }

    void _onTopo(const Rx& r, uint32_t now) {
        TopoMsg t;
        memcpy(&t, r.data, sizeof(t));
        if (memcmp(t.origin, _mac, 6) == 0) return;
        uint32_t key = _hashUp(r.data, r.len);
        if (_seenRecently(key, now)) return;
        _remember(key, now);
        if (isRoot()) {
            Node* nd = _node(t.origin, now);
            if (!nd) return;
            nd->number = t.number;
            memcpy(nd->parent, t.parent, 6);
            nd->hops = t.hops;
            nd->cost = t.cost;
            nd->parentRssi = t.parentRssi;
            nd->txDbm = t.txDbm;
            nd->neighbors = t.neighbors;
            nd->children = t.children;
            nd->battery = t.battery;
            nd->flags = t.flags;
            nd->sent = t.sent;
            nd->relayed = t.relayed;
            nd->failed = t.failed;
            nd->switches = t.switches;
            nd->heardMs = now;
            nd->dirty = true;
            return;
        }
        if (!_forwardsUp(r) || t.fwd >= kMaxHops) return;
        t.fwd++;
        _relayed++;
        if (testSlow()) {
            _hold((const uint8_t*)&t, sizeof(t), true, now + kSlowHopMs);
            return;
        }
        _act(Act::FwdUp, now);
        _sendUp((const uint8_t*)&t, sizeof(t), now);
    }

    void _onTrace(const Rx& r, uint32_t now) {
        TraceMsg t;
        memcpy(&t, r.data, sizeof(t));
        if (isRoot()) return;
        if (_traceSeen && !seqNewer(t.seq, _traceLast)) return;
        _traceSeen = true;
        _traceLast = t.seq;
        _act(Act::Trace, now);
        if (_children == 0 || t.hops >= kMaxHops) return;
        t.hops++;
        if (testSlow()) {
            _hold((const uint8_t*)&t, sizeof(t), false, now + kSlowHopMs);
            return;
        }
        _relayed++;
        _act(Act::FwdDown, now);
        _bcast((const uint8_t*)&t, sizeof(t));
    }

    // Round open and login, from the frames passing through (radio discipline).
    void _noteQuiz(const uint8_t* d, size_t len, uint32_t now) {
        if (d[0] == game::kMsgQuizInvite) {
            _loginSeen = true;
            _loginAt = now;
        }
        if (d[0] == game::kMsgQuizState && len >= game::kQuizStateHeadLen) {
            uint8_t type = d[offsetof(game::QuizStateMsg, roundType)];
            uint8_t phase = d[offsetof(game::QuizStateMsg, phase)];
            if (type != (uint8_t)game::RoundType::Idle && phase == (uint8_t)game::Phase::Open) {
                _roundOpenSeen = true;
                _roundOpenAt = now;
            } else {
                _roundOpenSeen = false;
            }
        }
    }

    bool _loginOpen(uint32_t now) const { return _loginSeen && now - _loginAt < kLoginHoldMs; }

    // ── Send ─────────────────────────────────────────────────────────────────
    void _bcast(const uint8_t* d, size_t len) {
        if (_io.broadcast) _io.broadcast(d, len);
    }

    void _sendUp(const uint8_t* d, size_t len, uint32_t now) {
        (void)now;
        if (_parent < 0 || !_io.unicast) {
            _bcast(d, len);
            return;
        }
        const uint8_t* mac = _nb[_parent].mac;
        if (_pendCount >= kPendingSlots || !_io.unicast(mac, d, len)) {
            _bcast(d, len);
            return;
        }
        Pending& p = _pending[(_pendHead + _pendCount) % kPendingSlots];
        p.used = true;
        memcpy(p.mac, mac, 6);
        p.len = (uint8_t)len;
        memcpy(p.data, d, len);
        _pendCount++;
    }

    void _drainSent() {
        for (;;) {
            Sent s;
            _lock();
            if (_sentCount == 0) {
                _unlock();
                return;
            }
            s = _sent[_sentHead];
            _sentHead = (uint8_t)((_sentHead + 1) % kSentSlots);
            _sentCount--;
            _unlock();
            if (_pendCount == 0) continue;
            Pending& p = _pending[_pendHead];
            _pendHead = (uint8_t)((_pendHead + 1) % kPendingSlots);
            _pendCount--;
            Neighbor* n = _find(p.mac);
            if (s.ok) {
                if (n) n->ackFails = 0;
                continue;
            }
            _failed++;
            _boostUntil = _now + kFailBoostMs;
            if (n && ++n->ackFails >= kAckFailsDrop) {
                n->ackFails = 0;
                n->penaltyUntil = _now + kAckPenaltyMs;
            }
            // Fallback: as a broadcast, for the root or the parent to hear.
            _bcast(p.data, p.len);
        }
    }

    void _hold(const uint8_t* d, size_t len, bool up, uint32_t due) {
        for (Held& h : _held) {
            if (h.used) continue;
            h.used = true;
            h.up = up;
            h.due = due;
            h.len = (uint8_t)len;
            memcpy(h.data, d, len);
            return;
        }
    }

    void _servicePending(uint32_t now) {
        for (Held& h : _held) {
            if (!h.used || (int32_t)(now - h.due) < 0) continue;
            h.used = false;
            if (h.up) {
                _act(Act::FwdUp, now);
                _sendUp(h.data, h.len, now);
            } else {
                _relayed++;
                _act(Act::FwdDown, now);
                _bcast(h.data, h.len);
            }
        }
    }

    void _sendBeacon(uint32_t now) {
        BeaconMsg b{};
        b.type = kMsgBeacon;
        b.ver = kVersion;
        if (isRoot()) {
            b.rootTag = _rootTagOwn;
            b.rootSeq = ++_rootSeqOwn;
            b.hops = 0;
            b.cost = 0;
            memset(b.parent, 0, 6);
        } else if (_parent >= 0) {
            b.rootTag = _rootTag;
            b.rootSeq = _rootSeq;
            b.hops = _hops;
            b.cost = _cost;
            memcpy(b.parent, _nb[_parent].mac, 6);
        } else {
            b.hops = kNoHops;
            b.cost = kNoCost;
        }
        b.flags =
            (uint8_t)((_self.mobile ? kFlagMobile : 0) | (_powerMaxMesh() ? kFlagPowerMax : 0) |
                      (_self.usb ? kFlagUsb : 0) | (roundOpen(now) ? kFlagRoundOpen : 0));
        b.txDbm = _txDbm;
        b.children = _children;
        b.battery = _self.battery;
        uint32_t iv = _period(now);
        b.intervalDs = (uint8_t)(iv / 100 > 255 ? 255 : iv / 100);
        b.seq = ++_beaconSeq;
        b.number = _self.number;
        b.testNonce = _testNonce;
        b.testMode = (uint8_t)_testMode;
        uint32_t left = testLeftMs(now) / 1000;
        b.testLeftS = (uint16_t)(left > 0xFFFF ? 0xFFFF : left);
        _bcast((const uint8_t*)&b, sizeof(b));
        // Trickle: double the period while nothing changes.
        if (_interval < kBeaconMaxMs) _interval *= 2;
        if (_interval > kBeaconMaxMs) _interval = kBeaconMaxMs;
        uint32_t p = _period(now);
        _nextBeaconAt = now + p - p / 8 + _rand(0, p / 4);
    }

    uint32_t _period(uint32_t now) const {
        if (testActive()) return kBeaconTestMs;
        if (roundOpen(now) && hasRoute()) return kBeaconMaxMs;
        return _interval;
    }

    void _resetTrickle(uint32_t now) {
        _lastTrickleReset = now;
        _interval = kBeaconMinMs;
        uint32_t soon = now + _rand(20, 120);
        if ((int32_t)(_nextBeaconAt - soon) > 0) _nextBeaconAt = soon;
    }

    void _serviceTopo(uint32_t now) {
        if (isRoot() || (int32_t)(now - _nextTopoAt) < 0) return;
        // Radio discipline: nothing extra while a round is open.
        if (!testActive() && roundOpen(now)) {
            _nextTopoAt = now + 1000;
            return;
        }
        uint32_t period = testActive() ? (testSlow() ? 2 * kTopoTestMs : kTopoTestMs) : kTopoIdleMs;
        _nextTopoAt = now + period - period / 10 + _rand(0, period / 5);
        if (!testActive() && !_rootTagSeen()) return;
        TopoMsg t{};
        t.type = kMsgTopo;
        t.ver = kVersion;
        memcpy(t.origin, _mac, 6);
        t.seq = ++_topoSeq;
        t.number = _self.number;
        if (_parent >= 0) {
            memcpy(t.parent, _nb[_parent].mac, 6);
            t.parentRssi = _nb[_parent].rssi;
        }
        t.hops = hops();
        t.cost = cost();
        t.txDbm = _txDbm;
        t.neighbors = neighborCount();
        t.children = _children;
        t.battery = _self.battery;
        t.flags = (uint8_t)((_self.mobile ? kFlagMobile : 0) | (_self.usb ? kFlagUsb : 0));
        t.sent = _sent_;
        t.relayed = _relayed;
        t.failed = _failed;
        t.switches = _switches;
        _remember(_hashUp((const uint8_t*)&t, sizeof(t)), now);
        _act(Act::TxOwn, now);
        _sendUp((const uint8_t*)&t, sizeof(t), now);
    }

    bool _rootTagSeen() const { return _parent >= 0; }

    void _serviceTrace(uint32_t now) {
        if (!_testInitiator && !(isRoot() && testActive())) return;
        if ((int32_t)(now - _nextTraceAt) < 0) return;
        _nextTraceAt = now + (testSlow() ? kTraceSlowMs : kTraceTreeMs);
        TraceMsg t{};
        t.type = kMsgTrace;
        t.ver = kVersion;
        t.rootTag = _rootTagOwn;
        t.seq = ++_traceSeq;
        t.hops = 0;
        _act(Act::Trace, now);
        _bcast((const uint8_t*)&t, sizeof(t));
    }

    // ── Tree ─────────────────────────────────────────────────────────────────
    bool _fresh(const Neighbor& n, uint32_t now) const {
        uint32_t limit = 3u * (n.intervalMs ? n.intervalMs : kBeaconMaxMs) + 500;
        if (limit < kParentMinTimeoutMs) limit = kParentMinTimeoutMs;
        return n.used && now - n.heardMs <= limit;
    }

    // Link cost in 1/16 ETX from the path loss (as if sent at full power) and
    // the beacon reception; kNoCost = not usable.
    uint16_t _linkCost(const Neighbor& n, uint32_t now) const {
        int rssiFull = _maxDbm - n.pl4 / 4;
        if (rssiFull < -92) return kNoCost;
        // 1 ETX down to -65 dBm, 2 at -80, 6 at -92: weak links get expensive
        // before they start losing frames.
        int base;
        if (rssiFull >= -65)
            base = 16;
        else if (rssiFull >= -80)
            base = 16 + (-65 - rssiFull) * 16 / 15;
        else
            base = 32 + (-80 - rssiFull) * 64 / 12;
        int prr = n.prr < 40 ? 40 : n.prr;
        int c = base * 255 / prr;
        if (n.battery != kNoBattery && n.battery < 15 && !(n.flags & kFlagUsb)) c += 32;
        if ((int32_t)(n.penaltyUntil - now) > 0) c += 64;
        return (uint16_t)(c > 0xFFF0 ? 0xFFF0 : c);
    }

    bool _eligible(const Neighbor& n, uint32_t now) const {
        if (!_fresh(n, now) || n.beaconMs == 0) return false;
        if (n.hops == kNoHops || n.hops >= kMaxHops || n.cost == kNoCost) return false;
        if (n.flags & kFlagMobile) return false;
        if (memcmp(n.parent, _mac, 6) == 0) return false;  // my child
        return true;
    }

    void _selectParent(uint32_t now) {
        if (isRoot()) {
            if (_parent >= 0) _setParent(-1, now);
            _children = _countChildren(now);
            return;
        }
        // The route in use went stale: the root stopped beaconing.
        if (_parent >= 0 && now - _rootSeqAt > kRootStaleMs) _setParent(-1, now);
        if (_parent >= 0 && !_fresh(_nb[_parent], now)) _setParent(-1, now);
        // Session traffic flows but the parent stopped passing it on.
        if (_parent >= 0 && _downSeen && now - _downAt < kParentQuietMs &&
            now - _nb[_parent].heardMs > kParentQuietMs) {
            // Gone until it is heard again.
            Neighbor& gone = _nb[_parent];
            _setParent(-1, now);
            gone.used = false;
        }

        int8_t best = -1;
        uint32_t bestCost = kNoCost;
        for (uint8_t i = 0; i < kMaxNeighbors; i++) {
            const Neighbor& n = _nb[i];
            if (!_eligible(n, now)) continue;
            uint16_t lc = _linkCost(n, now);
            if (lc == kNoCost) continue;
            uint32_t c = (uint32_t)n.cost + lc;
            if (c < bestCost) {
                bestCost = c;
                best = (int8_t)i;
            }
        }
        if (_parent >= 0) {
            const Neighbor& p = _nb[_parent];
            uint16_t lc = _linkCost(p, now);
            uint32_t cur = lc == kNoCost || !_eligible(p, now) ? 0xFFFFFFu : (uint32_t)p.cost + lc;
            bool better = best >= 0 && best != _parent && bestCost + kSwitchMargin < cur &&
                          now - _lastSwitch >= kSwitchGapMs;
            // Unicasts to it went unanswered: leave it if there is another way.
            bool broken = cur == 0xFFFFFFu ||
                          (best >= 0 && best != _parent && (int32_t)(p.penaltyUntil - now) > 0);
            if (better || broken) _setParent(best, now);
        } else if (best >= 0) {
            _setParent(best, now);
        }
        if (_parent >= 0) {
            const Neighbor& p = _nb[_parent];
            uint16_t lc = _linkCost(p, now);
            uint32_t c = (uint32_t)p.cost + (lc == kNoCost ? 0 : lc);
            uint8_t newHops = (uint8_t)(p.hops + 1);
            uint16_t newCost = (uint16_t)(c > 0xFFF0 ? 0xFFF0 : c);
            if (newHops != _hops) _resetTrickle(now);
            _hops = newHops;
            _cost = newCost;
        }
        _children = _countChildren(now);
    }

    void _setParent(int8_t idx, uint32_t now) {
        uint8_t oldMac[6] = {};
        if (_parent >= 0) memcpy(oldMac, _nb[_parent].mac, 6);
        if (idx == _parent) return;
        _parent = idx;
        _lastSwitch = now;
        _switches++;
        _resetTrickle(now);
        if (idx >= 0) {
            const Neighbor& p = _nb[idx];
            _rootTag = p.rootTag;
            _rootSeq = p.rootSeq;
            _rootSeqAt = now;
            _hops = (uint8_t)(p.hops + 1);
            _parentPowerMax = (p.flags & kFlagPowerMax) != 0;
        } else {
            _hops = kNoHops;
            _cost = kNoCost;
            _parentPowerMax = false;
        }
        // Unicasts still waiting refer to the old parent; their results drain normally.
        if (_io.parentChanged) _io.parentChanged(oldMac, idx >= 0 ? _nb[idx].mac : nullptr);
    }

    uint8_t _countChildren(uint32_t now) const {
        uint8_t c = 0;
        for (const Neighbor& n : _nb)
            if (n.used && _fresh(n, now) && memcmp(n.parent, _mac, 6) == 0 && n.hops != kNoHops)
                c++;
        return c;
    }

    // ── Power ────────────────────────────────────────────────────────────────
    void _updatePower(uint32_t now) {
        int want = _maxDbm;
        bool adaptive = _adaptiveAllowed && !_powerMaxMesh() && hasRoute() &&
                        (int32_t)(_helperUntil - now) <= 0 && !(isRoot() && _loginOpen(now));
        if (adaptive) {
            int need = -128;
            if (_parent >= 0) need = _nb[_parent].pl4 / 4 + kTargetRssi;
            for (const Neighbor& n : _nb) {
                if (!n.used || !_fresh(n, now) || memcmp(n.parent, _mac, 6) != 0) continue;
                int c = n.pl4 / 4 + kTargetRssi;
                if (c > need) need = c;
            }
            if (need == -128) {
                want = _maxDbm;  // root without children: be found
            } else {
                if ((int32_t)(_boostUntil - now) > 0) need += kFailBoostDb;
                want = need;
            }
        }
        if (want > _maxDbm) want = _maxDbm;
        if (want < kMinDbm) want = kMinDbm;
        if (want > _txDbm) {
            _txDbm = (int8_t)want;  // up at once
        } else if (want < _txDbm - 1 && now - _lastStepDown >= kPowerStepDownMs) {
            _txDbm = (int8_t)(_txDbm - 1);  // down slowly
            _lastStepDown = now;
        }
        // Again now and then: a WiFi start or connection sets the power back.
        if (_txDbm != _applied || now - _appliedAt >= 5000) {
            _applied = _txDbm;
            _appliedAt = now;
            if (_io.setTxPower) _io.setTxPower(_txDbm);
        }
    }

    // ── Tables ───────────────────────────────────────────────────────────────
    Neighbor* _find(const uint8_t* mac) { return const_cast<Neighbor*>(_findC(mac)); }
    const Neighbor* _findC(const uint8_t* mac) const {
        for (const Neighbor& n : _nb)
            if (n.used && memcmp(n.mac, mac, 6) == 0) return &n;
        return nullptr;
    }

    Neighbor* _alloc(const uint8_t* mac, uint32_t now) {
        Neighbor* slot = nullptr;
        for (Neighbor& n : _nb)
            if (!n.used) {
                slot = &n;
                break;
            }
        if (!slot) {
            // Replace the stalest neighbour that is not the parent.
            uint32_t oldest = 0;
            for (uint8_t i = 0; i < kMaxNeighbors; i++) {
                if ((int8_t)i == _parent) continue;
                uint32_t age = now - _nb[i].heardMs;
                if (!slot || age > oldest) {
                    slot = &_nb[i];
                    oldest = age;
                }
            }
        }
        if (!slot) return nullptr;
        memset(slot, 0, sizeof(Neighbor));
        slot->used = true;
        memcpy(slot->mac, mac, 6);
        slot->hops = kNoHops;
        slot->cost = kNoCost;
        slot->heardMs = now;
        return slot;
    }

    void _expire(uint32_t now) {
        for (uint8_t i = 0; i < kMaxNeighbors; i++) {
            Neighbor& n = _nb[i];
            if (!n.used || (int8_t)i == _parent) continue;
            if (now - n.heardMs > 30000) n.used = false;
        }
    }

    Node* _node(const uint8_t* mac, uint32_t now) {
        Node* fresh = nullptr;
        Node* oldest = nullptr;
        for (Node& n : _nodes) {
            if (n.used && memcmp(n.mac, mac, 6) == 0) return &n;
            if (!n.used && !fresh) fresh = &n;
            if (n.used && (!oldest || now - n.heardMs > now - oldest->heardMs)) oldest = &n;
        }
        Node* s = fresh ? fresh : oldest;
        if (!s) return nullptr;
        memset(s, 0, sizeof(Node));
        s->used = true;
        memcpy(s->mac, mac, 6);
        s->hops = kNoHops;
        return s;
    }

    // ── Duplicates ───────────────────────────────────────────────────────────
    // FNV-1a over the frame without the hop counters, so every relayed copy
    // of one transmission hashes the same.
    static uint32_t _hashUp(const uint8_t* d, size_t len) {
        size_t skipA = SIZE_MAX, skipB = SIZE_MAX;
        if (d[0] == kMsgTopo) {
            skipA = offsetof(TopoMsg, fwd);
        } else {
            skipA = offsetof(game::QuizHeader, ttl);
            skipB = offsetof(game::QuizHeader, hops);
        }
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < len; i++) {
            if (i == skipA || i == skipB) continue;
            h ^= d[i];
            h *= 16777619u;
        }
        return h ? h : 1;
    }

    bool _seenRecently(uint32_t key, uint32_t now) const {
        for (const Seen& s : _seen)
            if (s.hash == key && now - s.at < kDedupMs) return true;
        return false;
    }

    void _remember(uint32_t key, uint32_t now) {
        _seen[_seenNext] = {key, now};
        _seenNext = (uint8_t)((_seenNext + 1) % kSeenSlots);
    }
};

}  // namespace meshnet
