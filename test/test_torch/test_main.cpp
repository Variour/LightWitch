// Torch spreading: when a lit torch's beacons light an out torch
// (src/torch/TorchSpread.h, docs/torch.md).
#include <unity.h>

#include "torch/TorchSpread.h"

using namespace torch;

static const uint8_t kMacA[6] = {0x02, 0, 0, 0, 0, 0xA1};
static const uint8_t kMacB[6] = {0x02, 0, 0, 0, 0, 0xB2};
static const int8_t kClose = IGNITE_RSSI_DBM;
static const int8_t kFar = IGNITE_RSSI_DBM - 10;
static const uint8_t kGroup1 = 1u << 1;

// Beacons from mac every BEACON_INTERVAL_MS in [from, to].
static void beacons(Spread& s, const uint8_t* mac, uint8_t mask, int8_t rssi, uint32_t from,
                    uint32_t to) {
    for (uint32_t t = from; t <= to; t += BEACON_INTERVAL_MS) s.onBeacon(mac, mask, rssi, t);
}

void setUp() {}
void tearDown() {}

void test_no_beacon_no_fire() {
    Spread s;
    TEST_ASSERT_FALSE(s.shouldIgnite(1, 0, 10000));
}

void test_close_flame_lights_after_hold() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 11400);
    TEST_ASSERT_FALSE(s.shouldIgnite(1, 0, 11400));
    beacons(s, kMacA, kGroup1, kClose, 11600, 11600);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, 0, 11600));
}

void test_other_group_does_not_spread() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 13000);
    TEST_ASSERT_FALSE(s.shouldIgnite(2, 0, 13000));
}

void test_far_flame_does_not_spread() {
    Spread s;
    beacons(s, kMacA, kGroup1, kFar, 10000, 13000);
    TEST_ASSERT_FALSE(s.shouldIgnite(1, 0, 13000));
}

void test_single_weak_beacon_keeps_the_hold() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 10600);
    s.onBeacon(kMacA, kGroup1, kFar, 10800);
    beacons(s, kMacA, kGroup1, kClose, 11000, 11600);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, 0, 11600));
}

void test_moving_away_restarts_the_hold() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 11000);
    beacons(s, kMacA, kGroup1, kFar, 11200, 12000);
    beacons(s, kMacA, kGroup1, kClose, 12200, 13400);
    TEST_ASSERT_FALSE(s.shouldIgnite(1, 0, 13400));
    beacons(s, kMacA, kGroup1, kClose, 13600, 13800);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, 0, 13800));
}

void test_flame_gone_does_not_spread() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 11000);
    TEST_ASSERT_FALSE(s.shouldIgnite(1, 0, 11000 + CLOSE_GAP_MS + 1));
}

void test_put_out_next_to_a_flame_waits_the_full_hold() {
    Spread s;
    beacons(s, kMacA, kGroup1, kClose, 10000, 13000);
    const uint32_t outSince = 13000;
    beacons(s, kMacA, kGroup1, kClose, 13200, 14400);
    TEST_ASSERT_FALSE(s.shouldIgnite(1, outSince, 14400));
    beacons(s, kMacA, kGroup1, kClose, 14600, 14600);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, outSince, 14600));
}

void test_any_close_flame_of_the_group_counts() {
    Spread s;
    beacons(s, kMacA, kGroup1, kFar, 10000, 12000);
    beacons(s, kMacB, kGroup1 | (1u << 3), kClose, 10000, 12000);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, 0, 12000));
    TEST_ASSERT_TRUE(s.shouldIgnite(3, 0, 12000));
}

void test_group_out_of_range_never_fires() {
    Spread s;
    beacons(s, kMacA, 0xFF, kClose, 10000, 13000);
    TEST_ASSERT_FALSE(s.shouldIgnite(MAX_FLAME_GROUPS, 0, 13000));
}

void test_full_table_replaces_the_oldest_sender() {
    Spread s;
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
    // MAX_FLAMES far senders fill the table; kMacA, close, takes the oldest slot.
    for (uint8_t i = 0; i < MAX_FLAMES; i++) {
        mac[5] = i;
        s.onBeacon(mac, kGroup1, kFar, 10000 + i);
    }
    beacons(s, kMacA, kGroup1, kClose, 11000, 12600);
    TEST_ASSERT_TRUE(s.shouldIgnite(1, 0, 12600));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_beacon_no_fire);
    RUN_TEST(test_close_flame_lights_after_hold);
    RUN_TEST(test_other_group_does_not_spread);
    RUN_TEST(test_far_flame_does_not_spread);
    RUN_TEST(test_single_weak_beacon_keeps_the_hold);
    RUN_TEST(test_moving_away_restarts_the_hold);
    RUN_TEST(test_flame_gone_does_not_spread);
    RUN_TEST(test_put_out_next_to_a_flame_waits_the_full_hold);
    RUN_TEST(test_any_close_flame_of_the_group_counts);
    RUN_TEST(test_group_out_of_range_never_fires);
    RUN_TEST(test_full_table_replaces_the_oldest_sender);
    return UNITY_END();
}
