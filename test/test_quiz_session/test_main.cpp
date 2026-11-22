// Quiz session: bridge (src/game/Bridge.h) and participants
// (src/game/Participant.h) talking over a simulated radio.
#include <unity.h>

#include <deque>
#include <memory>
#include <vector>

#include "game/Bridge.h"
#include "game/Participant.h"

using namespace game;

// ── Simulated radio ──────────────────────────────────────────────────────────

struct Air {
    struct Frame {
        int from;  // -1 = bridge, otherwise participant index
        uint8_t channel;
        std::vector<uint8_t> data;
    };
    std::deque<Frame> queue;
    std::vector<Frame> log;  // every frame sent, for inspection
    bool drop = false;       // frames sent while set never arrive
};

struct Device {
    Participant p;
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
    uint8_t channel = 1;
    uint16_t storedNumber = 0;
    uint16_t storedSession = 0;
    uint8_t storedChannel = 0;
    int storeSessionCalls = 0;
    int enterQuietCalls = 0;
    int restarts = 0;
};

struct JoinSeen {
    uint8_t mac[6];
    uint16_t n;
    bool pressed;
    bool rejoin;
};

struct Recorder : Bridge::Listener {
    std::vector<JoinSeen> joins;
    std::vector<uint16_t> admitted;
    std::vector<uint16_t> admitTimeouts;
    int ended = 0;
    void onJoin(const Bridge::JoinInfo& j) override {
        JoinSeen s;
        memcpy(s.mac, j.mac, 6);
        s.n = j.n;
        s.pressed = j.pressed;
        s.rejoin = j.rejoin;
        joins.push_back(s);
    }
    void onAdmitted(const ReqId&, const uint8_t*, uint16_t n) override { admitted.push_back(n); }
    void onAdmitTimeout(const ReqId&, const uint8_t*, uint16_t n) override {
        admitTimeouts.push_back(n);
    }
    void onSessionEnded(const ReqId&) override { ended++; }
};

struct World {
    Air air;
    std::unique_ptr<Bridge> bridge;
    Recorder rec;
    uint8_t bridgeChannel = 1;
    int bridgeQuiet = 0;
    std::vector<std::unique_ptr<Device>> devs;
    uint32_t now = 1000;

    World() { startBridge(); }

    // A fresh bridge, as after a reboot of the bridge device.
    void startBridge() {
        bridge.reset(new Bridge());
        Bridge::Io io;
        io.send = [this](const uint8_t* d, size_t len) { _emit(-1, bridgeChannel, d, len); };
        io.setChannel = [this](uint8_t ch) { bridgeChannel = ch; };
        io.enterQuiet = [this]() { bridgeQuiet++; };
        bridge->begin(io, &rec);
    }

    Device& add(uint8_t macTail, uint16_t number = 0, uint16_t storedSession = 0,
                uint8_t storedChannel = 0) {
        devs.emplace_back(new Device());
        Device& d = *devs.back();
        int idx = (int)devs.size() - 1;
        d.mac[5] = macTail;
        d.storedNumber = number;
        Participant::Io io;
        io.send = [this, idx](const uint8_t* b, size_t len) {
            _emit(idx, devs[idx]->channel, b, len);
        };
        io.setChannel = [&d](uint8_t ch) { d.channel = ch; };
        io.randomRange = [](uint32_t lo, uint32_t hi) { return lo + (hi - lo) / 2; };
        io.storeNumber = [&d](uint16_t n) { d.storedNumber = n; };
        io.storeSession = [&d](uint16_t s, uint8_t ch) {
            d.storedSession = s;
            d.storedChannel = ch;
            d.storeSessionCalls++;
        };
        io.enterQuiet = [&d]() { d.enterQuietCalls++; };
        io.restart = [&d]() { d.restarts++; };
        io.health = []() {
            Participant::Health h;
            h.battery = 80;
            return h;
        };
        io.fwBuild = []() { return 0x12345678u; };
        d.p.setCanJoin(true);
        d.p.begin(io, d.mac, number, storedSession, storedChannel, now);
        return d;
    }

    // Delivers queued frames until the air is quiet (replies included).
    void pump() {
        while (!air.queue.empty()) {
            Air::Frame f = air.queue.front();
            air.queue.pop_front();
            if (f.from < 0) {
                for (auto& d : devs)
                    if (d->channel == f.channel)
                        d->p.onFrame(f.data.data(), f.data.size(), -50, now);
            } else if (bridgeChannel == f.channel && !bridge->offChannel()) {
                bridge->onFrame(f.data.data(), f.data.size(), -55, now);
            }
        }
    }

    void tick() {
        bridge->tick(now);
        pump();
        for (auto& d : devs) d->p.tick(now);
        pump();
    }

    // Advances time in 10 ms steps, ticking everyone.
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            tick();
        }
    }

    void press(Device& d, Key k) {
        d.p.onKey(k, true, now, now);
        pump();
        d.p.onKey(k, false, now, now);
        pump();
    }

    size_t countSent(int from, uint8_t type) const {
        size_t n = 0;
        for (auto& f : air.log)
            if (f.from == from && f.data[0] == type) n++;
        return n;
    }

   private:
    void _emit(int from, uint8_t ch, const uint8_t* d, size_t len) {
        Air::Frame f{from, ch, std::vector<uint8_t>(d, d + len)};
        air.log.push_back(f);
        if (!air.drop) air.queue.push_back(f);
    }
};

static Bridge::RoundCmd roundCmd(uint16_t id, RoundType type, Phase phase, uint16_t keyMask) {
    Bridge::RoundCmd c;
    c.round = id;
    c.type = type;
    c.phase = phase;
    c.keyMask = keyMask;
    for (uint8_t k = 0; k < kKeySlots; k++) setKeyColor(c.keyColor, k, (uint8_t)Color::Blue);
    return c;
}

static constexpr uint16_t kBuzzMask = 1u << (uint8_t)Key::Buzz;
static constexpr uint16_t kArrowMask = (1u << (uint8_t)Key::Up) | (1u << (uint8_t)Key::Down) |
                                       (1u << (uint8_t)Key::Left) | (1u << (uint8_t)Key::Right);

// Opens session 77 on channel 6 with login and brings d in as number n.
static void admitDevice(World& w, Device& d, uint16_t n) {
    if (!w.bridge->active())
        TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->session(77, 6, 0, true, w.now));
    // Invites reach devices on foreign channels during the bridge's hops.
    for (int i = 0; i < 500 && !d.p.inviteVisible(w.now); i++) w.run(10);
    TEST_ASSERT_TRUE(d.p.inviteVisible(w.now));
    size_t joinsBefore = w.rec.joins.size();
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL((int)Participant::State::Joining, (int)d.p.state());
    for (int i = 0; i < 100 && w.rec.joins.size() == joinsBefore; i++) w.run(10);
    TEST_ASSERT_TRUE(w.rec.joins.size() > joinsBefore);
    TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->admit(d.mac, n, ReqId{}, w.now));
    w.pump();
    w.run(20);
    TEST_ASSERT_EQUAL((int)Participant::State::Member, (int)d.p.state());
}

void setUp() {}
void tearDown() {}

// ── Join ─────────────────────────────────────────────────────────────────────

static void test_join_from_invite_to_member() {
    World w;
    Device& d = w.add(0x11);
    TEST_ASSERT_EQUAL((int)Participant::State::Normal, (int)d.p.state());
    TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->session(77, 6, 0, true, w.now));
    TEST_ASSERT_EQUAL(1, w.bridgeQuiet);

    admitDevice(w, d, 7);
    TEST_ASSERT_EQUAL(1, d.enterQuietCalls);
    TEST_ASSERT_EQUAL(6, d.channel);
    TEST_ASSERT_EQUAL(1, w.rec.joins.size());
    TEST_ASSERT_TRUE(w.rec.joins[0].pressed);
    TEST_ASSERT_FALSE(w.rec.joins[0].rejoin);
    TEST_ASSERT_EQUAL(0, w.rec.joins[0].n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(d.mac, w.rec.joins[0].mac, 6);

    TEST_ASSERT_EQUAL(7, d.p.number());
    TEST_ASSERT_EQUAL(7, d.storedNumber);
    TEST_ASSERT_EQUAL(77, d.storedSession);
    TEST_ASSERT_EQUAL(6, d.storedChannel);
    TEST_ASSERT_EQUAL(1, w.rec.admitted.size());
    TEST_ASSERT_EQUAL(7, w.rec.admitted[0]);
    const Bridge::Device* bd = w.bridge->device(7);
    TEST_ASSERT_NOT_NULL(bd);
    TEST_ASSERT_TRUE(bd->macKnown);
    TEST_ASSERT_EQUAL(80, bd->battery);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, bd->fw);
}

static void test_buzz_without_invite_does_nothing() {
    World w;
    Device& d = w.add(0x11);
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL((int)Participant::State::Normal, (int)d.p.state());
    TEST_ASSERT_EQUAL(0, w.air.log.size());
}

static void test_device_that_cannot_join_ignores_invites() {
    World w;
    Device& d = w.add(0x11);
    d.p.setCanJoin(false);
    d.channel = 6;
    w.bridge->session(77, 6, 0, true, w.now);
    w.run(100);
    TEST_ASSERT_FALSE(d.p.inviteVisible(w.now));
}

static void test_invite_for_other_fleet_is_ignored() {
    World w;
    Device& d = w.add(0x11);
    d.channel = 6;
    w.bridge->session(77, 6, /*fleet=*/5, true, w.now);
    w.run(100);
    TEST_ASSERT_FALSE(d.p.inviteVisible(w.now));
}

static void test_invite_expires_after_10_s() {
    World w;
    Device& d = w.add(0x11);
    d.channel = 6;
    w.bridge->session(77, 6, 0, true, w.now);
    w.run(100);
    TEST_ASSERT_TRUE(d.p.inviteVisible(w.now));
    w.bridge->session(77, 6, 0, /*login=*/false, w.now);
    w.run(kInviteVisibleMs - 200);
    TEST_ASSERT_TRUE(d.p.inviteVisible(w.now));
    w.run(300);
    TEST_ASSERT_FALSE(d.p.inviteVisible(w.now));
}

static void test_join_gives_up_after_30_s() {
    World w;
    Device& d = w.add(0x11);
    d.channel = 6;
    w.bridge->session(77, 6, 0, true, w.now);
    w.run(100);
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL((int)Participant::State::Joining, (int)d.p.state());
    w.run(kJoinTimeoutMs);
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)d.p.state());
    w.run(kLeaveRestartDelayMs + 20);
    TEST_ASSERT_EQUAL(1, d.restarts);
}

static void test_admit_times_out_without_status() {
    World w;
    w.bridge->session(77, 6, 0, true, w.now);
    const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0x99};
    TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->admit(mac, 3, ReqId{}, w.now));
    w.run(kAdmitTimeoutMs + 20);
    TEST_ASSERT_EQUAL(1, w.rec.admitTimeouts.size());
    TEST_ASSERT_EQUAL(3, w.rec.admitTimeouts[0]);
}

static void test_commands_need_a_session() {
    World w;
    const uint8_t mac[6] = {};
    bool changed;
    TEST_ASSERT_EQUAL((int)Bridge::Err::NoSession, (int)w.bridge->admit(mac, 1, ReqId{}, w.now));
    TEST_ASSERT_EQUAL((int)Bridge::Err::NoSession, (int)w.bridge->release(mac, w.now));
    TEST_ASSERT_EQUAL((int)Bridge::Err::NoSession, (int)w.bridge->end(ReqId{}, w.now));
    TEST_ASSERT_EQUAL(
        (int)Bridge::Err::NoSession,
        (int)w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed));
    w.bridge->session(77, 6, 0, true, w.now);
    TEST_ASSERT_EQUAL((int)Bridge::Err::SessionActive,
                      (int)w.bridge->session(78, 6, 0, true, w.now));
}

// ── Rounds ───────────────────────────────────────────────────────────────────

static void test_race_round_press_is_reported_and_confirmed() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    TEST_ASSERT_EQUAL(
        (int)Bridge::Err::None,
        (int)w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed));
    TEST_ASSERT_TRUE(changed);
    w.run(20);
    TEST_ASSERT_TRUE(d.p.round().known);
    TEST_ASSERT_EQUAL((int)RoundType::Race, (int)d.p.round().type);
    TEST_ASSERT_TRUE(d.p.keyFree(Key::Buzz, w.now));
    TEST_ASSERT_FALSE(d.p.keyFree(Key::Up, w.now));

    w.run(100);
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(1, d.p.value());
    const Bridge::Device* bd = w.bridge->device(7);
    TEST_ASSERT_TRUE(bd->hasValue);
    TEST_ASSERT_EQUAL(1, bd->value);
    TEST_ASSERT_EQUAL(1, bd->arr);
    TEST_ASSERT_INT_WITHIN(20, 120, bd->t);
    TEST_ASSERT_FALSE(bd->late);
    TEST_ASSERT_FALSE(d.p.confirmed());

    w.run(300);  // next state frame carries the echo
    TEST_ASSERT_TRUE(d.p.confirmed());
    size_t reports = w.countSent(0, kMsgQuizReport);
    w.run(3000);
    TEST_ASSERT_EQUAL(reports, w.countSent(0, kMsgQuizReport));

    // A second press in a race changes nothing.
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(reports, w.countSent(0, kMsgQuizReport));
}

static void test_race_keys_wait_for_announced_start() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    Bridge::RoundCmd c = roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask);
    c.hasOpenIn = true;
    c.openInMs = 1000;
    bool changed;
    w.bridge->round(c, w.now, changed);
    w.run(100);
    TEST_ASSERT_TRUE(d.p.beforeStart(w.now));
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(kNoValue, d.p.value());
    w.run(1000);
    TEST_ASSERT_FALSE(d.p.beforeStart(w.now));
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(1, d.p.value());
}

static void test_announced_round_takes_no_input() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Announced, kBuzzMask), w.now, changed);
    w.run(100);
    TEST_ASSERT_TRUE(d.p.round().known);
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(kNoValue, d.p.value());
}

static void test_choice_last_press_counts() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(2, RoundType::Choice, Phase::Open, kArrowMask), w.now, changed);
    w.run(100);
    w.press(d, Key::Down);
    TEST_ASSERT_EQUAL(3, d.p.value());
    w.press(d, Key::Left);
    TEST_ASSERT_EQUAL(4, d.p.value());
    w.press(d, Key::Buzz);  // not part of a choice
    TEST_ASSERT_EQUAL(4, d.p.value());
    TEST_ASSERT_EQUAL(4, w.bridge->device(7)->value);
    TEST_ASSERT_EQUAL(2, w.bridge->device(7)->vseq);
}

static void test_choice_answer_mapping() {
    TEST_ASSERT_EQUAL(1, Participant::choiceAnswer(Key::Up));
    TEST_ASSERT_EQUAL(2, Participant::choiceAnswer(Key::Right));
    TEST_ASSERT_EQUAL(3, Participant::choiceAnswer(Key::Down));
    TEST_ASSERT_EQUAL(4, Participant::choiceAnswer(Key::Left));
    TEST_ASSERT_EQUAL(0, Participant::choiceAnswer(Key::Buzz));
}

static void test_scale_starts_in_the_middle_and_sends_after_150_ms() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    Bridge::RoundCmd c = roundCmd(3, RoundType::Scale, Phase::Open, kArrowMask);
    c.min = 1;
    c.max = 10;
    bool changed;
    w.bridge->round(c, w.now, changed);
    w.run(100);

    w.press(d, Key::Up);
    TEST_ASSERT_EQUAL(6, d.p.value());  // 1 + 10 / 2
    size_t reports = w.countSent(0, kMsgQuizReport);
    w.run(kScaleSendDelayMs - 20);
    TEST_ASSERT_EQUAL(reports, w.countSent(0, kMsgQuizReport));
    w.run(40);
    TEST_ASSERT_EQUAL(reports + 1, w.countSent(0, kMsgQuizReport));
    TEST_ASSERT_EQUAL(6, w.bridge->device(7)->value);

    for (int i = 0; i < 8; i++) w.press(d, Key::Up);
    TEST_ASSERT_EQUAL(10, d.p.value());  // stops at max
    for (int i = 0; i < 12; i++) w.press(d, Key::Down);
    TEST_ASSERT_EQUAL(1, d.p.value());  // stops at min
}

static void test_scale_first_down_tap_rounds_down() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    Bridge::RoundCmd c = roundCmd(3, RoundType::Scale, Phase::Open, kArrowMask);
    c.min = 1;
    c.max = 10;
    bool changed;
    w.bridge->round(c, w.now, changed);
    w.run(100);
    w.press(d, Key::Down);
    TEST_ASSERT_EQUAL(5, d.p.value());  // 1 + 9 / 2
}

static void test_round_cannot_change_shape_or_go_back() {
    World w;
    w.bridge->session(77, 6, 0, true, w.now);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    TEST_ASSERT_EQUAL((int)Bridge::Err::RoundFixed,
                      (int)w.bridge->round(roundCmd(1, RoundType::Choice, Phase::Open, kBuzzMask),
                                           w.now, changed));
    TEST_ASSERT_EQUAL(
        (int)Bridge::Err::BadPhase,
        (int)w.bridge->round(roundCmd(1, RoundType::Race, Phase::Announced, kBuzzMask), w.now,
                             changed));
    TEST_ASSERT_EQUAL(
        (int)Bridge::Err::None,
        (int)w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed));
    TEST_ASSERT_FALSE(changed);
    uint16_t seq = w.bridge->stateInfo(w.now).seq;
    TEST_ASSERT_EQUAL((int)Bridge::Err::None,
                      (int)w.bridge->round(roundCmd(1, RoundType::Race, Phase::Closed, kBuzzMask),
                                           w.now, changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL(seq + 1, w.bridge->stateInfo(w.now).seq);
}

static void test_close_and_winner_reach_participant() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    w.run(100);
    w.press(d, Key::Buzz);
    w.run(50);
    Bridge::RoundCmd c = roundCmd(1, RoundType::Race, Phase::Closed, kBuzzMask);
    c.winner = 7;
    w.bridge->round(c, w.now, changed);
    w.run(100);
    TEST_ASSERT_EQUAL((int)Phase::Closed, (int)d.p.round().phase);
    TEST_ASSERT_EQUAL(7, d.p.round().winner);
    Bridge::StateInfo s = w.bridge->stateInfo(w.now);
    TEST_ASSERT_TRUE(s.closedValid);
    TEST_ASSERT_INT_WITHIN(20, 160, s.closedAtMs);
}

static void test_press_after_close_is_marked_late() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    w.run(100);
    // The bridge closes while the device has not heard of it yet.
    w.air.drop = true;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Closed, kBuzzMask), w.now, changed);
    w.run(20);
    w.air.drop = false;
    w.press(d, Key::Buzz);
    TEST_ASSERT_TRUE(w.bridge->device(7)->hasValue);
    TEST_ASSERT_TRUE(w.bridge->device(7)->late);
}

static void test_new_round_clears_values() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    w.run(100);
    w.press(d, Key::Buzz);
    TEST_ASSERT_TRUE(w.bridge->device(7)->hasValue);
    TEST_ASSERT_TRUE(w.bridge->isNewRound(2));
    w.bridge->round(roundCmd(2, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    TEST_ASSERT_FALSE(w.bridge->device(7)->hasValue);
    w.run(100);
    TEST_ASSERT_EQUAL(2, d.p.round().id);
    TEST_ASSERT_EQUAL(kNoValue, d.p.value());
}

static void test_lost_report_is_resent_until_echoed() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    w.run(100);
    w.air.drop = true;
    w.press(d, Key::Buzz);
    w.air.drop = false;
    TEST_ASSERT_FALSE(w.bridge->device(7)->hasValue);
    w.run(kRetryMaxMs + 20);
    TEST_ASSERT_TRUE(w.bridge->device(7)->hasValue);
    TEST_ASSERT_EQUAL(2, w.countSent(0, kMsgQuizReport));
    w.run(300);
    TEST_ASSERT_TRUE(d.p.confirmed());
}

static void test_stale_value_seq_is_ignored() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    bool changed;
    w.bridge->round(roundCmd(2, RoundType::Choice, Phase::Open, kArrowMask), w.now, changed);
    w.run(100);
    w.press(d, Key::Up);
    QuizReportMsg old{};
    for (auto& f : w.air.log)
        if (f.from == 0 && f.data[0] == kMsgQuizReport) memcpy(&old, f.data.data(), sizeof(old));
    w.press(d, Key::Down);
    TEST_ASSERT_EQUAL(3, w.bridge->device(7)->value);
    // The first report arrives again, late.
    w.bridge->onFrame((const uint8_t*)&old, sizeof(old), -50, w.now);
    TEST_ASSERT_EQUAL(3, w.bridge->device(7)->value);
}

static void test_bridge_restart_makes_participant_report_again() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    w.bridge->session(77, 6, 0, false, w.now);  // login closed: no invite hops
    Bridge::RoundCmd c = roundCmd(2, RoundType::Choice, Phase::Open, kArrowMask);
    bool changed;
    w.bridge->round(c, w.now, changed);
    w.run(100);
    w.press(d, Key::Right);
    c.correct = 2;  // same phase, new content: stateSeq 2
    w.bridge->round(c, w.now, changed);
    w.run(300);
    TEST_ASSERT_TRUE(d.p.confirmed());
    TEST_ASSERT_EQUAL(2, d.p.round().seq);

    // A rebooted bridge gets the session and round again from the console.
    // Its stateSeq starts over, so the device reports its value again.
    w.startBridge();
    w.bridge->session(77, 6, 0, false, w.now);
    w.bridge->round(roundCmd(2, RoundType::Choice, Phase::Open, kArrowMask), w.now, changed);
    w.run(20);
    TEST_ASSERT_EQUAL(1, d.p.round().seq);
    TEST_ASSERT_FALSE(d.p.confirmed());
    w.run(kRetryMaxMs + 20);
    const Bridge::Device* bd = w.bridge->device(7);
    TEST_ASSERT_NOT_NULL(bd);
    TEST_ASSERT_TRUE(bd->hasValue);
    TEST_ASSERT_EQUAL(2, bd->value);
    w.run(300);
    TEST_ASSERT_TRUE(d.p.confirmed());
}

static void test_echo_without_value_makes_participant_resend() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    w.bridge->session(77, 6, 0, false, w.now);
    bool changed;
    w.bridge->round(roundCmd(2, RoundType::Choice, Phase::Open, kArrowMask), w.now, changed);
    w.run(100);
    w.press(d, Key::Up);
    w.run(300);
    TEST_ASSERT_TRUE(d.p.confirmed());
    // A status from another MAC for number 7 drops the value the bridge held.
    QuizStatusMsg st{};
    st.h.type = kMsgQuizStatus;
    st.h.proto = kProto;
    st.h.origin = 7;
    st.session = 77;
    const uint8_t other[6] = {0x02, 0, 0, 0, 0, 0x99};
    memcpy(st.mac, other, 6);
    w.bridge->onFrame((const uint8_t*)&st, sizeof(st), -50, w.now);
    TEST_ASSERT_FALSE(w.bridge->device(7)->hasValue);
    w.run(1100);  // the echo shows no value, the device resends
    TEST_ASSERT_TRUE(w.bridge->device(7)->hasValue);
    TEST_ASSERT_EQUAL(1, w.bridge->device(7)->value);
}

// ── Several devices ──────────────────────────────────────────────────────────

static void test_two_devices_arrival_order() {
    World w;
    Device& a = w.add(0x11);
    Device& b = w.add(0x22);
    admitDevice(w, a, 1);
    admitDevice(w, b, 2);
    bool changed;
    w.bridge->round(roundCmd(1, RoundType::Race, Phase::Open, kBuzzMask), w.now, changed);
    w.run(100);
    w.press(b, Key::Buzz);
    w.run(30);
    w.press(a, Key::Buzz);
    w.run(kRetryMaxMs);  // a report sent during an invite hop arrives on the retry
    TEST_ASSERT_EQUAL(1, w.bridge->device(2)->arr);
    TEST_ASSERT_EQUAL(2, w.bridge->device(1)->arr);
    TEST_ASSERT_TRUE(w.bridge->device(1)->t > w.bridge->device(2)->t);
    w.run(300);
    TEST_ASSERT_TRUE(a.p.confirmed());
    TEST_ASSERT_TRUE(b.p.confirmed());
}

static void test_member_buzz_during_login_reports_pressed() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    w.run(1100);  // an invite on the session channel
    TEST_ASSERT_TRUE(d.p.loginOpen(w.now));
    size_t joins = w.rec.joins.size();
    w.press(d, Key::Buzz);
    TEST_ASSERT_EQUAL(joins + 1, w.rec.joins.size());
    TEST_ASSERT_TRUE(w.rec.joins.back().pressed);
    TEST_ASSERT_EQUAL(7, w.rec.joins.back().n);
}

// ── Leaving ──────────────────────────────────────────────────────────────────

static void test_release_sends_device_back_to_normal() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->release(d.mac, w.now));
    w.pump();
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)d.p.state());
    TEST_ASSERT_EQUAL(0, d.storedSession);
    TEST_ASSERT_FALSE(w.bridge->device(7)->macKnown);
    w.run(kLeaveRestartDelayMs + 20);
    TEST_ASSERT_EQUAL(1, d.restarts);
}

static void test_end_releases_everyone_and_reports_end() {
    World w;
    Device& a = w.add(0x11);
    Device& b = w.add(0x22);
    admitDevice(w, a, 1);
    admitDevice(w, b, 2);
    TEST_ASSERT_EQUAL((int)Bridge::Err::None, (int)w.bridge->end(ReqId{}, w.now));
    TEST_ASSERT_TRUE(w.bridge->ending());
    w.run(20);
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)a.p.state());
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)b.p.state());
    TEST_ASSERT_EQUAL(0, w.rec.ended);
    w.run(kEndReleaseMs);
    TEST_ASSERT_EQUAL(1, w.rec.ended);
    TEST_ASSERT_FALSE(w.bridge->active());
}

static void test_buzz_and_enter_held_3_s_leaves() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    d.p.onKey(Key::Buzz, true, w.now, w.now);
    d.p.onKey(Key::Enter, true, w.now, w.now);
    w.run(kLeaveComboMs - 100);
    TEST_ASSERT_EQUAL((int)Participant::State::Member, (int)d.p.state());
    w.run(200);
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)d.p.state());
}

static void test_silent_bridge_starts_channel_search() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    w.air.drop = true;
    w.run(kNoGatewaySearchMs + 100);
    TEST_ASSERT_TRUE(d.p.searching());
    TEST_ASSERT_EQUAL(7, d.channel);  // one dwell past channel 6
    w.air.drop = false;
    // Search wraps around and finds the bridge again on channel 6.
    w.run(kSearchDwellMs * kMaxChannel);
    TEST_ASSERT_FALSE(d.p.searching());
    TEST_ASSERT_EQUAL(6, d.channel);
}

static void test_channel_change_is_announced_and_followed() {
    World w;
    Device& d = w.add(0x11);
    admitDevice(w, d, 7);
    w.bridge->session(77, 11, 0, false, w.now);
    TEST_ASSERT_EQUAL(11, w.bridge->channel());
    w.run(100);
    TEST_ASSERT_EQUAL(6, d.channel);
    w.run(kChannelAnnounceMs);
    TEST_ASSERT_EQUAL(11, w.bridgeChannel);
    TEST_ASSERT_EQUAL(11, d.channel);
    TEST_ASSERT_EQUAL(11, d.storedChannel);
}

// ── Restart mid-session ──────────────────────────────────────────────────────

static void test_quiet_boot_needs_session_and_valid_channel() {
    TEST_ASSERT_FALSE(Participant::wantsQuietBoot(0, 6));
    TEST_ASSERT_FALSE(Participant::wantsQuietBoot(77, 0));
    TEST_ASSERT_FALSE(Participant::wantsQuietBoot(77, 14));
    TEST_ASSERT_TRUE(Participant::wantsQuietBoot(77, 13));
}

static void test_rejoin_after_reboot() {
    World w;
    w.bridge->session(77, 6, 0, false, w.now);
    Device& d = w.add(0x11, /*number=*/7, /*storedSession=*/77, /*storedChannel=*/6);
    TEST_ASSERT_EQUAL((int)Participant::State::RejoinListen, (int)d.p.state());
    TEST_ASSERT_EQUAL(6, d.channel);
    w.run(1100);
    TEST_ASSERT_EQUAL((int)Participant::State::Joining, (int)d.p.state());
    TEST_ASSERT_TRUE(w.rec.joins.size() >= 1);
    TEST_ASSERT_TRUE(w.rec.joins[0].rejoin);
    TEST_ASSERT_FALSE(w.rec.joins[0].pressed);
    TEST_ASSERT_EQUAL(7, w.rec.joins[0].n);
    w.bridge->admit(d.mac, 7, ReqId{}, w.now);
    w.run(20);
    TEST_ASSERT_EQUAL((int)Participant::State::Member, (int)d.p.state());
}

static void test_rejoin_listen_gives_up_after_10_s() {
    World w;
    Device& d = w.add(0x11, 7, 77, 6);
    w.run(kRejoinListenMs + 20);
    TEST_ASSERT_EQUAL((int)Participant::State::Leaving, (int)d.p.state());
    TEST_ASSERT_EQUAL(0, d.storedSession);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_join_from_invite_to_member);
    RUN_TEST(test_buzz_without_invite_does_nothing);
    RUN_TEST(test_device_that_cannot_join_ignores_invites);
    RUN_TEST(test_invite_for_other_fleet_is_ignored);
    RUN_TEST(test_invite_expires_after_10_s);
    RUN_TEST(test_join_gives_up_after_30_s);
    RUN_TEST(test_admit_times_out_without_status);
    RUN_TEST(test_commands_need_a_session);
    RUN_TEST(test_race_round_press_is_reported_and_confirmed);
    RUN_TEST(test_race_keys_wait_for_announced_start);
    RUN_TEST(test_announced_round_takes_no_input);
    RUN_TEST(test_choice_last_press_counts);
    RUN_TEST(test_choice_answer_mapping);
    RUN_TEST(test_scale_starts_in_the_middle_and_sends_after_150_ms);
    RUN_TEST(test_scale_first_down_tap_rounds_down);
    RUN_TEST(test_round_cannot_change_shape_or_go_back);
    RUN_TEST(test_close_and_winner_reach_participant);
    RUN_TEST(test_press_after_close_is_marked_late);
    RUN_TEST(test_new_round_clears_values);
    RUN_TEST(test_lost_report_is_resent_until_echoed);
    RUN_TEST(test_stale_value_seq_is_ignored);
    RUN_TEST(test_bridge_restart_makes_participant_report_again);
    RUN_TEST(test_echo_without_value_makes_participant_resend);
    RUN_TEST(test_two_devices_arrival_order);
    RUN_TEST(test_member_buzz_during_login_reports_pressed);
    RUN_TEST(test_release_sends_device_back_to_normal);
    RUN_TEST(test_end_releases_everyone_and_reports_end);
    RUN_TEST(test_buzz_and_enter_held_3_s_leaves);
    RUN_TEST(test_silent_bridge_starts_channel_search);
    RUN_TEST(test_channel_change_is_announced_and_followed);
    RUN_TEST(test_quiet_boot_needs_session_and_valid_channel);
    RUN_TEST(test_rejoin_after_reboot);
    RUN_TEST(test_rejoin_listen_gives_up_after_10_s);
    return UNITY_END();
}
