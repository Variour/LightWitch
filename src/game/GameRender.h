#pragma once
#include <math.h>
#include <stdint.h>

#include "GameConfig.h"
#include "Lage.h"
#include "Participant.h"

// Turns the participant's state into the 34 Buzzerboard LEDs (round format
// v0, "Verhalten am Gerät"). Plain C++, no driver access.
//
// Orientation (firmware spec "Lage v0"): colours belong to logical keys;
// each lights the physical key that gives that input in the orientation
// whose mapping applies (Participant::inputLage()). Choice answers take the
// direction colours (owner decision). The keys that take an answer glow
// white at kLevelFree, the chosen answer and the confirmation ring follow
// kLevelFree's owner decision (GameConfig.h). The column fills in reading
// direction. Nothing blinks faster than 3 Hz, pulses are soft, nothing
// blinks red; a wrong answer shows in kWrongRgb.
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

// Direction colours, by logical direction (firmware spec "Lage v0"), for the
// choice answers and the change animation: up light orange, right blue, down
// purple, left light blue. At full level the four draw about 78 mA together (12 mA per channel at
// 255, Buzzerboard hardware/README.md current budget), well inside the 300 mA LED cap.
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

// a toward b by t/255.
inline Rgb blendRgb(Rgb a, Rgb b, uint8_t t) {
    return {(uint8_t)(a.r + ((int)b.r - a.r) * t / 255),
            (uint8_t)(a.g + ((int)b.g - a.g) * t / 255),
            (uint8_t)(a.b + ((int)b.b - a.b) * t / 255)};
}

// A key that takes an answer in an open round.
inline Rgb freeKeyRgb() { return scaleRgb(colorRgb(Color::White), kLevelFree); }

// The chosen answer's key: steady white until the bridge confirms it, then
// a pulse between white and c, starting at white.
inline Rgb chosenKeyRgb(const Participant& p, Rgb c, uint32_t now) {
    Rgb white = colorRgb(Color::White);
    if (!p.confirmed()) return white;
    return blendRgb(white, c, softPulse(now - p.confirmedMs(), kPulsePeriodMs, 0, 255));
}

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

// A right answer or a race win (kCelebrateMs, owner 2026-11-10), t = time
// since the result came: a bright head runs up the column in reading
// direction once per kCelebrateRunMs and leaves it green, the ring pulses
// softly between green and white. Each LED lights up once per run.
inline void renderCelebration(uint32_t t, bool rev, Rgb* px) {
    Rgb green = colorRgb(Color::Green);
    Rgb white = colorRgb(Color::White);
    uint32_t run = t % kCelebrateRunMs;
    uint32_t sweep = kCelebrateRunMs * 4 / 5;
    // Head from 2 LEDs before the column to 2 past its end in 80 % of a run.
    float head = run < sweep ? (float)run * (kScoreCount + 4) / (float)sweep - 2.0f : 100.0f;
    for (uint8_t n = 0; n < kScoreCount; n++) {
        float d = head - (float)n;
        Rgb c = d >= 0.0f || t >= kCelebrateRunMs ? green : Rgb{};
        float a = fabsf(d);
        if (a < 2.5f) c = blendRgb(c, white, (uint8_t)(255.0f * (1.0f - a / 2.5f)));
        px[scoreLed(n, rev)] = c;
    }
    fillRing(px, blendRgb(green, white, softPulse(t, kCelebrateRunMs, 0, 255)));
}

// The race device that answers now (kAnswerOverMs, owner 2026-11-10): its
// answer time as a full column that runs down evenly in reading direction
// and turns from green to kWrongRgb, the ring in the same colour; once it
// answered, the column stays where it was. In a race with answer keys its
// free answer keys glow white at kLevelFree, the given answer shows like a
// chosen answer. Time over without an answer: ring and column in kWrongRgb,
// after kAnswerOverMs the column alone.
inline void renderAnswering(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    Lage l = p.inputLage();
    bool rev = columnReversed(l);
    uint32_t at = p.answerGiven() ? p.inputMs() : now;
    int32_t left = r.deadlineKnown ? (int32_t)(r.deadline - at) : (int32_t)r.answerMs;
    const Key answers[4] = {Key::Up, Key::Right, Key::Down, Key::Left};
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };
    if (left <= 0) {
        fillScore(px, kScoreCount, kWrongRgb, rev);
        if ((uint32_t)-left < kAnswerOverMs) fillRing(px, kWrongRgb);
        return;
    }
    float frac = r.answerMs ? (float)left / (float)r.answerMs : 1.0f;
    if (frac > 1.0f) frac = 1.0f;
    Rgb c = blendRgb(colorRgb(Color::Green), kWrongRgb, (uint8_t)(255.0f * (1.0f - frac)));
    float lit = frac * kScoreCount;
    for (uint8_t n = 0; n < kScoreCount; n++) {
        float on = lit - (float)n;
        if (on >= 1.0f)
            px[scoreLed(n, rev)] = c;
        else if (on > 0.0f)
            px[scoreLed(n, rev)] = scaleRgb(c, (uint8_t)(255.0f * on));
    }
    fillRing(px, c);
    uint8_t val = p.value();
    for (uint8_t i = 0; i < 4; i++) {
        Key k = answers[i];
        if (!inMask(k)) continue;
        if (p.answerGiven() && val == i + 1)
            px[logicalLed(l, k)] = chosenKeyRgb(p, directionRgb(k), now);
        else if (!p.answerGiven())
            px[logicalLed(l, k)] = freeKeyRgb();
    }
}

// A scale of 24 steps in the rainbow colours (owner, 2026-11-10; step 1
// where the column starts in reading direction). Open: the whole column
// shows the rainbow at kLevelDimmed and the chosen step shows like a chosen
// answer, white until confirmed, then pulsing toward its colour, with the
// ring in its colour. Result (`correct`): the right step pulses between
// white and its colour. On the right step the device celebrates, then the
// ring stays green; one step off the own step shows in its colour and the
// ring pulses once; further off the own step shows in kWrongRgb.
inline void renderRainbow(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    Lage l = p.inputLage();
    bool rev = columnReversed(l);
    uint8_t val = p.value();
    int own = val != kNoValue && val >= 1 && val <= kRainbowSteps ? val - 1 : -1;
    for (uint8_t n = 0; n < kRainbowSteps && n < kScoreCount; n++)
        px[scoreLed(n, rev)] = scaleRgb(kRainbow[n], kLevelDimmed);
    if (r.phase == Phase::Closed && r.correct >= 1 && r.correct <= kRainbowSteps) {
        int right = r.correct - 1;
        int off = own < 0 ? 0xFF : (own > right ? own - right : right - own);
        uint32_t t = now - r.resultMs;
        if (off == 0 && t < kCelebrateMs) return renderCelebration(t, rev, px);
        px[scoreLed((uint8_t)right, rev)] =
            blendRgb(colorRgb(Color::White), kRainbow[right], softPulse(t, kPulsePeriodMs, 0, 255));
        if (own >= 0 && off > 0)
            px[scoreLed((uint8_t)own, rev)] = off == 1 ? kRainbow[own] : kWrongRgb;
        if (off == 0)
            fillRing(px, colorRgb(Color::Green));
        else if (off == 1 && t < kCelebrateRunMs)
            fillRing(px, blendRgb(colorRgb(Color::Green), colorRgb(Color::White),
                                  softPulse(t, kCelebrateRunMs, 0, 255)));
        return;
    }
    if (r.phase == Phase::Open && !p.answerLocked()) {
        px[logicalLed(l, Key::Up)] = freeKeyRgb();
        px[logicalLed(l, Key::Down)] = freeKeyRgb();
    }
    if (own < 0) return;
    px[scoreLed((uint8_t)own, rev)] = chosenKeyRgb(p, kRainbow[own], now);
    if (p.confirmed()) fillRing(px, kRainbow[own]);
}

// Named or anonymous, after the admission while login is open (owner,
// 2026-11-10): Up (with the animal name) and Down (anonymous) glow white at
// kLevelFree; the chosen one pulses like a chosen answer, Up toward green,
// Down toward dimmed white, and the ring glows green (named) or white at
// kLevelFree (anonymous). Without a choice the device plays anonymously.
inline void renderNaming(const Participant& p, uint32_t now, Rgb* px) {
    using N = Participant::Naming;
    Lage l = p.inputLage();
    px[logicalLed(l, Key::Up)] = freeKeyRgb();
    px[logicalLed(l, Key::Down)] = freeKeyRgb();
    if (p.naming() == N::None) return;
    bool named = p.naming() == N::Named;
    Rgb c = named ? colorRgb(Color::Green) : scaleRgb(colorRgb(Color::White), kLevelDimmed);
    px[logicalLed(l, named ? Key::Up : Key::Down)] =
        blendRgb(colorRgb(Color::White), c, softPulse(now - p.namingMs(), kPulsePeriodMs, 0, 255));
    fillRing(px, named ? colorRgb(Color::Green) : freeKeyRgb());
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
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };

    if (anim.on) {
        renderLageAnim(anim, now, px);
    } else if (locked) {
        // Provisional (owner to confirm): key LEDs and column off.
    } else if (p.namingOpen(now)) {
        renderNaming(p, now, px);
    } else if (r.known && r.type != RoundType::Idle && r.phase != Phase::Announced) {
        switch (r.type) {
            case RoundType::Scale: {
                if (r.rainbow) {
                    renderRainbow(p, now, px);
                    break;
                }
                if (r.phase == Phase::Open && !p.answerLocked()) {
                    for (Key k : {Key::Up, Key::Down})
                        if (inMask(k)) px[logicalLed(l, k)] = freeKeyRgb();
                }
                if (val != kNoValue && r.max >= r.min && val >= r.min) {
                    uint8_t steps = (uint8_t)(r.max - r.min + 1);
                    uint8_t per = (uint8_t)(kScoreCount / steps);  // proposal 3
                    Color zone = val <= r.redMax     ? Color::Red  // proposal 4
                                 : val >= r.greenMin ? Color::Green
                                                     : Color::Yellow;
                    fillScore(px, (uint8_t)(per * (val - r.min + 1)),
                              scaleRgb(colorRgb(zone), inputLevel), rev);
                    if (p.confirmed()) fillRing(px, colorRgb(zone));
                }
                break;
            }
            case RoundType::Race: {
                bool winner = r.winner != 0 && r.winner == p.number();
                if (winner && r.winnerWrong) {
                    fillScore(px, kScoreCount, kWrongRgb);
                } else if (winner && now - r.resultMs < kCelebrateMs) {
                    renderCelebration(now - r.resultMs, rev, px);
                } else if (winner) {
                    fillRing(px, colorRgb(Color::Green));
                    fillScore(px, kScoreCount, colorRgb(Color::Green));
                } else if (p.answeringMe()) {
                    renderAnswering(p, now, px);
                } else if (r.winner == 0 && val != kNoValue) {
                    // Pressed: white until confirmed, then the ring glows blue.
                    fillRing(px, colorRgb(p.confirmed() ? Color::Blue : Color::White));
                } else if (r.phase == Phase::Open && inMask(Key::Buzz) && r.startKnown &&
                           !p.beforeStart(now)) {
                    // Owner (2026-11-10): no countdown; the ring lights at the start.
                    fillRing(px, freeKeyRgb());
                }
                break;
            }
            case RoundType::Choice: {
                // Answer colours belong to the direction, not to the round.
                const Key answers[4] = {Key::Up, Key::Right, Key::Down, Key::Left};
                if (r.phase == Phase::Closed && r.correct >= 1 && r.correct <= 4) {
                    Key ck = answers[r.correct - 1];
                    px[logicalLed(l, ck)] = directionRgb(ck);
                    if (val == r.correct && now - r.resultMs < kCelebrateMs)
                        renderCelebration(now - r.resultMs, rev, px);
                    else if (val != kNoValue)
                        fillScore(px, kScoreCount,
                                  val == r.correct ? colorRgb(Color::Green) : kWrongRgb);
                    break;
                }
                for (uint8_t i = 0; i < 4; i++) {
                    Key k = answers[i];
                    if (!inMask(k)) continue;
                    if (val == i + 1)
                        px[logicalLed(l, k)] = chosenKeyRgb(p, directionRgb(k), now);
                    else if (r.phase == Phase::Open && !p.answerLocked())
                        px[logicalLed(l, k)] = freeKeyRgb();
                }
                if (val >= 1 && val <= 4 && p.confirmed())
                    fillRing(px, directionRgb(answers[val - 1]));
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
