#pragma once
#include <Arduino.h>

#include "../config/Config.h"

// Per-light direct-command override slots (issue #457, LightWitch M1 command
// arbitration): a direct command aimed at one light stores a full LightConfig
// snapshot here (replace, never merge) and the light renders it instead of its
// group's state — bypassing the standing brightness adjustment
// (LightHardwareConfig::brightnessOverride), which applies again once the
// light rejoins its group. The hardware clamp at the LED driver boundary
// applies to overrides like everything else.
//
// Arbitration is "the newest command owns the light": the slot captures the
// group's LightConfig::seq at set time (baseSeq). Group re-broadcasts with
// seq == baseSeq (periodic self-heal) keep the override; any group update
// with seq > baseSeq — someone actually changed the group — displaces it, as
// does reassigning the light to a different group. A newer direct command
// simply replaces the slot; an optional durationMs auto-expires it
// receiver-side (see LightOverrides::expired and main.cpp's slow tick).
//
// Deliberately volatile: state lives in RAM only, so a reboot always comes
// back rendering the group. Local to this device — the mesh message that
// carries direct commands between devices builds on this (issue #458).
class LightOverrides {
   public:
    struct Slot {
        bool active = false;
        LightConfig cfg;       // full snapshot; replace, never merge
        uint32_t baseSeq = 0;  // group's light.seq when the override was set
        uint8_t groupId = 0;   // group the light followed when the override was set
        bool hasExpiry = false;
        uint32_t expiresAtMs = 0;  // millis() deadline, only meaningful with hasExpiry
    };

    static bool active(uint8_t i) { return i < MAX_LIGHTS && _slots[i].active; }

    static const LightConfig& config(uint8_t i) { return _slots[i].cfg; }

    // Captures the current group seq + membership as the arbitration baseline.
    // durationMs == 0 means no expiry.
    static void set(uint8_t i, const LightConfig& cfg, uint32_t durationMs) {
        if (i >= MAX_LIGHTS) return;
        const auto& l = Config::get().lights[i];
        GroupConfig* g = Config::group(l.groupId);
        Slot& s = _slots[i];
        s.active = true;
        s.cfg = cfg;
        s.baseSeq = g ? g->light.seq : 0;
        s.groupId = l.groupId;
        s.hasExpiry = durationMs > 0;
        s.expiresAtMs = millis() + durationMs;
    }

    static void clear(uint8_t i) {
        if (i < MAX_LIGHTS) _slots[i] = Slot{};
    }

    // True if light i's active override has been displaced by a newer group
    // command: the light was reassigned to a different group, or its group's
    // seq advanced past baseSeq (a real change, not a seq == baseSeq
    // re-broadcast). The caller clears the slot and reapplies the group.
    static bool displaced(uint8_t i) {
        if (i >= MAX_LIGHTS || !_slots[i].active) return false;
        const Slot& s = _slots[i];
        const auto& l = Config::get().lights[i];
        if (l.groupId != s.groupId) return true;
        GroupConfig* g = Config::group(l.groupId);
        return g && g->light.seq > s.baseSeq;
    }

    // True if light i's active override carries a durationMs whose deadline
    // has passed (wraparound-safe).
    static bool expired(uint8_t i, uint32_t nowMs) {
        if (i >= MAX_LIGHTS || !_slots[i].active || !_slots[i].hasExpiry) return false;
        return (int32_t)(nowMs - _slots[i].expiresAtMs) >= 0;
    }

   private:
    inline static Slot _slots[MAX_LIGHTS] = {};
};
