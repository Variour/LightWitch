#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "GameConfig.h"

// Radio frames of the game session (gateway protocol v0, "Abbildung auf die
// Funk-Nachrichten"). Packed, no padding, little-endian like the ESP32 and the
// host. The MsgType values are mirrored in src/mesh/MeshTypes.h, which
// static_asserts that both agree. All frames stay at or below 250 bytes.
namespace game {

static constexpr uint8_t kMsgQuizInvite = 37;
static constexpr uint8_t kMsgQuizState = 38;
static constexpr uint8_t kMsgQuizReport = 39;
static constexpr uint8_t kMsgQuizStatus = 40;
static constexpr uint8_t kMsgQuizAdmit = 41;
static constexpr uint8_t kMsgQuizRelease = 42;
static constexpr uint8_t kMsgQuizChannel = 43;

struct __attribute__((packed)) QuizHeader {
    uint8_t type;
    uint8_t proto;    // kProto
    uint16_t fleet;   // invite filter only; 0 = all
    uint16_t origin;  // 0 = bridge, kNoNumber = device without number
    uint16_t msgId;   // per-origin counter, +1 per transmission
    uint8_t ttl;      // always 0 in v0
    uint8_t hops;     // always 0 in v0
    uint8_t dist;     // 0 bridge, 1 device
    uint8_t flags;    // reserved, 0
};
static_assert(sizeof(QuizHeader) == 12, "QuizHeader layout");

struct __attribute__((packed)) QuizInviteMsg {
    QuizHeader h;
    uint16_t session;
    uint8_t channel;
    uint8_t inviteFlags;  // reserved, 0
};
static_assert(sizeof(QuizInviteMsg) == 16, "QuizInviteMsg layout");

struct __attribute__((packed)) QuizStateMsg {
    QuizHeader h;
    uint16_t session;
    uint16_t roundId;     // 0 = idle after session start
    uint16_t stateSeq;    // +1 per content change
    uint8_t roundType;    // RoundType
    uint8_t phase;        // Phase; Closed for idle
    uint16_t keyMask;     // bit per free key (Key index)
    uint8_t keyColor[5];  // 4-bit Color per key: byte k = key 2k low nibble, 2k+1 high nibble
    uint8_t scaleMin;
    uint8_t scaleMax;
    uint8_t redMax;
    uint8_t greenMin;
    int32_t openOffsetMs;  // time since round start at send; negative = opens in; 0 while announced
    uint16_t winner;       // race winner number, 0 = none
    uint8_t correct;       // choice answer 1-4, 0 = none
    uint16_t echoFirst;    // first number covered by echo
    uint8_t echoCount;     // entries in echo, at most kEchoPageSize
    uint8_t echo[kEchoPageSize];  // per number: value the bridge holds, kNoValue = none
};
static constexpr size_t kQuizStateHeadLen = offsetof(QuizStateMsg, echo);
static_assert(kQuizStateHeadLen == 41, "QuizStateMsg layout");
static_assert(sizeof(QuizStateMsg) == 241, "QuizStateMsg layout");

struct __attribute__((packed)) QuizReportMsg {
    QuizHeader h;
    uint16_t session;
    uint16_t roundId;
    uint8_t valueSeq;
    uint8_t value;
    int32_t pressOffsetMs;  // input moment since local round start
    uint8_t battery;        // percent, kNoBattery = unknown
    int8_t rssi;            // last bridge frame at the device
};
static_assert(sizeof(QuizReportMsg) == 24, "QuizReportMsg layout");

struct __attribute__((packed)) QuizStatusMsg {
    QuizHeader h;
    uint16_t session;
    uint8_t mac[6];
    uint32_t fwBuild;
    uint8_t battery;
    uint8_t flags;  // kStatusCharging | kStatusJoin | kStatusPressed | kStatusRejoin
    int8_t rssi;
    uint16_t lastRoundId;
    uint16_t lastStateSeq;
};
static_assert(sizeof(QuizStatusMsg) == 31, "QuizStatusMsg layout");

struct __attribute__((packed)) QuizAdmitMsg {
    QuizHeader h;
    uint16_t session;
    uint8_t mac[6];
    uint16_t number;
};
static_assert(sizeof(QuizAdmitMsg) == 22, "QuizAdmitMsg layout");

struct __attribute__((packed)) QuizReleaseMsg {
    QuizHeader h;
    uint16_t session;
    uint8_t mac[6];  // ff:ff:ff:ff:ff:ff = all
};
static_assert(sizeof(QuizReleaseMsg) == 20, "QuizReleaseMsg layout");

struct __attribute__((packed)) QuizChannelMsg {
    QuizHeader h;
    uint16_t session;
    uint8_t channel;
    uint16_t switchInMs;
};
static_assert(sizeof(QuizChannelMsg) == 17, "QuizChannelMsg layout");

static constexpr size_t kMaxFrameLen = sizeof(QuizStateMsg);
static_assert(kMaxFrameLen <= 250, "ESP-NOW v1 payload limit");

inline bool isQuizType(uint8_t type) { return type >= kMsgQuizInvite && type <= kMsgQuizChannel; }

// Strict framing: fixed frames need their exact size, QuizState needs its
// head plus exactly echoCount echo bytes, echoCount <= kEchoPageSize.
inline bool quizFrameLenOk(const uint8_t* d, size_t len) {
    if (len < 1) return false;
    switch (d[0]) {
        case kMsgQuizInvite:
            return len == sizeof(QuizInviteMsg);
        case kMsgQuizState: {
            if (len < kQuizStateHeadLen) return false;
            uint8_t count = d[offsetof(QuizStateMsg, echoCount)];
            return count <= kEchoPageSize && len == kQuizStateHeadLen + count;
        }
        case kMsgQuizReport:
            return len == sizeof(QuizReportMsg);
        case kMsgQuizStatus:
            return len == sizeof(QuizStatusMsg);
        case kMsgQuizAdmit:
            return len == sizeof(QuizAdmitMsg);
        case kMsgQuizRelease:
            return len == sizeof(QuizReleaseMsg);
        case kMsgQuizChannel:
            return len == sizeof(QuizChannelMsg);
        default:
            return false;
    }
}

// Copies a validated frame into its struct; the tail of QuizStateMsg.echo past
// echoCount stays zero. Returns false if the frame is not of type T's kind.
template <typename T>
inline bool decodeFrame(const uint8_t* d, size_t len, uint8_t type, T& out) {
    if (len < 1 || d[0] != type || !quizFrameLenOk(d, len) || len > sizeof(T)) return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, d, len);
    return true;
}

// On-air length of an encoded frame.
inline size_t stateFrameLen(const QuizStateMsg& m) { return kQuizStateHeadLen + m.echoCount; }

inline void setKeyColor(uint8_t* keyColor, uint8_t key, uint8_t color) {
    uint8_t& b = keyColor[key / 2];
    if (key % 2 == 0)
        b = (uint8_t)((b & 0xF0) | (color & 0x0F));
    else
        b = (uint8_t)((b & 0x0F) | ((color & 0x0F) << 4));
}

inline uint8_t getKeyColor(const uint8_t* keyColor, uint8_t key) {
    uint8_t b = keyColor[key / 2];
    return key % 2 == 0 ? (b & 0x0F) : (b >> 4);
}

}  // namespace game
