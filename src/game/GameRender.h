#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "Participant.h"

// Turns the participant's state into the 34 Buzzerboard LEDs (round format
// v0, "Verhalten am Gerät"). Plain C++, no driver access.
namespace game {

inline uint8_t keyLed(Key k) {
    switch (k) {
        case Key::Up:
            return kLedUp;
        case Key::Left:
            return kLedLeft;
        case Key::Enter:
            return kLedEnter;
        case Key::Right:
            return kLedRight;
        case Key::Down:
            return kLedDown;
        default:
            return 0xFF;
    }
}

// Triangle between kLevelDimmed and kLevelBright, one cycle per kPulsePeriodMs.
inline uint8_t pulseLevel(uint32_t now) {
    uint32_t ph = now % kPulsePeriodMs;
    uint32_t half = kPulsePeriodMs / 2;
    uint32_t tri = ph < half ? ph : kPulsePeriodMs - ph;  // 0..half
    return (uint8_t)(kLevelDimmed + (uint32_t)(kLevelBright - kLevelDimmed) * tri / half);
}

inline bool blinkOn(uint32_t now) { return now % kBlinkPeriodMs < kBlinkPeriodMs / 2; }

inline void fillRing(Rgb* px, Rgb c) {
    for (uint8_t i : kRingLeds) px[i] = c;
}

inline void fillScore(Rgb* px, uint8_t count, Rgb c) {
    for (uint8_t i = 0; i < count && i < kScoreCount; i++) px[kScoreFirst + i] = c;
}

// Ring overlay over the normal light pattern while an invite is visible.
// Returns false (and leaves ring untouched) when there is nothing to show.
inline bool renderInviteOverlay(const Participant& p, uint32_t now, Rgb* ring4) {
    if (p.state() != Participant::State::Normal || !p.inviteVisible(now)) return false;
    Rgb c = scaleRgb(colorRgb(Color::Blue), pulseLevel(now));
    for (uint8_t i = 0; i < 4; i++) ring4[i] = c;
    return true;
}

// Full frame while the device is in a session. px has kLedCount entries.
inline void renderParticipant(const Participant& p, uint32_t now, Rgb* px) {
    for (uint16_t i = 0; i < kLedCount; i++) px[i] = Rgb{};
    using S = Participant::State;
    if (p.state() == S::Joining) {
        if (blinkOn(now)) fillRing(px, colorRgb(Color::White));
        return;
    }
    if (p.state() != S::Member) return;

    // Status dot.
    if (p.gatewayAgeMs(now) < kNoGatewayWarnMs)
        px[kStatusDot] = scaleRgb(colorRgb(Color::White), kLevelFaint);
    else if (blinkOn(now))
        px[kStatusDot] = scaleRgb(colorRgb(Color::Orange), kLevelDimmed);

    const Participant::Round& r = p.round();
    uint8_t val = p.value();
    uint8_t inputLevel = (kPulseUntilConfirmed && val != kNoValue && !p.confirmed())
                             ? pulseLevel(now)
                             : kLevelBright;
    auto keyColor = [&](Key k) { return (Color)getKeyColor(r.keyColor, (uint8_t)k); };
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };

    if (r.known && r.type != RoundType::Idle && r.phase != Phase::Announced) {
        switch (r.type) {
            case RoundType::Scale: {
                if (r.phase == Phase::Open) {
                    for (Key k : {Key::Up, Key::Down})
                        if (inMask(k)) px[keyLed(k)] = colorRgb(keyColor(k));
                }
                if (val != kNoValue && r.max >= r.min && val >= r.min) {
                    uint8_t steps = (uint8_t)(r.max - r.min + 1);
                    uint8_t per = (uint8_t)(kScoreCount / steps);  // proposal 3
                    Color zone = val <= r.redMax     ? Color::Red  // proposal 4
                                 : val >= r.greenMin ? Color::Green
                                                     : Color::Yellow;
                    fillScore(px, (uint8_t)(per * (val - r.min + 1)),
                              scaleRgb(colorRgb(zone), inputLevel));
                }
                break;
            }
            case RoundType::Race: {
                bool winner = r.winner != 0 && r.winner == p.number();
                if (winner) {
                    fillRing(px, colorRgb(Color::Green));
                    fillScore(px, kScoreCount, colorRgb(Color::Green));
                } else if (r.phase == Phase::Open && inMask(Key::Buzz)) {
                    if (val != kNoValue)
                        fillRing(px, scaleRgb(colorRgb(Color::Blue), inputLevel));
                    else if (p.beforeStart(now))
                        fillRing(px, scaleRgb(colorRgb(Color::Yellow), kLevelFaint));
                    else if (r.startKnown)
                        fillRing(px, colorRgb(keyColor(Key::Buzz)));
                }
                break;
            }
            case RoundType::Choice: {
                const Key answers[4] = {Key::Up, Key::Right, Key::Down, Key::Left};
                if (r.phase == Phase::Closed && r.correct >= 1 && r.correct <= 4) {
                    Key ck = answers[r.correct - 1];
                    px[keyLed(ck)] = colorRgb(keyColor(ck));
                    if (val != kNoValue)
                        fillScore(px, kScoreCount,
                                  colorRgb(val == r.correct ? Color::Green : Color::Red));
                    break;
                }
                for (uint8_t i = 0; i < 4; i++) {
                    Key k = answers[i];
                    if (!inMask(k)) continue;
                    Rgb c = colorRgb(keyColor(k));
                    if (val == i + 1)
                        px[keyLed(k)] = scaleRgb(c, inputLevel);
                    else if (r.phase == Phase::Open)
                        px[keyLed(k)] = val == kNoValue ? c : scaleRgb(c, kLevelDimmed);
                }
                break;
            }
            default:
                break;
        }
    }

    // Admitted: ring green for 2 s.
    if (p.admittedAgeMs(now) < kAdmittedShowMs) fillRing(px, colorRgb(Color::Green));
}

}  // namespace game
