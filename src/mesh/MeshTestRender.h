#pragma once
#include <stdint.h>

#include "../game/LedEffects.h"
#include "../game/LedZones.h"
#include "MeshRoute.h"

// LED picture of the mesh test (MeshRoute.h) on a Buzzerboard. Plain C++.
//
//   ring       place in the tree: white = root, green 1 hop, cyan 2, blue 3,
//              violet 4, magenta 5 and more; red blinking = no mesh
//   column     received level of the parent as a bar, bottom red to top
//              green (-95 to -40 dBm); a white dot marks the own transmit
//              power (bottom 2 dBm, top full power). Root: one LED per child.
//              No mesh: one red LED per neighbour heard.
//   key LEDs   number of children (up, right, down, left)
//   status dot white flash: own frame sent; hop colour flash: frame from the parent
//   comet      a frame passed on: down the column toward the children, up
//              the column toward the root
//   flash      the root's trace wave passes; in slow motion every relay holds
//              it for kSlowHopMs, so the wave walks through the room
namespace meshnet {

inline game::Rgb hopColor(uint8_t hops) {
    switch (hops) {
        case 0:
            return {255, 255, 255};
        case 1:
            return {0, 255, 60};
        case 2:
            return {0, 200, 255};
        case 3:
            return {40, 60, 255};
        case 4:
            return {170, 0, 255};
        case kNoHops:
            return {255, 0, 0};
        default:
            return {255, 0, 160};
    }
}

// Red at the bottom over yellow to green at the top, n of count.
inline game::Rgb levelColor(uint8_t n, uint8_t count) {
    uint32_t t = count > 1 ? (uint32_t)n * 510 / (count - 1) : 510;
    if (t <= 255) return {255, (uint8_t)t, 0};
    return {(uint8_t)(510 - t), 255, 0};
}

inline uint32_t _ageOf(const Router& r, Router::Act k, uint32_t now) {
    for (uint8_t i = 0; i < Router::kActs; i++) {
        const Router::Activity& a = r.activity(i);
        if (a.at != 0 && a.kind == k) return now - a.at;
    }
    return 0xFFFFFFFF;
}

// One comet over the column: head at t/durMs of the way, 3 LEDs of tail.
inline void _comet(const game::LedZones& z, game::Rgb* px, uint32_t t, uint32_t durMs, bool up) {
    if (t >= durMs) return;
    int span = z.columnCount + 3;
    int pos = (int)(t * span / durMs);
    for (int k = 0; k < 4; k++) {
        int n = pos - k;
        if (n < 0 || n >= z.columnCount) continue;
        uint8_t lvl = (uint8_t)(255 - k * 70);
        uint8_t led = z.columnLed((uint8_t)n, /*reversed=*/!up);
        px[led] = game::blendRgb(px[led], {255, 255, 255}, lvl);
    }
}

inline void renderMeshTest(const Router& r, const game::LedZones& z, uint32_t now, game::Rgb* px) {
    using game::Rgb;
    game::allOff(z, px);
    const bool slow = r.testSlow();
    const uint32_t flashMs = slow ? 900 : 250;
    const uint32_t cometMs = slow ? 1200 : 300;

    if (!r.hasRoute()) {
        Rgb red = game::blinkOn(now) ? Rgb{255, 0, 0} : Rgb{40, 0, 0};
        game::fillRing(z, px, red);
        for (uint8_t i = 0; i < game::kKeySlots; i++)
            if (z.key[i] != game::kNoLed) px[z.key[i]] = red;
        uint8_t nb = r.neighborCount();
        for (uint8_t n = 0; n < nb && n < z.columnCount; n++)
            px[z.columnLed(n, false)] = {120, 0, 0};
        px[z.dot] = red;
        return;
    }

    const uint8_t hops = r.hops();
    const Rgb hc = hopColor(hops);
    game::fillRing(z, px, game::scaleRgb(hc, 140));

    if (r.isRoot()) {
        for (uint8_t n = 0; n < r.children() && n < z.columnCount; n++)
            px[z.columnLed(n, false)] = {90, 90, 90};
    } else if (const Router::Neighbor* p = r.parent()) {
        int q = ((int)p->rssi + 95) * z.columnCount / 55;
        if (q < 1) q = 1;
        if (q > z.columnCount) q = z.columnCount;
        for (uint8_t n = 0; n < q; n++)
            px[z.columnLed(n, false)] = game::scaleRgb(levelColor(n, z.columnCount), 90);
    }
    // Own transmit power.
    int span = r.maxDbm() - kMinDbm;
    int at = span > 0 ? (r.txDbm() - kMinDbm) * (z.columnCount - 1) / span : z.columnCount - 1;
    if (at < 0) at = 0;
    if (at >= z.columnCount) at = z.columnCount - 1;
    px[z.columnLed((uint8_t)at, false)] = {255, 255, 255};

    // Children on the key LEDs.
    static const game::Key kChildKeys[4] = {game::Key::Up, game::Key::Right, game::Key::Down,
                                            game::Key::Left};
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t led = z.keyLed(kChildKeys[i]);
        if (led == game::kNoLed) continue;
        px[led] = i < r.children() ? Rgb{200, 200, 200} : game::scaleRgb(hc, 40);
    }
    uint8_t enter = z.keyLed(game::Key::Enter);
    if (enter != game::kNoLed) px[enter] = game::scaleRgb(hc, 40);

    // Status dot.
    uint32_t tx = _ageOf(r, Router::Act::TxOwn, now);
    uint32_t rx = _ageOf(r, Router::Act::RxDown, now);
    px[z.dot] = game::scaleRgb(hc, 60);
    if (rx < flashMs) px[z.dot] = hc;
    if (tx < flashMs) px[z.dot] = {255, 255, 255};

    // Frames passed on.
    _comet(z, px, _ageOf(r, Router::Act::FwdDown, now), cometMs, /*up=*/false);
    _comet(z, px, _ageOf(r, Router::Act::FwdUp, now), cometMs, /*up=*/true);

    // Trace wave.
    uint32_t tr = _ageOf(r, Router::Act::Trace, now);
    if (tr < flashMs) {
        uint8_t lvl = (uint8_t)(255 - tr * 115 / flashMs);  // down to the ring's 140
        Rgb c = game::scaleRgb(hc, lvl);
        game::fillRing(z, px, c);
        for (uint8_t i = 0; i < game::kKeySlots; i++)
            if (z.key[i] != game::kNoLed) px[z.key[i]] = game::blendRgb(px[z.key[i]], c, lvl);
    }
}

}  // namespace meshnet
