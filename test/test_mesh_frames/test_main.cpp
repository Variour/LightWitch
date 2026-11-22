// Radio frames on the mesh: quiz frames (src/game/GameFrames.h), fleet frames
// (src/fleet/FleetFrames.h), their helpers and the receive ring.
#include <unity.h>

#include "fleet/FleetFrames.h"
#include "game/GameFrames.h"
#include "game/GameTypes.h"
#include "game/RxRing.h"

using namespace game;

void setUp() {}
void tearDown() {}

// ── Quiz frame layouts ───────────────────────────────────────────────────────

static void test_quiz_layouts_match_protocol() {
    TEST_ASSERT_EQUAL(12, sizeof(QuizHeader));
    TEST_ASSERT_EQUAL(16, sizeof(QuizInviteMsg));
    TEST_ASSERT_EQUAL(41, kQuizStateHeadLen);
    TEST_ASSERT_EQUAL(241, sizeof(QuizStateMsg));
    TEST_ASSERT_EQUAL(24, sizeof(QuizReportMsg));
    TEST_ASSERT_EQUAL(31, sizeof(QuizStatusMsg));
    TEST_ASSERT_EQUAL(22, sizeof(QuizAdmitMsg));
    TEST_ASSERT_EQUAL(20, sizeof(QuizReleaseMsg));
    TEST_ASSERT_EQUAL(17, sizeof(QuizChannelMsg));
    TEST_ASSERT_TRUE(kMaxFrameLen <= 250);
}

static void test_quiz_header_is_little_endian() {
    QuizReportMsg m{};
    m.h.type = kMsgQuizReport;
    m.h.origin = 0x0102;
    m.h.msgId = 0xA0B0;
    m.pressOffsetMs = -2;
    const uint8_t* b = (const uint8_t*)&m;
    TEST_ASSERT_EQUAL_HEX8(0x02, b[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[5]);
    TEST_ASSERT_EQUAL_HEX8(0xB0, b[6]);
    TEST_ASSERT_EQUAL_HEX8(0xA0, b[7]);
    size_t off = offsetof(QuizReportMsg, pressOffsetMs);
    TEST_ASSERT_EQUAL_HEX8(0xFE, b[off]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[off + 3]);
}

static void test_is_quiz_type_covers_37_to_43() {
    TEST_ASSERT_FALSE(isQuizType(36));
    for (uint8_t t = kMsgQuizInvite; t <= kMsgQuizChannel; t++) TEST_ASSERT_TRUE(isQuizType(t));
    TEST_ASSERT_FALSE(isQuizType(44));
}

// ── Quiz framing ─────────────────────────────────────────────────────────────

static void test_fixed_quiz_frames_need_exact_size() {
    struct {
        uint8_t type;
        size_t size;
    } cases[] = {
        {kMsgQuizInvite, sizeof(QuizInviteMsg)},   {kMsgQuizReport, sizeof(QuizReportMsg)},
        {kMsgQuizStatus, sizeof(QuizStatusMsg)},   {kMsgQuizAdmit, sizeof(QuizAdmitMsg)},
        {kMsgQuizRelease, sizeof(QuizReleaseMsg)}, {kMsgQuizChannel, sizeof(QuizChannelMsg)}};
    uint8_t buf[256] = {};
    for (auto& c : cases) {
        buf[0] = c.type;
        TEST_ASSERT_TRUE(quizFrameLenOk(buf, c.size));
        TEST_ASSERT_FALSE(quizFrameLenOk(buf, c.size - 1));
        TEST_ASSERT_FALSE(quizFrameLenOk(buf, c.size + 1));
    }
}

static void test_unknown_or_empty_quiz_frame_is_rejected() {
    uint8_t buf[32] = {};
    TEST_ASSERT_FALSE(quizFrameLenOk(buf, 0));
    buf[0] = 1;  // Presence
    TEST_ASSERT_FALSE(quizFrameLenOk(buf, sizeof(QuizInviteMsg)));
    buf[0] = 44;  // FleetStatus
    TEST_ASSERT_FALSE(quizFrameLenOk(buf, 20));
}

static void test_state_frame_length_follows_echo_count() {
    QuizStateMsg m{};
    m.h.type = kMsgQuizState;
    const uint8_t* d = (const uint8_t*)&m;

    m.echoCount = 0;
    TEST_ASSERT_TRUE(quizFrameLenOk(d, kQuizStateHeadLen));
    TEST_ASSERT_FALSE(quizFrameLenOk(d, kQuizStateHeadLen - 1));
    TEST_ASSERT_FALSE(quizFrameLenOk(d, kQuizStateHeadLen + 1));

    m.echoCount = 3;
    TEST_ASSERT_EQUAL(kQuizStateHeadLen + 3, stateFrameLen(m));
    TEST_ASSERT_TRUE(quizFrameLenOk(d, kQuizStateHeadLen + 3));
    TEST_ASSERT_FALSE(quizFrameLenOk(d, kQuizStateHeadLen + 2));

    m.echoCount = kEchoPageSize;
    TEST_ASSERT_TRUE(quizFrameLenOk(d, sizeof(QuizStateMsg)));

    m.echoCount = kEchoPageSize + 1;
    TEST_ASSERT_FALSE(quizFrameLenOk(d, kQuizStateHeadLen + kEchoPageSize + 1));
}

static void test_decode_state_zeroes_echo_tail() {
    QuizStateMsg m{};
    m.h.type = kMsgQuizState;
    m.roundId = 0x1234;
    m.echoCount = 2;
    m.echo[0] = 7;
    m.echo[1] = 8;
    m.echo[2] = 99;  // beyond echoCount: not on air
    QuizStateMsg out;
    memset(&out, 0xAA, sizeof(out));
    TEST_ASSERT_TRUE(decodeFrame((const uint8_t*)&m, stateFrameLen(m), kMsgQuizState, out));
    TEST_ASSERT_EQUAL_HEX16(0x1234, out.roundId);
    TEST_ASSERT_EQUAL(7, out.echo[0]);
    TEST_ASSERT_EQUAL(8, out.echo[1]);
    for (size_t i = 2; i < kEchoPageSize; i++) TEST_ASSERT_EQUAL(0, out.echo[i]);
}

static void test_decode_rejects_other_type_and_bad_length() {
    QuizAdmitMsg m{};
    m.h.type = kMsgQuizAdmit;
    m.number = 42;
    QuizAdmitMsg out;
    const uint8_t* d = (const uint8_t*)&m;
    TEST_ASSERT_TRUE(decodeFrame(d, sizeof(m), kMsgQuizAdmit, out));
    TEST_ASSERT_EQUAL(42, out.number);
    TEST_ASSERT_FALSE(decodeFrame(d, sizeof(m) - 1, kMsgQuizAdmit, out));
    QuizReleaseMsg rel;
    TEST_ASSERT_FALSE(decodeFrame(d, sizeof(m), kMsgQuizRelease, rel));
}

static void test_key_colors_pack_two_per_byte() {
    uint8_t kc[5] = {};
    for (uint8_t k = 0; k < kKeySlots; k++) setKeyColor(kc, k, (uint8_t)(k % kColorCount));
    for (uint8_t k = 0; k < kKeySlots; k++) TEST_ASSERT_EQUAL(k % kColorCount, getKeyColor(kc, k));
    TEST_ASSERT_EQUAL_HEX8(0x10, kc[0]);  // key 0 low nibble, key 1 high nibble
    setKeyColor(kc, 1, 0x1F);             // only the low 4 bits count
    TEST_ASSERT_EQUAL(0x0F, getKeyColor(kc, 1));
    TEST_ASSERT_EQUAL(0, getKeyColor(kc, 0));
}

// ── Helpers in GameTypes.h ───────────────────────────────────────────────────

static void test_seq16_newer_wraps() {
    TEST_ASSERT_TRUE(seq16Newer(2, 1));
    TEST_ASSERT_FALSE(seq16Newer(1, 1));
    TEST_ASSERT_FALSE(seq16Newer(1, 2));
    TEST_ASSERT_TRUE(seq16Newer(0, 0xFFFF));
    TEST_ASSERT_TRUE(seq16Newer(0x7FFF, 0));
    TEST_ASSERT_FALSE(seq16Newer(0x8000, 0));
}

static void test_seq8_newer_wraps() {
    TEST_ASSERT_TRUE(seq8Newer(1, 0));
    TEST_ASSERT_FALSE(seq8Newer(0, 0));
    TEST_ASSERT_TRUE(seq8Newer(0, 255));
    TEST_ASSERT_TRUE(seq8Newer(127, 0));
    TEST_ASSERT_FALSE(seq8Newer(128, 0));
}

static void test_mac_round_trip() {
    const uint8_t mac[6] = {0x02, 0x00, 0xAB, 0xcd, 0x0F, 0x11};
    char s[18];
    formatMac(mac, s);
    TEST_ASSERT_EQUAL_STRING("02:00:ab:cd:0f:11", s);
    uint8_t back[6];
    TEST_ASSERT_TRUE(parseMac("02:00:AB:cd:0f:11", back));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, back, 6);
}

static void test_parse_mac_rejects_other_shapes() {
    uint8_t out[6];
    TEST_ASSERT_FALSE(parseMac(nullptr, out));
    TEST_ASSERT_FALSE(parseMac("02:00:00:00:00:1", out));
    TEST_ASSERT_FALSE(parseMac("02-00-00-00-00-11", out));
    TEST_ASSERT_FALSE(parseMac("02:00:00:00:00:1g", out));
    TEST_ASSERT_FALSE(parseMac("02:00:00:00:00:110", out));
}

static void test_names_map_to_enums() {
    TEST_ASSERT_EQUAL((int)Key::Buzz, (int)keyFromName("buzz"));
    TEST_ASSERT_EQUAL((int)Key::Power, (int)keyFromName("power"));
    TEST_ASSERT_EQUAL((int)Key::None, (int)keyFromName("BUZZ"));
    TEST_ASSERT_EQUAL((int)Key::None, (int)keyFromName(nullptr));
    TEST_ASSERT_EQUAL((int)Color::Orange, indexOfName("orange", kColorNames, kColorCount));
    TEST_ASSERT_EQUAL(-1, indexOfName("pink", kColorNames, kColorCount));
    TEST_ASSERT_EQUAL((int)RoundType::Choice,
                      indexOfName("choice", kRoundTypeNames, kRoundTypeCount));
}

// ── Fleet frames ─────────────────────────────────────────────────────────────

static void test_fleet_layouts_match_protocol() {
    TEST_ASSERT_EQUAL(20, sizeof(fleet::FleetStatusMsg));
    TEST_ASSERT_EQUAL(11, fleet::kCommandHeadLen);
    TEST_ASSERT_TRUE(sizeof(fleet::FleetCommandMsg) <= 250);
    TEST_ASSERT_TRUE(fleet::isFleetType(44));
    TEST_ASSERT_TRUE(fleet::isFleetType(45));
    TEST_ASSERT_FALSE(fleet::isFleetType(43));
}

static void test_fleet_command_length_follows_target_count() {
    fleet::FleetCommandMsg m{};
    m.type = fleet::kMsgFleetCommand;
    m.proto = fleet::kProto;
    const uint8_t* d = (const uint8_t*)&m;

    m.targetCount = 0;
    TEST_ASSERT_EQUAL(fleet::kCommandHeadLen, fleet::commandFrameLen(m));
    TEST_ASSERT_TRUE(fleet::fleetFrameLenOk(d, fleet::kCommandHeadLen));
    TEST_ASSERT_FALSE(fleet::fleetFrameLenOk(d, fleet::kCommandHeadLen + 6));

    m.targetCount = 2;
    TEST_ASSERT_TRUE(fleet::fleetFrameLenOk(d, fleet::kCommandHeadLen + 12));
    TEST_ASSERT_FALSE(fleet::fleetFrameLenOk(d, fleet::kCommandHeadLen + 11));

    m.targetCount = fleet::kMaxTargets;
    TEST_ASSERT_TRUE(fleet::fleetFrameLenOk(d, sizeof(m)));
    m.targetCount = fleet::kMaxTargets + 1;
    TEST_ASSERT_FALSE(
        fleet::fleetFrameLenOk(d, fleet::kCommandHeadLen + (size_t)(fleet::kMaxTargets + 1) * 6));
}

static void test_fleet_decode_checks_proto_and_zeroes_targets() {
    fleet::FleetCommandMsg m{};
    m.type = fleet::kMsgFleetCommand;
    m.proto = fleet::kProto;
    m.nonce = 0xDEADBEEF;
    m.targetCount = 1;
    memset(m.targets[0], 0x11, 6);
    memset(m.targets[1], 0x22, 6);  // beyond targetCount: not on air
    fleet::FleetCommandMsg out;
    const uint8_t* d = (const uint8_t*)&m;
    size_t len = fleet::commandFrameLen(m);
    TEST_ASSERT_TRUE(fleet::decodeFleetFrame(d, len, fleet::kMsgFleetCommand, out));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, out.nonce);
    TEST_ASSERT_EQUAL_HEX8(0x11, out.targets[0][5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out.targets[1][0]);

    m.proto = fleet::kProto + 1;
    TEST_ASSERT_FALSE(fleet::decodeFleetFrame(d, len, fleet::kMsgFleetCommand, out));
}

static void test_fleet_status_needs_exact_size() {
    fleet::FleetStatusMsg m{};
    m.type = fleet::kMsgFleetStatus;
    m.proto = fleet::kProto;
    fleet::FleetStatusMsg out;
    const uint8_t* d = (const uint8_t*)&m;
    TEST_ASSERT_TRUE(fleet::decodeFleetFrame(d, sizeof(m), fleet::kMsgFleetStatus, out));
    TEST_ASSERT_FALSE(fleet::decodeFleetFrame(d, sizeof(m) - 1, fleet::kMsgFleetStatus, out));
    TEST_ASSERT_FALSE(fleet::decodeFleetFrame(d, sizeof(m), fleet::kMsgFleetCommand, out));
}

// ── Receive ring ─────────────────────────────────────────────────────────────

static void test_rx_ring_keeps_order_and_counts_overflow() {
    RxRing ring;
    TEST_ASSERT_TRUE(ring.allocate(2));
    uint8_t a[3] = {1, 2, 3}, b[1] = {4}, c[1] = {5};
    const uint8_t mac[6] = {9, 9, 9, 9, 9, 9};
    ring.push(a, sizeof(a), -40, mac);
    ring.push(b, sizeof(b), -50);
    ring.push(c, sizeof(c), -60);  // full
    TEST_ASSERT_EQUAL(1, ring.overflow());

    RxRing::Frame f;
    TEST_ASSERT_TRUE(ring.pop(f));
    TEST_ASSERT_EQUAL(3, f.len);
    TEST_ASSERT_EQUAL(-40, f.rssi);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, f.data, 3);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(mac, f.mac, 6);
    TEST_ASSERT_TRUE(ring.pop(f));
    TEST_ASSERT_EQUAL(4, f.data[0]);
    TEST_ASSERT_EQUAL(0, f.mac[0]);
    TEST_ASSERT_FALSE(ring.pop(f));

    ring.push(c, sizeof(c), -60);  // wraps around
    TEST_ASSERT_TRUE(ring.pop(f));
    TEST_ASSERT_EQUAL(5, f.data[0]);
}

static void test_rx_ring_drops_empty_and_oversized_frames() {
    RxRing ring;
    uint8_t big[kMaxFrameLen + 1] = {};
    ring.push(big, 1, 0);  // not allocated yet
    TEST_ASSERT_EQUAL(1, ring.overflow());
    TEST_ASSERT_TRUE(ring.allocate(4));
    ring.push(big, 0, 0);
    ring.push(big, sizeof(big), 0);
    RxRing::Frame f;
    TEST_ASSERT_FALSE(ring.pop(f));
    TEST_ASSERT_EQUAL(1, ring.overflow());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_quiz_layouts_match_protocol);
    RUN_TEST(test_quiz_header_is_little_endian);
    RUN_TEST(test_is_quiz_type_covers_37_to_43);
    RUN_TEST(test_fixed_quiz_frames_need_exact_size);
    RUN_TEST(test_unknown_or_empty_quiz_frame_is_rejected);
    RUN_TEST(test_state_frame_length_follows_echo_count);
    RUN_TEST(test_decode_state_zeroes_echo_tail);
    RUN_TEST(test_decode_rejects_other_type_and_bad_length);
    RUN_TEST(test_key_colors_pack_two_per_byte);
    RUN_TEST(test_seq16_newer_wraps);
    RUN_TEST(test_seq8_newer_wraps);
    RUN_TEST(test_mac_round_trip);
    RUN_TEST(test_parse_mac_rejects_other_shapes);
    RUN_TEST(test_names_map_to_enums);
    RUN_TEST(test_fleet_layouts_match_protocol);
    RUN_TEST(test_fleet_command_length_follows_target_count);
    RUN_TEST(test_fleet_decode_checks_proto_and_zeroes_targets);
    RUN_TEST(test_fleet_status_needs_exact_size);
    RUN_TEST(test_rx_ring_keeps_order_and_counts_overflow);
    RUN_TEST(test_rx_ring_drops_empty_and_oversized_frames);
    return UNITY_END();
}
