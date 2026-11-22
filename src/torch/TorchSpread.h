#pragma once
#include <stdint.h>
#include <string.h>

// Torch spreading (docs/torch.md): an unlit torch catches fire once a lit
// torch of the same group has stayed close for IGNITE_HOLD_MS. Lit torches
// broadcast a TorchFlameMsg every BEACON_INTERVAL_MS; this class turns the
// received beacons into an ignite decision per light. Hardware-free: the
// caller passes in every beacon, the light's group and the time.
namespace torch {

// A beacon at or above this RSSI counts as "close".
static constexpr int8_t IGNITE_RSSI_DBM = -45;
// How long a lit torch has to stay close before the flame spreads.
static constexpr uint32_t IGNITE_HOLD_MS = 1500;
// How often a device with a lit torch broadcasts its flame.
static constexpr uint32_t BEACON_INTERVAL_MS = 200;
// A flame stays close this long after its last close beacon, so a single
// lost or weak beacon does not restart the hold.
static constexpr uint32_t CLOSE_GAP_MS = 3 * BEACON_INTERVAL_MS;
// Distinct flame senders tracked at once (same as PeerRegistry::MAX_PEERS).
static constexpr uint8_t MAX_FLAMES = 16;
// Group ids a TorchFlameMsg can carry (bit per group id).
static constexpr uint8_t MAX_FLAME_GROUPS = 8;

class Spread {
   public:
    // One received TorchFlameMsg: sender mac, its lit groups (bit per group
    // id) and the beacon's RSSI. A weak beacon only refreshes the sender's
    // groups; the sender stays close until CLOSE_GAP_MS after its last close
    // beacon.
    void onBeacon(const uint8_t mac[6], uint8_t groupMask, int8_t rssi, uint32_t now) {
        Flame* f = _find(mac);
        if (!f) f = _slot(now);
        memcpy(f->mac, mac, 6);
        f->used = true;
        f->groupMask = groupMask;
        f->lastBeaconMs = now;
        if (rssi >= IGNITE_RSSI_DBM) {
            f->closeSinceValid = f->closeSinceValid && now - f->lastCloseMs <= CLOSE_GAP_MS;
            if (!f->closeSinceValid) {
                f->closeSinceMs = now;
                f->closeSinceValid = true;
            }
            f->lastCloseMs = now;
        }
    }

    // True once a flame of groupId has been close for IGNITE_HOLD_MS while
    // the light was unlit. unlitSinceMs: when the light went out (or became
    // a torch), so a torch put out next to a lit one waits the full hold.
    bool shouldIgnite(uint8_t groupId, uint32_t unlitSinceMs, uint32_t now) const {
        if (groupId >= MAX_FLAME_GROUPS) return false;
        if (now - unlitSinceMs < IGNITE_HOLD_MS) return false;
        for (const Flame& f : _flames) {
            if (!f.used || !f.closeSinceValid) continue;
            if (!(f.groupMask & (1u << groupId))) continue;
            if (now - f.lastCloseMs > CLOSE_GAP_MS) continue;
            if (now - f.closeSinceMs >= IGNITE_HOLD_MS) return true;
        }
        return false;
    }

   private:
    struct Flame {
        uint8_t mac[6] = {};
        bool used = false;
        uint8_t groupMask = 0;
        uint32_t lastBeaconMs = 0;
        uint32_t lastCloseMs = 0;
        uint32_t closeSinceMs = 0;
        bool closeSinceValid = false;
    };
    Flame _flames[MAX_FLAMES];

    Flame* _find(const uint8_t mac[6]) {
        for (Flame& f : _flames)
            if (f.used && memcmp(f.mac, mac, 6) == 0) return &f;
        return nullptr;
    }

    // A free slot, else the one heard from longest ago.
    Flame* _slot(uint32_t now) {
        Flame* oldest = &_flames[0];
        for (Flame& f : _flames) {
            if (!f.used) {
                f = Flame{};
                return &f;
            }
            if (now - f.lastBeaconMs > now - oldest->lastBeaconMs) oldest = &f;
        }
        *oldest = Flame{};
        return oldest;
    }
};

}  // namespace torch
