#pragma once
#include <stdint.h>
#include <string.h>

#include "../game/Bridge.h"

namespace display {

// What the hub display shows, taken from the bridge each refresh. Counts and
// numbers only: no round content (questions, answers as text) reaches the hub.
struct HubView {
    static constexpr uint16_t kMaxListed = 120;  // members drawn on a page
    static constexpr uint8_t kMaxValue = 24;     // scale steps, choice keys 1-5

    // Session
    bool session = false;
    uint16_t sessionId = 0;
    bool console = false;  // console connected over USB (gateway link open)
    uint8_t channel = 0;
    bool login = false;
    uint32_t sessionMs = 0;  // since this hub saw the session start

    // Members: a number with a known device and a status.
    uint16_t members = 0;
    uint16_t present = 0;  // heard within kPresentMs
    uint16_t lowBattery = 0;
    uint16_t listedCount = 0;
    struct Member {
        uint16_t n;
        bool present;
        bool charging;
        uint8_t battery;  // percent, game::kNoBattery = unknown
        int8_t rssi;
        bool answered;
    } listed[kMaxListed];

    // Round
    game::RoundType type = game::RoundType::Idle;
    game::Phase phase = game::Phase::Closed;
    uint16_t round = 0;
    bool opened = false;
    uint32_t sinceOpenMs = 0;
    uint16_t answered = 0;  // present members with a value
    uint16_t answering = 0;
    uint16_t winner = 0;
    bool winnerWrong = false;
    uint8_t correct = 0;  // choice: right key value 1-5, 0 = none
    uint8_t min = 0, max = 0, redMax = 0, greenMin = 0;
    bool rainbow = false;
    uint8_t keyColor[5] = {};
    uint16_t counts[kMaxValue + 1] = {};  // index = value
    uint32_t valueSum = 0;
    uint16_t valueCount = 0;

    // Radio
    uint32_t rxFrames = 0, txFrames = 0;
    int8_t weakestRssi = 0;
    uint16_t weakCount = 0;  // present members below kWeakRssi
    bool offChannel = false;

    static constexpr uint32_t kPresentMs = 2 * game::kStatusPeriodMs + 2 * game::kStatusJitterMs;
    static constexpr int8_t kWeakRssi = -80;
    static constexpr uint8_t kLowBattery = 20;

    void fill(const game::Bridge& b, bool isBridge, bool consoleUp, uint32_t sessionMsIn,
              uint32_t now) {
        *this = HubView{};
        console = consoleUp;
        session = isBridge && b.active() && !b.ending();
        if (!session) return;
        sessionId = b.sessionId();
        channel = b.channel();
        login = b.login();
        sessionMs = sessionMsIn;
        rxFrames = b.rxFrames();
        txFrames = b.txFrames();
        offChannel = b.offChannel();

        const game::Bridge::RoundModel& r = b.roundModel();
        game::Bridge::StateInfo s = b.stateInfo(now);
        type = r.type;
        phase = r.phase;
        round = r.id;
        opened = s.sinceOpenValid;
        sinceOpenMs = s.sinceOpenValid && s.sinceOpenMs > 0 ? (uint32_t)s.sinceOpenMs : 0;
        answering = r.answering;
        winner = r.winner;
        winnerWrong = r.winnerWrong;
        correct = r.correct;
        min = r.min;
        max = r.max;
        redMax = r.redMax;
        greenMin = r.greenMin;
        rainbow = r.rainbow;
        memcpy(keyColor, r.keyColor, sizeof(keyColor));

        for (uint16_t n = 1; n <= b.maxNumber(); n++) {
            const game::Bridge::Device* d = b.device(n);
            if (!d || !d->macKnown || !d->hasStatus) continue;
            members++;
            bool here = now - d->seenAt < kPresentMs;
            bool value = d->hasValue && d->value != game::kNoValue;
            if (here) present++;
            if (here && d->battery != game::kNoBattery && d->battery < kLowBattery) lowBattery++;
            if (here && d->rssi < kWeakRssi) weakCount++;
            if (here && (weakestRssi == 0 || d->rssi < weakestRssi)) weakestRssi = d->rssi;
            if (value && here) answered++;
            if (value && d->value <= kMaxValue) {
                counts[d->value]++;
                valueSum += d->value;
                valueCount++;
            }
            if (listedCount < kMaxListed)
                listed[listedCount++] = {n, here, d->charging, d->battery, d->rssi, value};
        }
    }
};

}  // namespace display
