#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>

#include <functional>

#include "../led/LedDriver.h"
#include "../logging/Logger.h"
#include "Bridge.h"
#include "GameRender.h"
#include "Link.h"
#include "Participant.h"
#include "RxRing.h"

// Arduino side of the game session module (src/game/): NVS, serial port,
// radio channel, restart and LEDs around the plain-C++ Participant, Bridge
// and Link. main.cpp wires it through callbacks like every other module.
//
// Participant: a device with a buzz key joins a session on a buzz press while
// an invite is visible, then runs as a quiet participant until it is released
// and restarts. Bridge: any device becomes the bridge with the first
// `link hello` on its serial port and stays it until the next restart.
class GameManager {
   public:
    using SendFn = std::function<void(const uint8_t*, size_t)>;
    // participant = true: also take the LEDs and the keys from normal operation.
    using EnterQuietFn = std::function<void(bool participant)>;
    using HealthFn = std::function<game::Participant::Health()>;

    void setSendFn(SendFn f) { _send = f; }
    void setEnterQuietFn(EnterQuietFn f) { _enterQuiet = f; }
    void setHealthFn(HealthFn f) { _health = f; }

    // The light the session renders on: the first one with the Buzzerboard's
    // 34 LEDs.
    void setLight(LedDriver* led, uint16_t ledCount) {
        if (!_led && ledCount == game::kLedCount) _led = led;
    }

    // Only devices with a buzz key take invites (round format v0, "Einladung").
    void setHasBuzzKey(bool has) {
        _hasBuzzKey = has;
        _participant.setCanJoin(has && !_isBridge);
    }

    // Call before WiFi setup: reads the device number and the session entry.
    void loadStored() {
        Preferences p;
        if (p.begin(NVS_NS, /*readOnly=*/true)) {
            _number = p.getUShort(KEY_NUMBER, 0);
            _storedSession = p.getUShort(KEY_SESSION, 0);
            _storedChannel = p.getUChar(KEY_CHANNEL, 0);
            p.end();
        }
        if (_number > game::kMaxNumber) _number = 0;
    }

    // A stored session entry: boot without softAP, WiFi connection, channel
    // search and LightWitch traffic, and listen for that session's bridge.
    bool quietBoot() const {
        return game::Participant::wantsQuietBoot(_storedSession, _storedChannel);
    }

    // Call after mesh.begin().
    void begin(uint32_t now) {
        WiFi.macAddress(_mac);
        _inviteRx.allocate(2);
        if (quietBoot()) _allocRx(game::kRxQueueParticipant);

        game::Participant::Io pio;
        pio.send = [this](const uint8_t* d, size_t n) { _sendFrame(d, n); };
        pio.setChannel = [](uint8_t ch) { _setChannel(ch); };
        pio.randomRange = [](uint32_t lo, uint32_t hi) {
            return lo + esp_random() % (hi - lo + 1);
        };
        pio.storeNumber = [this](uint16_t n) { _storeNumber(n); };
        pio.storeSession = [this](uint16_t s, uint8_t ch) { _storeSession(s, ch); };
        pio.enterQuiet = [this]() {
            _allocRx(game::kRxQueueParticipant);
            if (_enterQuiet) _enterQuiet(true);
        };
        pio.restart = []() {
            Logger::i("[game] leaving the session, restarting into normal operation");
            ESP.restart();
        };
        pio.health = [this]() { return _health ? _health() : game::Participant::Health{}; };
        pio.fwBuild = [this]() { return _fwBuild(); };
        _participant.begin(pio, _mac, _number, _storedSession, _storedChannel, now);
        _participant.setCanJoin(_hasBuzzKey);

        game::Bridge::Io bio;
        bio.send = [this](const uint8_t* d, size_t n) { _sendFrame(d, n); };
        bio.setChannel = [](uint8_t ch) { _setChannel(ch); };
        bio.enterQuiet = [this]() {
            _allocRx(game::kRxQueueBridge);
            if (_enterQuiet) _enterQuiet(false);
        };
        _bridge.begin(bio, &_link);

        game::Link::Io lio;
        lio.fwBuild = [this]() { return _fwBuild(); };
        lio.loadNumber = [this]() { return _number; };
        lio.storeNumber = [this](uint16_t n) { _storeNumber(n); };
        lio.participantBusy = [this]() { return _participant.inSession(); };
        lio.becameBridge = [this]() {
            _isBridge = true;
            _participant.setCanJoin(false);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
            Serial.setTxTimeoutMs(0);  // never block loop() on a host that does not read
#endif
        };
        lio.rxOverflow = [this]() { return _rx.overflow(); };
        _link.begin(&_bridge, &_writer, lio, _mac);
        if (quietBoot())
            Logger::i("[game] session %u entry found, listening on ch %u", _storedSession,
                      _storedChannel);
    }

    // ESP-NOW receive callback (WiFi task): copy only, processed in tick().
    void onRadioFrame(const uint8_t* data, int len, int8_t rssi) {
        if (len <= 0) return;
        if (_rx.allocated())
            _rx.push(data, (size_t)len, rssi);
        else if (data[0] == game::kMsgQuizInvite)
            _inviteRx.push(data, (size_t)len, rssi);
    }

    // Raw key edge with its moment (millis()).
    void onKey(game::Key k, bool pressed, uint32_t ms) {
        _participant.onKey(k, pressed, ms, millis());
    }

    // Every loop() pass.
    void tick(uint32_t now) {
        for (int budget = 2048; budget > 0 && _link.readyForInput(); budget--) {
            int c = Serial.read();
            if (c < 0) break;
            _link.feedByte((uint8_t)c, now);
        }
        game::RxRing::Frame f;
        while (_inviteRx.pop(f)) _participant.onFrame(f.data, f.len, f.rssi, now);
        while (_rx.pop(f)) {
            _participant.onFrame(f.data, f.len, f.rssi, now);
            _bridge.onFrame(f.data, f.len, f.rssi, now);
        }
        _participant.tick(now);
        _bridge.tick(now);
        _link.tick(now);
    }

    // The radio belongs to the game: no softAP, WiFi, channel search, or
    // LightWitch mesh traffic.
    bool radioQuiet() const { return _participant.inSession() || _bridge.quiet(); }
    // The session renders the light exclusively; patterns and button actions pause.
    bool ownsLeds() const { return _participant.inSession(); }
    // Log lines stay off the serial port while the bridge link is open.
    bool logMuted() const { return _link.active(); }

    // Full frame while ownsLeds().
    void render(uint32_t now) {
        if (!_led) return;
        game::Rgb px[game::kLedCount];
        game::renderParticipant(_participant, now, px);
        for (uint16_t i = 0; i < game::kLedCount; i++) _led->setPixel(i, px[i].r, px[i].g, px[i].b);
        _led->show();
    }

    enum class Overlay : uint8_t { None, Drawn, Ended };

    // Invite ring over the normal pattern. Ended: the caller re-applies the
    // light's config so the pattern repaints the ring.
    Overlay renderOverlay(uint32_t now) {
        if (!_led) return Overlay::None;
        game::Rgb ring[4];
        if (game::renderInviteOverlay(_participant, now, ring)) {
            for (uint8_t i = 0; i < 4; i++)
                _led->setPixel(game::kRingLeds[i], ring[i].r, ring[i].g, ring[i].b);
            _led->show();
            _overlayOn = true;
            return Overlay::Drawn;
        }
        if (!_overlayOn) return Overlay::None;
        _overlayOn = false;
        return Overlay::Ended;
    }

   private:
    static constexpr const char* NVS_NS = "game";
    static constexpr const char* KEY_NUMBER = "num";
    static constexpr const char* KEY_SESSION = "sess";
    static constexpr const char* KEY_CHANNEL = "sch";

    struct SerialWriter : game::Link::Writer {
        int availableForWrite() override { return Serial.availableForWrite(); }
        void write(const char* d, size_t n) override { Serial.write((const uint8_t*)d, n); }
    };

    game::Participant _participant;
    game::Bridge _bridge;
    game::Link _link;
    game::RxRing _rx;        // all quiz frames, once in a session or bridging one
    game::RxRing _inviteRx;  // invites in normal operation
    SerialWriter _writer;
    SendFn _send;
    EnterQuietFn _enterQuiet;
    HealthFn _health;
    LedDriver* _led = nullptr;
    uint8_t _mac[6] = {};
    bool _hasBuzzKey = false;
    bool _isBridge = false;
    bool _overlayOn = false;
    uint16_t _number = 0;
    uint16_t _storedSession = 0;
    uint8_t _storedChannel = 0;
    bool _fwKnown = false;
    uint32_t _fw = 0;

    void _allocRx(uint8_t capacity) {
        if (!_rx.allocate(capacity)) Logger::e("[game] out of memory for the receive queue");
    }

    void _sendFrame(const uint8_t* d, size_t n) {
        if (_send) _send(d, n);
    }

    static void _setChannel(uint8_t ch) { esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }

    // Firmware build: the first 4 bytes of the running app image's SHA-256,
    // the same for every device flashed with the same binary.
    uint32_t _fwBuild() {
        if (!_fwKnown) {
            _fwKnown = true;
            uint8_t sha[32];
            const esp_partition_t* part = esp_ota_get_running_partition();
            if (part && esp_partition_get_sha256(part, sha) == ESP_OK)
                _fw = (uint32_t)sha[0] << 24 | (uint32_t)sha[1] << 16 | (uint32_t)sha[2] << 8 |
                      sha[3];
        }
        return _fw;
    }

    // Flash writes: only the number and the session entry, only on change.
    void _storeNumber(uint16_t n) {
        if (n == _number) return;
        _number = n;
        Preferences p;
        if (!p.begin(NVS_NS, false)) return;
        if (n == 0)
            p.remove(KEY_NUMBER);
        else
            p.putUShort(KEY_NUMBER, n);
        p.end();
    }

    void _storeSession(uint16_t session, uint8_t channel) {
        if (session == _storedSession && channel == _storedChannel) return;
        _storedSession = session;
        _storedChannel = channel;
        Preferences p;
        if (!p.begin(NVS_NS, false)) return;
        if (session == 0) {
            p.remove(KEY_SESSION);
            p.remove(KEY_CHANNEL);
        } else {
            p.putUShort(KEY_SESSION, session);
            p.putUChar(KEY_CHANNEL, channel);
        }
        p.end();
    }
};
