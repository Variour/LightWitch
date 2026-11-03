#pragma once
#include <stdint.h>

#include "GameConfig.h"
#include "Lage.h"
#include "Participant.h"

// Turns the participant's state into the 34 Buzzerboard LEDs (round format
// v0, "Verhalten am Gerät"). Plain C++, no driver access.
//
// Orientation (firmware spec "Lage v0"): the round's key colours belong to
// logical keys; each lights the physical key that gives that input in the
// orientation whose mapping applies (Participant::inputLage()). The column
// fills in reading direction. Nothing blinks faster than 3 Hz, pulses are
// soft, nothing blinks red.
namespace game {

// LED of a physical key; 0xFF for keys without one.
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

// LED of a logical key (round key slot) in orientation l.
inline uint8_t logicalLed(Lage l, Key logical) { return keyLed(physicalKey(l, inputOf(logical))); }

// Direction colours, by logical direction (firmware spec "Lage v0"): up
// light orange, right blue, down purple, left light blue. At full level the
// four draw about 78 mA together (12 mA per channel at 255, Buzzerboard
// hardware/README.md current budget), well inside the 300 mA LED cap.
inline Rgb directionRgb(Key logical) {
    switch (logical) {
        case Key::Up:
            return {255, 110, 25};
        case Key::Right:
            return {20, 70, 255};
        case Key::Down:
            return {140, 30, 255};
        case Key::Left:
            return {70, 180, 255};
        default:
            return {0, 0, 0};
    }
}

// Triangle between lo and hi, one cycle per periodMs.
inline uint8_t softPulse(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi) {
    uint32_t ph = now % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t tri = ph < half ? ph : periodMs - ph;  // 0..half
    return (uint8_t)(lo + (uint32_t)(hi - lo) * tri / half);
}

// Triangle between kLevelDimmed and kLevelBright, one cycle per kPulsePeriodMs.
inline uint8_t pulseLevel(uint32_t now) {
    return softPulse(now, kPulsePeriodMs, kLevelDimmed, kLevelBright);
}

inline bool blinkOn(uint32_t now) { return now % kBlinkPeriodMs < kBlinkPeriodMs / 2; }

inline void fillRing(Rgb* px, Rgb c) {
    for (uint8_t i : kRingLeds) px[i] = c;
}

// Column LED n in reading direction (0 = where the fill starts).
inline uint8_t scoreLed(uint8_t n, bool reversed) {
    return (uint8_t)(kScoreFirst + (reversed ? kScoreCount - 1 - n : n));
}

inline void fillScore(Rgb* px, uint8_t count, Rgb c, bool reversed = false) {
    for (uint8_t i = 0; i < count && i < kScoreCount; i++) px[scoreLed(i, reversed)] = c;
}

// Ring overlay over the normal light pattern while an invite is visible.
// Returns false (and leaves ring untouched) when there is nothing to show.
inline bool renderInviteOverlay(const Participant& p, uint32_t now, Rgb* ring4) {
    if (p.state() != Participant::State::Normal || !p.inviteVisible(now)) return false;
    Rgb c = scaleRgb(colorRgb(Color::Blue), pulseLevel(now));
    for (uint8_t i = 0; i < 4; i++) ring4[i] = c;
    return true;
}

// Change animation: the direction colours run once around the key circle and
// on to their keys in the new orientation, then one LED runs through the
// column in its new reading direction.
inline void renderLageAnim(const Participant::LageAnim& a, uint32_t now, Rgb* px) {
    uint32_t t = now - a.start;
    LageAnimPlan plan = lageAnimPlan(a.from, a.to);
    uint32_t step = t / kLageAnimStepMs;
    if (step > plan.steps) step = plan.steps;
    int rot = (int)lageRotation(a.from) + plan.dir * (int)step;
    const Key dirs[4] = {Key::Up, Key::Right, Key::Down, Key::Left};
    for (int j = 0; j < 4; j++)
        px[keyLed(kKeyCircle[((j + rot) % 4 + 4) % 4])] = directionRgb(dirs[j]);
    uint32_t keysMs = lageAnimKeysMs(a.from, a.to);
    if (t < keysMs) return;
    uint32_t n = (t - keysMs) * kScoreCount / kLageAnimColumnMs;
    if (n < kScoreCount) px[scoreLed((uint8_t)n, columnReversed(a.to))] = colorRgb(Color::White);
}

// Move round (provisional display): the direction keys glow dimmed in the
// round's colour while open; each unacknowledged press pulses its key.
inline void renderMove(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    if (r.phase != Phase::Open) return;
    Lage l = p.inputLage();
    for (Key k : {Key::Up, Key::Right, Key::Down, Key::Left}) {
        if (!(r.keyMask & (1u << (uint8_t)k))) continue;
        px[logicalLed(l, k)] =
            scaleRgb(colorRgb((Color)getKeyColor(r.keyColor, (uint8_t)k)), kLevelDimmed);
    }
    for (uint8_t i = 0; i < p.pressCount(); i++) {
        const Participant::Press& pr = p.press(i);
        uint8_t led = keyLed(pr.physical);
        if (led == 0xFF) continue;
        Color c = (Color)getKeyColor(r.keyColor, (uint8_t)pr.key);
        px[led] = scaleRgb(colorRgb(c), pulseLevel(now));
    }
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

    // Status dot; locked by the round's hold it pulses slowly white.
    bool locked = p.locked();
    if (p.gatewayAgeMs(now) >= kNoGatewayWarnMs) {
        if (blinkOn(now)) px[kStatusDot] = scaleRgb(colorRgb(Color::Orange), kLevelDimmed);
    } else if (locked) {
        px[kStatusDot] = scaleRgb(colorRgb(Color::White),
                                  softPulse(now, kLockPulseMs, kLevelFaint, kLevelBright));
    } else {
        px[kStatusDot] = scaleRgb(colorRgb(Color::White), kLevelFaint);
    }

    const Participant::Round& r = p.round();
    const Participant::LageAnim& anim = p.lageAnim();
    Lage l = p.inputLage();
    bool rev = columnReversed(l);
    uint8_t val = p.value();
    uint8_t inputLevel = (kPulseUntilConfirmed && val != kNoValue && !p.confirmed())
                             ? pulseLevel(now)
                             : kLevelBright;
    auto keyColor = [&](Key k) { return (Color)getKeyColor(r.keyColor, (uint8_t)k); };
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };

    if (anim.on) {
        renderLageAnim(anim, now, px);
    } else if (locked) {
        // Provisional (owner to confirm): key LEDs and column off.
    } else if (r.known && r.type != RoundType::Idle && r.phase != Phase::Announced) {
        switch (r.type) {
            case RoundType::Scale: {
                if (r.phase == Phase::Open) {
                    for (Key k : {Key::Up, Key::Down})
                        if (inMask(k)) px[logicalLed(l, k)] = colorRgb(keyColor(k));
                }
                if (val != kNoValue && r.max >= r.min && val >= r.min) {
                    uint8_t steps = (uint8_t)(r.max - r.min + 1);
                    uint8_t per = (uint8_t)(kScoreCount / steps);  // proposal 3
                    Color zone = val <= r.redMax     ? Color::Red  // proposal 4
                                 : val >= r.greenMin ? Color::Green
                                                     : Color::Yellow;
                    fillScore(px, (uint8_t)(per * (val - r.min + 1)),
                              scaleRgb(colorRgb(zone), inputLevel), rev);
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
                    px[logicalLed(l, ck)] = colorRgb(keyColor(ck));
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
                        px[logicalLed(l, k)] = scaleRgb(c, inputLevel);
                    else if (r.phase == Phase::Open)
                        px[logicalLed(l, k)] = val == kNoValue ? c : scaleRgb(c, kLevelDimmed);
                }
                break;
            }
            case RoundType::Move:
                renderMove(p, now, px);
                break;
            default:
                break;
        }
    }

    // Admitted: ring green for 2 s.
    if (p.admittedAgeMs(now) < kAdmittedShowMs) fillRing(px, colorRgb(Color::Green));
}

}  // namespace game
