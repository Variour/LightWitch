#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>

#include "../game/GameTypes.h"
#include "../game/Link.h"
#include "MeshRoute.h"

// Namespace `mesh` on the USB link (docs/usb-link.md), any device on USB:
//   test    start or end the mesh test for the whole mesh from this device
//   power   full transmit power for the whole mesh, or back to adaptive
//   status  this device's place in the tree and its neighbours
//   nodes   the root's table, one `node` event per device; while the mesh
//           test runs, every new report follows as a `node` event
// Plain C++ plus ArduinoJson.
namespace meshnet {

static constexpr uint8_t kLinkVersion = 0;
static constexpr uint16_t kTestDefaultS = 300;
static constexpr uint16_t kTestMaxS = 3600;

inline const char* testModeName(TestMode m) {
    switch (m) {
        case TestMode::Tree:
            return "tree";
        case TestMode::Slow:
            return "slow";
        default:
            return "off";
    }
}

class MeshLink : public game::Link::Extension {
   public:
    using Link = game::Link;
    using ReqId = game::ReqId;

    void begin(Router* router) { _r = router; }

    Scope scope(const char* ns) const override {
        return strcmp(ns, "mesh") == 0 ? Scope::Device : Scope::None;
    }
    const char* names() const override { return "mesh"; }
    void addSpaces(JsonObject spaces) override { spaces["mesh"] = kLinkVersion; }

    void onHello() override { _markAll(); }

    void flushTable(Link& link, uint32_t now) override {
        if (!_r || !(_r->testActive() || _replay)) return;
        Router::Node* nodes = _r->nodesMut();
        char line[game::kEventLineMax];
        bool left = false;
        for (uint8_t i = 0; i < kMaxNodes; i++) {
            Router::Node& n = nodes[i];
            if (!n.used || !n.dirty) continue;
            int len = formatNode(n, *_r, now, line, sizeof(line));
            if (len <= 0 || len >= (int)sizeof(line)) {
                n.dirty = false;
                continue;
            }
            if (!link.writeNow(line, (size_t)len)) {
                left = true;
                break;
            }
            n.dirty = false;
        }
        if (!left) _replay = false;
    }

    void command(Link& link, const char* ns, const char* cmd, JsonObjectConst o, const ReqId& id,
                 uint32_t now) override {
        (void)ns;
        if (!_r) return link.error(id, "unsupported", "mesh", cmd, nullptr, "no mesh router");
        if (strcmp(cmd, "test") == 0) return _test(link, o, id, now);
        if (strcmp(cmd, "power") == 0) return _power(link, o, id);
        if (strcmp(cmd, "status") == 0) return _status(link, id, now);
        if (strcmp(cmd, "nodes") == 0) {
            _markAll();
            JsonDocument d;
            Link::head(d, "mesh", "nodes", id);
            d["root"] = _r->isRoot();
            uint8_t count = 0;
            for (uint8_t i = 0; i < kMaxNodes; i++) count += _r->nodes()[i].used ? 1 : 0;
            d["count"] = count;
            link.emit(d);
            return;
        }
        link.error(id, "unknown_cmd", "mesh", cmd, nullptr, "test, power, status or nodes");
    }

    // One `mesh node` line with its newline; returns its length.
    static int formatNode(const Router::Node& n, const Router& r, uint32_t now, char* out,
                          size_t size) {
        char mac[18], parent[24], num[8], hops[8], bat[8];
        game::formatMac(n.mac, mac);
        static const uint8_t zero[6] = {};
        if (memcmp(n.parent, zero, 6) == 0) {
            strcpy(parent, "null");
        } else {
            char p[18];
            game::formatMac(n.parent, p);
            snprintf(parent, sizeof(parent), "\"%s\"", p);
        }
        _numOrNull(num, n.number != 0, n.number);
        _numOrNull(hops, n.hops != kNoHops, n.hops);
        _numOrNull(bat, n.battery != kNoBattery, n.battery);
        return snprintf(out, size,
                        "{\"ns\":\"mesh\",\"ev\":\"node\",\"mac\":\"%s\",\"n\":%s,\"parent\":%s,"
                        "\"hops\":%s,\"rssi\":%d,\"tx\":%d,\"children\":%u,\"neighbors\":%u,"
                        "\"bat\":%s,\"usb\":%s,\"mobile\":%s,\"sent\":%u,\"relayed\":%u,"
                        "\"failed\":%u,\"switches\":%u,\"lost\":%s,\"age\":%lu}\n",
                        mac, num, parent, hops, n.parentRssi, n.txDbm, n.children, n.neighbors, bat,
                        _b(n.flags & kFlagUsb), _b(n.flags & kFlagMobile), n.sent, n.relayed,
                        n.failed, n.switches, _b(r.nodeLost(n, now)),
                        (unsigned long)((now - n.heardMs) / 1000));
    }

   private:
    Router* _r = nullptr;
    bool _replay = false;

    static const char* _b(bool v) { return v ? "true" : "false"; }
    static void _numOrNull(char* out, bool present, long v) {
        if (present)
            snprintf(out, 8, "%ld", v);
        else
            strcpy(out, "null");
    }

    void _markAll() {
        if (!_r) return;
        Router::Node* nodes = _r->nodesMut();
        for (uint8_t i = 0; i < kMaxNodes; i++)
            if (nodes[i].used) nodes[i].dirty = true;
        _replay = true;
    }

    void _test(Link& link, JsonObjectConst o, const ReqId& id, uint32_t now) {
        const char* mode = o["mode"].is<const char*>() ? o["mode"].as<const char*>() : nullptr;
        TestMode m;
        if (mode && strcmp(mode, "tree") == 0)
            m = TestMode::Tree;
        else if (mode && strcmp(mode, "slow") == 0)
            m = TestMode::Slow;
        else if (mode && strcmp(mode, "off") == 0)
            m = TestMode::Off;
        else
            return link.error(id, "bad_field", "mesh", "test", "mode",
                              "mode must be tree, slow or off");
        long long s = kTestDefaultS;
        Link::Field f = Link::intField(o, "s", 10, kTestMaxS, s);
        if (f == Link::Field::Bad || f == Link::Field::Null)
            return link.error(id, "bad_field", "mesh", "test", "s", "s must be 10-3600");
        _r->startTest(m, (uint16_t)s, now);
        if (m != TestMode::Off) _markAll();
        JsonDocument d;
        Link::head(d, "mesh", "test", id);
        d["mode"] = testModeName(m);
        d["s"] = m == TestMode::Off ? 0 : (int)s;
        link.emit(d);
    }

    void _power(Link& link, JsonObjectConst o, const ReqId& id) {
        const char* mode = o["mode"].is<const char*>() ? o["mode"].as<const char*>() : nullptr;
        if (!mode || (strcmp(mode, "auto") != 0 && strcmp(mode, "max") != 0))
            return link.error(id, "bad_field", "mesh", "power", "mode", "mode must be auto or max");
        _r->setPowerMax(strcmp(mode, "max") == 0);
        JsonDocument d;
        Link::head(d, "mesh", "power", id);
        d["mode"] = mode;
        link.emit(d);
    }

    void _status(Link& link, const ReqId& id, uint32_t now) {
        JsonDocument d;
        Link::head(d, "mesh", "status", id);
        d["root"] = _r->isRoot();
        if (_r->hops() == kNoHops)
            d["hops"] = nullptr;
        else
            d["hops"] = _r->hops();
        char mac[18];
        if (const Router::Neighbor* p = _r->parent()) {
            game::formatMac(p->mac, mac);
            d["parent"] = mac;
            d["parentRssi"] = p->rssi;
        } else {
            d["parent"] = nullptr;
        }
        d["tx"] = _r->txDbm();
        d["txMax"] = _r->maxDbm();
        d["powerMax"] = _r->powerMax();
        d["children"] = _r->children();
        d["roundOpen"] = _r->roundOpen(now);
        d["test"] = testModeName(_r->testMode());
        d["testLeft"] = _r->testLeftMs(now) / 1000;
        d["sent"] = _r->sentCount();
        d["relayed"] = _r->relayedCount();
        d["failed"] = _r->failedCount();
        d["switches"] = _r->switchCount();
        d["overflow"] = _r->rxOverflow();
        JsonArray arr = d["neighbors"].to<JsonArray>();
        for (uint8_t i = 0; i < kMaxNeighbors; i++) {
            const Router::Neighbor& n = _r->neighbors()[i];
            if (!n.used) continue;
            JsonObject e = arr.add<JsonObject>();
            game::formatMac(n.mac, mac);
            e["mac"] = mac;
            if (n.number) e["n"] = n.number;
            if (n.hops == kNoHops)
                e["hops"] = nullptr;
            else
                e["hops"] = n.hops;
            e["rssi"] = n.rssi;
            e["loss"] = n.pl4 / 4;
            e["prr"] = n.prr * 100 / 255;
            e["age"] = (now - n.heardMs) / 1000;
        }
        link.emit(d);
    }
};

// Two extensions on one link: namespaces go to the first that claims them.
class LinkChain : public game::Link::Extension {
   public:
    void begin(game::Link::Extension* a, game::Link::Extension* b) {
        _a = a;
        _b = b;
        snprintf(_names, sizeof(_names), "%s, %s", a ? a->names() : "", b ? b->names() : "");
    }
    Scope scope(const char* ns) const override {
        if (_a) {
            Scope s = _a->scope(ns);
            if (s != Scope::None) return s;
        }
        return _b ? _b->scope(ns) : Scope::None;
    }
    const char* names() const override { return _names; }
    void addSpaces(JsonObject spaces) override {
        if (_a) _a->addSpaces(spaces);
        if (_b) _b->addSpaces(spaces);
    }
    void command(game::Link& link, const char* ns, const char* cmd, JsonObjectConst o,
                 const game::ReqId& id, uint32_t now) override {
        if (_a && _a->scope(ns) != Scope::None) return _a->command(link, ns, cmd, o, id, now);
        if (_b) _b->command(link, ns, cmd, o, id, now);
    }
    void onHello() override {
        if (_a) _a->onHello();
        if (_b) _b->onHello();
    }
    void flushTable(game::Link& link, uint32_t now) override {
        if (_a) _a->flushTable(link, now);
        if (_b) _b->flushTable(link, now);
    }

   private:
    game::Link::Extension* _a = nullptr;
    game::Link::Extension* _b = nullptr;
    char _names[64] = {};
};

}  // namespace meshnet
