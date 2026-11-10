#pragma once
#include <stdint.h>

// Every tunable of the game session module in one place. Values marked
// "fixed v0" come from the round format's "Feste Werte in v0" list or the
// gateway protocol; "proposal N" refers to item N of the round format's
// "Entscheidungen für Timo" list (implemented with its proposed default);
// "firmware" values are not set by the contract docs and were chosen here.
namespace game {

// ── Radio protocol ───────────────────────────────────────────────────────────
static constexpr uint8_t kProto = 1;           // fixed v0: `proto` in every frame header
static constexpr uint16_t kMaxNumber = 999;    // fixed v0: device numbers 1-999, 0 = bridge
static constexpr uint16_t kNoNumber = 0xFFFF;  // fixed v0: `origin` of a device without number
static constexpr uint8_t kEchoPageSize = 200;  // fixed v0: echo entries per QuizState frame
static constexpr uint8_t kMinChannel = 1;      // fixed v0: 2.4 GHz channels 1-13 only
static constexpr uint8_t kMaxChannel = 13;
// firmware: v0 defines no way to store a device fleet, so every device is in
// fleet 0 and sees invites for fleet 0 ("all devices") only.
static constexpr uint16_t kDeviceFleet = 0;

// ── Bridge (gateway) radio timing ────────────────────────────────────────────
static constexpr uint32_t kStateRepeat1Ms = 50;           // fixed v0: repeat after 50 ms
static constexpr uint32_t kStateRepeat2Ms = 150;          // fixed v0: and after 150 ms
static constexpr uint32_t kStatePeriodOpenMs = 250;       // fixed v0: then every 250 ms while open
static constexpr uint32_t kStatePeriodIdleMs = 1000;      // fixed v0: otherwise every second
static constexpr uint32_t kStateMinGapMs = 50;            // fixed v0: coalesce faster changes
static constexpr uint32_t kInviteSessionPeriodMs = 1000;  // fixed v0: 1x/s on the session channel
static constexpr uint32_t kInviteHopPeriodMs =
    300;                                           // proposal 13: next foreign channel every 300 ms
static constexpr uint32_t kInviteHopDwellMs = 30;  // proposal 13: back after about 30 ms
static constexpr uint8_t kInvitesPerHop = 2;       // proposal 13: two invites per foreign channel
static constexpr uint32_t kEndReleaseMs = 5000;    // fixed v0: release to all for 5 s
static constexpr uint32_t kReleaseOneMs = 2000;    // fixed v0: single release for 2 s
static constexpr uint32_t kAdmitRepeatMs = 250;    // fixed v0: admit every 250 ms
static constexpr uint32_t kAdmitTimeoutMs = 5000;  // fixed v0: at most 5 s
static constexpr uint32_t kChannelAnnounceMs = 2000;  // fixed v0: announce a channel change 2 s
// firmware: repeat interval inside the release and channel-change bursts.
static constexpr uint32_t kBurstRepeatMs = 250;
// firmware: pending admits the bridge tracks at once.
static constexpr uint16_t kMaxPendingAdmits = 128;
// firmware: concurrent single releases.
static constexpr uint8_t kMaxPendingReleases = 16;

// ── Bridge USB link ──────────────────────────────────────────────────────────
static constexpr uint16_t kLineMax = 512;          // fixed v0: bytes per line, both directions
static constexpr uint8_t kEventQueueLines = 32;    // fixed v0: queued event lines
static constexpr uint32_t kLinkTimeoutMs = 10000;  // fixed v0: link ends after 10 s of silence
static constexpr uint32_t kRoundFlushMaxMs = 200;  // fixed v0: flush old values before a new round
static constexpr uint8_t kLinkVersion = 0;         // fixed v0: namespace `link` version
static constexpr uint8_t kGameVersion = 0;         // fixed v0: namespace `game` version
static constexpr uint8_t kIdMaxLen = 16;           // fixed v0: string `id` length
static constexpr uint16_t kUsbTxBufferBytes = 4096;  // fixed v0: USB send buffer of about 4 KB
// firmware: USB receive buffer, two full lines.
static constexpr uint16_t kUsbRxBufferBytes = 1024;
// firmware: longest queued event line (hello, join and error lines stay well below).
static constexpr uint16_t kEventLineMax = 320;

// ── Radio receive queue ──────────────────────────────────────────────────────
static constexpr uint8_t kRxQueueBridge = 64;       // fixed v0: 64 frames on the bridge
static constexpr uint8_t kRxQueueParticipant = 16;  // firmware: participants hear only the bridge

// ── Participant timing ───────────────────────────────────────────────────────
static constexpr uint32_t kScaleSendDelayMs = 150;  // fixed v0: send 150 ms after the last tap
// firmware: in a round with firstOnly a scale value goes out, and stays,
// this long after the last tap, so a few taps set it.
static constexpr uint32_t kScaleCommitMs = 1000;
// owner (2026-11-10): holding Up or Down in a scale round steps on, after
// kScaleRepeatDelayMs once per kScaleRepeatMs.
static constexpr uint32_t kScaleRepeatDelayMs = 400;
static constexpr uint32_t kScaleRepeatMs = 120;
static constexpr uint32_t kRetryMinMs = 300;  // fixed v0: resend after 300-600 ms
static constexpr uint32_t kRetryMaxMs = 600;
static constexpr uint32_t kRetryCapMs = 2000;      // fixed v0: doubling up to 2 s
static constexpr uint32_t kJoinRepeatMinMs = 300;  // fixed v0: join request every 300-600 ms
static constexpr uint32_t kJoinRepeatMaxMs = 600;
static constexpr uint32_t kJoinTimeoutMs = 30000;      // fixed v0: give up joining after 30 s
static constexpr uint32_t kInviteVisibleMs = 10000;    // fixed v0: invite shown 10 s after the last
static constexpr uint32_t kRejoinListenMs = 10000;     // fixed v0 / proposal 15: wait 10 s at boot
static constexpr uint32_t kNoGatewayWarnMs = 3000;     // fixed v0: "no connection" after 3 s
static constexpr uint32_t kNoGatewaySearchMs = 10000;  // fixed v0: channel search after 10 s
static constexpr uint32_t kAutoReleaseMs = 30UL * 60UL * 1000UL;  // decided: leave after 30 min
static constexpr uint32_t kSearchDwellMs = 600;     // mesh concept: about 0.6 s per channel
static constexpr uint32_t kStatusPeriodMs = 15000;  // fixed v0: status about every 15 s
// firmware: the status interval is drawn from kStatusPeriodMs +- kStatusJitterMs.
static constexpr uint32_t kStatusJitterMs = 1500;
static constexpr uint32_t kAdmittedShowMs = 2000;  // fixed v0: ring green for 2 s
static constexpr uint32_t kLeaveComboMs = 3000;    // proposal 14: BUZZ + ENTER held 3 s
// firmware: a member treats login as open while invites arrived this recently
// (the bridge sends one per second on the session channel).
static constexpr uint32_t kLoginOpenHoldMs = 2500;
// firmware: delay between "all LEDs off" and the restart, so the LEDs update.
static constexpr uint32_t kLeaveRestartDelayMs = 200;
// proposal 8: the last press counts; owner (2026-11-10): a round with
// firstOnly (link `game round`) takes only the first answer instead.
static constexpr bool kChoiceChangeAllowed = true;
// proposal 11: the scale column pulses until the echo matches (the keys
// follow kLevelFree instead).
static constexpr bool kPulseUntilConfirmed = true;

// ── Orientation and move rounds (firmware spec "Lage v0") ────────────────────
static constexpr uint32_t kLageSettleMs = 2000;  // decided: a new orientation counts after 2 s
static constexpr uint8_t kKeyLogDepth = 8;       // decided: unacknowledged presses a device keeps
// firmware: accepted presses the bridge holds until their USB line is written.
static constexpr uint8_t kKeyQueueBridge = 64;
// firmware: change animation. The direction colours move one key per step,
// below 3 changes per second; then one LED runs through the column.
static constexpr uint32_t kLageAnimStepMs = 350;
static constexpr uint32_t kLageAnimColumnMs = 720;
// firmware: status dot pulse while the round's hold locks the device (0.5 Hz).
static constexpr uint32_t kLockPulseMs = 2000;

// ── LEDs (Buzzerboard rev A0) ────────────────────────────────────────────────
// One chain of 34 SK6812MINI-E. Verified against the routed board
// (buzzerboard.kicad_pcb: LEDn DIN = LED_D(n-1), positions) and
// hardware/gen/buzzer.py ROLES: chain index = LEDn - 1.
static constexpr uint16_t kLedCount = 34;
static constexpr uint8_t kScoreFirst = 0;  // LED1-24: score column, bottom to top
static constexpr uint8_t kScoreCount = 24;
// LED25-28: buzzer ring corners (lower left, upper left, upper right, lower right).
static constexpr uint8_t kRingLeds[4] = {24, 25, 26, 27};
static constexpr uint8_t kStatusDot = 28;  // LED29: switch window, the status dot
static constexpr uint8_t kLedUp = 29;      // LED30
static constexpr uint8_t kLedLeft = 30;    // LED31
static constexpr uint8_t kLedEnter = 31;   // LED32
static constexpr uint8_t kLedRight = 32;   // LED33
static constexpr uint8_t kLedDown = 33;    // LED34

// Brightness levels ("hell", "gedimmt" ~20 %, "schwach" ~5 %). The light's
// brightness clamp and the driver's current budget still apply on top.
static constexpr uint8_t kLevelBright = 255;
static constexpr uint8_t kLevelDimmed = 51;
static constexpr uint8_t kLevelFaint = 13;
// owner (2026-11-09): the session scales these to the brightness light 0
// renders at (its brightness override, else its group's brightness), so
// kLevelBright shows at that brightness. At least kLevelShowMin, so a
// session stays visible with the LEDs switched off.
static constexpr uint8_t kLevelShowMin = 2;
// owner (2026-11-10): in an open round the keys that take an answer glow
// white at kLevelFree (40 %): the choice answers, Up and Down of a scale,
// the buzzer ring of a race. The chosen answer shows steady white at
// kLevelBright until the bridge confirms it, then pulses once per
// kPulsePeriodMs between white and its direction colour, while the other
// answer keys stay at kLevelFree (dark once a firstOnly round has the
// answer). Confirmed, the ring glows steadily in the answer's colour:
// direction colour, the scale value's zone colour, blue for a race. The
// result of a closed round shows as before. GameManager scales every level
// to the brightness with at least one LED step above 0 (scaleToLevel), so
// kLevelFree stays visible at the lowest brightness.
static constexpr uint8_t kLevelFree = 102;
static constexpr uint32_t kPulsePeriodMs = 1000;  // firmware: one pulse per second
// owner (2026-11-10): a right answer at the resolution (choice with
// correct) and the race winner celebrate for kCelebrateMs: a bright head
// runs up the column once per kCelebrateRunMs and fills it green, the ring
// pulses softly between green and white; then the result shows as before. A
// race winner whose answer the quizmaster rated wrong (link `verdict`) shows
// the column red instead.
static constexpr uint32_t kCelebrateMs = 2000;
static constexpr uint32_t kCelebrateRunMs = 1000;
static constexpr uint32_t kBlinkPeriodMs = 1000;  // fixed v0: status dot blinks at 1 Hz

}  // namespace game
