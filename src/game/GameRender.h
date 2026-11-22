#pragma once
#include <math.h>
#include <stdint.h>

#include "GameConfig.h"
#include "Lage.h"
#include "LedEffects.h"
#include "LedZones.h"
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

// The zones this module draws on.
static constexpr const LedZones& kZones = kBuzzerboardA0Zones;

// LED of a logical key (round key slot) in orientation l.
inline uint8_t logicalLed(Lage l, Key logical) {
    return kZones.keyLed(physicalKey(l, inputOf(logical)));
}

// Direction colours, by logical direction (firmware spec "Lage v0"), for the
// choice answers and the change animation: up light orange, right blue, down
// purple, left light blue; enter (answer 5) rose. At full level the four draw about 78 mA together
// (12 mA per channel at 255, Buzzerboard hardware/README.md current budget), well inside the 300 mA
// LED cap.
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
        case Key::Enter:  // owner (2026-11-10): the centre as answer 5, rose
            return {255, 40, 150};
        default:
            return {0, 0, 0};
    }
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
        px[kZones.keyLed(kKeyCircle[((j + rot) % 4 + 4) % 4])] = directionRgb(dirs[j]);
    uint32_t keysMs = lageAnimKeysMs(a.from, a.to);
    if (t < keysMs) return;
    uint32_t n = (t - keysMs) * kZones.columnCount / kLageAnimColumnMs;
    if (n < kZones.columnCount)
        px[kZones.columnLed((uint8_t)n, columnReversed(a.to))] = colorRgb(Color::White);
}

// A right answer or a race win (kCelebrateMs, owner 2026-11-10), t = time
// since the result came: a bright head runs up the column in reading
// direction once per kCelebrateRunMs and leaves it green, the ring pulses
// softly between green and white. Each LED lights up once per run.
inline void renderCelebration(uint32_t t, bool rev, Rgb* px) {
    Rgb green = colorRgb(Color::Green);
    Rgb white = colorRgb(Color::White);
    columnRun(kZones, px, t, kCelebrateRunMs, green, white, rev);
    fillRing(kZones, px, blendRgb(green, white, softPulse(t, kCelebrateRunMs, 0, 255)));
}

// The console's countdown on a device without an answer (link `game
// countdown`): the column runs down evenly in reading direction, white at
// kLevelFree.
inline void renderCountdown(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    int32_t left = (int32_t)(r.deadline - now);
    if (left <= 0 || r.answerMs == 0) return;
    float lit = (float)left * kZones.columnCount / (float)r.answerMs;
    columnLevel(kZones, px, lit, freeKeyRgb(), columnReversed(p.inputLage()));
}

// The race device that answers now (kAnswerOverMs, owner 2026-11-10): for
// the first half of its answer time the full column and the ring glow green
// and pulse softly once per kPulsePeriodMs; in the second half the column
// runs down evenly in reading direction and turns from green to kWrongRgb,
// the ring in the same colour. Once it answered, the display stays where it
// was (without the pulse). In a race with answer keys its
// free answer keys glow white at kLevelFree, the given answer shows like a
// chosen answer. Time over without an answer: ring and column in kWrongRgb,
// after kAnswerOverMs the column alone.
inline void renderAnswering(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    Lage l = p.inputLage();
    bool rev = columnReversed(l);
    uint32_t at = p.answerGiven() ? p.inputMs() : now;
    int32_t left = r.deadlineKnown ? (int32_t)(r.deadline - at) : (int32_t)r.answerMs;
    const Key answers[5] = {Key::Up, Key::Right, Key::Down, Key::Left, Key::Enter};
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };
    bool unlimited = r.answerMs == 0;  // no time limit: the pulse only
    if (left <= 0 && !unlimited) {
        fillColumn(kZones, px, kZones.columnCount, kWrongRgb, rev);
        if ((uint32_t)-left < kAnswerOverMs) fillRing(kZones, px, kWrongRgb);
        return;
    }
    // left in units of half the answer time: above 1 in the first half.
    float halves = unlimited ? 2.0f : 2.0f * (float)left / (float)r.answerMs;
    if (halves > 1.0f) {
        // Full column and ring green, pulsing softly (steady once answered).
        uint32_t used = unlimited                     ? now - r.turnSeenMs
                        : r.answerMs > (uint32_t)left ? r.answerMs - (uint32_t)left
                                                      : 0;
        uint8_t dip = p.answerGiven() ? 0 : softPulse(used, kPulsePeriodMs, 0, 255 - kLevelTurnLow);
        Rgb g = scaleRgb(colorRgb(Color::Green), (uint8_t)(255 - dip));
        fillColumn(kZones, px, kZones.columnCount, g, rev);
        fillRing(kZones, px, g);
    } else {
        // The column runs down and turns from green to kWrongRgb.
        Rgb c = blendRgb(colorRgb(Color::Green), kWrongRgb, (uint8_t)(255.0f * (1.0f - halves)));
        columnLevel(kZones, px, halves * kZones.columnCount, c, rev);
        fillRing(kZones, px, c);
    }
    uint8_t val = p.value();
    for (uint8_t i = 0; i < 5; i++) {
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
    for (uint8_t n = 0; n < kRainbowSteps && n < kZones.columnCount; n++)
        px[kZones.columnLed(n, rev)] = scaleRgb(kRainbow[n], kLevelDimmed);
    if (r.phase == Phase::Closed && r.correct >= 1 && r.correct <= kRainbowSteps) {
        int right = r.correct - 1;
        int off = own < 0 ? 0xFF : (own > right ? own - right : right - own);
        uint32_t t = now - r.resultMs;
        if (off == 0 && t < kCelebrateMs) return renderCelebration(t, rev, px);
        px[kZones.columnLed((uint8_t)right, rev)] =
            blendRgb(colorRgb(Color::White), kRainbow[right], softPulse(t, kPulsePeriodMs, 0, 255));
        if (own >= 0 && off > 0)
            px[kZones.columnLed((uint8_t)own, rev)] = off == 1 ? kRainbow[own] : kWrongRgb;
        if (off == 0)
            fillRing(kZones, px, colorRgb(Color::Green));
        else if (off == 1 && t < kCelebrateRunMs)
            fillRing(kZones, px,
                     blendRgb(colorRgb(Color::Green), colorRgb(Color::White),
                              softPulse(t, kCelebrateRunMs, 0, 255)));
        return;
    }
    if (r.phase == Phase::Open && !p.answerLocked()) {
        px[logicalLed(l, Key::Up)] = freeKeyRgb();
        px[logicalLed(l, Key::Down)] = freeKeyRgb();
        if (p.confirmEnter() && own >= 0) px[logicalLed(l, Key::Enter)] = freeKeyRgb();
    }
    if (own < 0) return;
    px[kZones.columnLed((uint8_t)own, rev)] = chosenKeyRgb(p, kRainbow[own], now);
    if (p.confirmed()) fillRing(kZones, px, kRainbow[own]);
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
    fillRing(kZones, px, named ? colorRgb(Color::Green) : freeKeyRgb());
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
        uint8_t led = kZones.keyLed(pr.physical);
        if (led == kNoLed) continue;
        Color c = (Color)getKeyColor(r.keyColor, (uint8_t)pr.key);
        px[led] = scaleRgb(colorRgb(c), pulseLevel(now));
    }
}

// The round's clock on the ring (SYSTEM.md v7): while the console's countdown
// runs, ceil(4 * left / duration) corners clockwise from the upper right,
// else all four, in c.
inline void renderRingClock(const Participant& p, uint32_t now, Rgb c, Rgb* px) {
    const Participant::Round& r = p.round();
    uint8_t lit = 4;
    if (r.answering == 0 && r.answerMs != 0 && r.deadlineKnown) {
        int32_t left = (int32_t)(r.deadline - now);
        lit = left <= 0 ? 0 : (uint8_t)((4 * (uint32_t)left + r.answerMs - 1) / r.answerMs);
        if (lit > 4) lit = 4;
    }
    // kZones.ring: lower left, upper left, upper right, lower right.
    static constexpr uint8_t kClockwise[4] = {2, 3, 0, 1};
    for (uint8_t i = 0; i < lit; i++) px[kZones.ring[kClockwise[i]]] = c;
}

// One ring corner after the other, clockwise, once per kWrongWaveMs.
inline void renderRingWave(uint32_t t, Rgb c, Rgb* px) {
    static constexpr uint8_t kClockwise[4] = {2, 3, 0, 1};
    px[kZones.ring[kClockwise[(t * 4 / kWrongWaveMs) % 4]]] = c;
}

// Choice round (owner, 2026-11-18, SYSTEM.md "Entscheidungen v7", levels in
// GameConfig.h): one press gives the answer, Enter releases it.
inline void renderChoice(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    Lage l = p.inputLage();
    bool rev = columnReversed(l);
    const Key answers[4] = {Key::Up, Key::Right, Key::Down, Key::Left};
    auto inMask = [&](Key k) { return (r.keyMask & (1u << (uint8_t)k)) != 0; };
    uint8_t val = p.value();
    bool given = val >= 1 && val <= 4;
    Rgb yellow = colorRgb(Color::Yellow);
    Rgb enterLed{};
    if (r.phase == Phase::Closed && r.correct >= 1 && r.correct <= 4) {
        bool peek = p.peeking(now);
        if (r.anonymous && !peek) {
            if (given) px[logicalLed(l, answers[val - 1])] = scaleRgb(yellow, kLevelDimmed);
            fillRing(kZones, px, scaleRgb(kWarmWhiteRgb, kLevelFaint));
            px[logicalLed(l, Key::Enter)] = scaleRgb(
                colorRgb(Color::White), softPulse(now, kBreathMs, kLevelShowMin, kLevelRelease));
            return;
        }
        uint32_t t = now - (peek ? p.peekMs() : r.resultMs);
        bool right = given && val == r.correct;
        if (right && t < kCelebrateMs)
            renderCelebration(t, rev, px);
        else if (!given)
            fillRing(kZones, px, kNotGivenRgb);
        else if (!right && t < kWrongWaveMs)
            renderRingWave(t, kWrongRgb, px);
        px[logicalLed(l, answers[r.correct - 1])] = scaleRgb(colorRgb(Color::Green), kLevelRight);
        if (given)
            px[logicalLed(l, answers[val - 1])] =
                scaleRgb(right ? colorRgb(Color::Green) : kWrongRgb,
                         softPulse(t, kPulsePeriodMs, kLevelDimmed, kLevelBright));
        return;
    }
    if (r.phase == Phase::Closed) {
        if (given) px[logicalLed(l, answers[val - 1])] = scaleRgb(yellow, kLevelDimmed);
        uint32_t t = now - r.closedMs;
        if (t < kClosedShowMs)
            fillRing(kZones, px,
                     scaleRgb(kWarmWhiteRgb,
                              softPulse(t, kClosedShowMs / 2, kLevelDimmed, kLevelBright)));
        return;
    }
    if (!given) {
        for (Key k : answers)
            if (inMask(k)) px[logicalLed(l, k)] = freeKeyRgb();
        renderRingClock(p, now, freeKeyRgb(), px);
        return;
    }
    // Given: yellow, pulsing until the bridge confirms it.
    uint8_t lvl = p.confirmed() ? kLevelGiven : pulseLevel(now);
    px[logicalLed(l, answers[val - 1])] = scaleRgb(yellow, lvl);
    if (p.rule().release) enterLed = scaleRgb(colorRgb(Color::White), kLevelRelease);
    px[logicalLed(l, Key::Enter)] = enterLed;
    renderRingClock(p, now, scaleRgb(yellow, kLevelGiven), px);
}

// Teacher device (owner, 2026-11-18, SYSTEM.md v7): only keys that do
// something now glow white. Right (next) and Left (back) always; in a closed
// choice without its resolution the answer keys too (held: this answer is
// right). The held key lights brighter. The column shows how many devices
// gave a value, one LED each; the ring is the round's clock while it runs.
inline void renderTeacher(const Participant& p, uint32_t now, Rgb* px) {
    const Participant::Round& r = p.round();
    auto key = [&](Key k) {
        uint8_t led = kZones.keyLed(k);
        if (led != kNoLed)
            px[led] = scaleRgb(colorRgb(Color::White), p.keyDown(k) ? kLevelBright : kLevelFree);
    };
    bool open = r.known && r.type != RoundType::Idle && r.phase == Phase::Open;
    if (open) {
        if (p.gatewayAgeMs(now) < kNoGatewayWarnMs)
            px[kZones.dot] = scaleRgb(colorRgb(Color::White), kLevelFree);
    } else {
        key(Key::Right);
        key(Key::Left);
    }
    if (r.known && r.type == RoundType::Choice && r.phase == Phase::Closed && r.correct == 0)
        for (Key k : {Key::Up, Key::Right, Key::Down, Key::Left})
            if (r.keyMask & (1u << (uint8_t)k)) key(k);
    uint16_t given = p.givenCount();
    if (r.known && r.type != RoundType::Idle && given > 0)
        fillColumn(kZones, px, given > kZones.columnCount ? kZones.columnCount : (uint8_t)given,
                   scaleRgb(colorRgb(Color::White), kLevelDimmed));
    if (open) renderRingClock(p, now, freeKeyRgb(), px);
}

// Full frame while the device is in a session. px has kLedCount entries.
inline void renderParticipant(const Participant& p, uint32_t now, Rgb* px) {
    allOff(kZones, px);
    using S = Participant::State;
    if (p.state() == S::Joining) {
        if (blinkOn(now)) fillRing(kZones, px, colorRgb(Color::White));
        return;
    }
    if (p.state() != S::Member) return;

    // Status dot; locked by the round's hold it pulses slowly white.
    bool locked = p.locked();
    if (p.gatewayAgeMs(now) >= kNoGatewayWarnMs) {
        if (blinkOn(now)) px[kZones.dot] = scaleRgb(colorRgb(Color::Orange), kLevelDimmed);
    } else if (locked) {
        px[kZones.dot] = scaleRgb(colorRgb(Color::White),
                                  softPulse(now, kLockPulseMs, kLevelFaint, kLevelBright));
    } else {
        px[kZones.dot] = scaleRgb(colorRgb(Color::White), kLevelFaint);
    }

    if (p.teacher()) return renderTeacher(p, now, px);

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
        if (p.countdownMe() && r.type != RoundType::Choice) renderCountdown(p, now, px);
        switch (r.type) {
            case RoundType::Scale: {
                if (r.rainbow) {
                    renderRainbow(p, now, px);
                    break;
                }
                if (r.phase == Phase::Open && !p.answerLocked()) {
                    for (Key k : {Key::Up, Key::Down})
                        if (inMask(k)) px[logicalLed(l, k)] = freeKeyRgb();
                    // Enter logs a value in (confirm).
                    if (p.confirmEnter() && val != kNoValue)
                        px[logicalLed(l, Key::Enter)] = freeKeyRgb();
                }
                if (val != kNoValue && r.max >= r.min && val >= r.min) {
                    uint8_t steps = (uint8_t)(r.max - r.min + 1);
                    uint8_t per = (uint8_t)(kZones.columnCount / steps);  // proposal 3
                    Color zone = val <= r.redMax     ? Color::Red         // proposal 4
                                 : val >= r.greenMin ? Color::Green
                                                     : Color::Yellow;
                    fillColumn(kZones, px, (uint8_t)(per * (val - r.min + 1)),
                               scaleRgb(colorRgb(zone), inputLevel), rev);
                    if (p.confirmed()) fillRing(kZones, px, colorRgb(zone));
                }
                break;
            }
            case RoundType::Race: {
                bool winner = r.winner != 0 && r.winner == p.number();
                if (winner && r.winnerWrong) {
                    fillColumn(kZones, px, kZones.columnCount, kWrongRgb);
                } else if (winner && now - r.resultMs < kCelebrateMs) {
                    renderCelebration(now - r.resultMs, rev, px);
                } else if (winner) {
                    fillRing(kZones, px, colorRgb(Color::Green));
                    fillColumn(kZones, px, kZones.columnCount, colorRgb(Color::Green));
                } else if (p.answeringMe()) {
                    renderAnswering(p, now, px);
                } else if ((r.winner == 0 || r.winnerWrong) && val != kNoValue) {
                    // Pressed: white until confirmed, then the ring glows blue;
                    // also while the winner so far was rated wrong.
                    fillRing(kZones, px, colorRgb(p.confirmed() ? Color::Blue : Color::White));
                } else if (r.phase == Phase::Open && inMask(Key::Buzz) && r.startKnown &&
                           !p.beforeStart(now)) {
                    // Owner (2026-11-10): no countdown; the ring lights at the start.
                    fillRing(kZones, px, freeKeyRgb());
                }
                break;
            }
            case RoundType::Choice:
                renderChoice(p, now, px);
                break;
            case RoundType::Move:
                renderMove(p, now, px);
                break;
            default:
                break;
        }
    }

    // Admitted: ring green for 2 s.
    if (p.admittedAgeMs(now) < kAdmittedShowMs) fillRing(kZones, px, colorRgb(Color::Green));
}

}  // namespace game
