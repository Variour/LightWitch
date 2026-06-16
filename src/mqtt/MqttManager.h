#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "../config/Config.h"
#include "../logging/Logger.h"

class MqttManager {
public:
    using CommandCb = std::function<void(const LightConfig&)>;

    void setOnCommand(CommandCb cb) { _onCommand = cb; }

    void begin(const DeviceConfig& cfg) {
        if (strlen(cfg.mqttHost) == 0) return;

        strlcpy(_deviceName, cfg.deviceName, sizeof(_deviceName));
        strlcpy(_host,       cfg.mqttHost,   sizeof(_host));
        _port = cfg.mqttPort;
        strlcpy(_user, cfg.mqttUser,     sizeof(_user));
        strlcpy(_pass, cfg.mqttPassword, sizeof(_pass));

        // Derive a stable unique ID from the MAC
        uint8_t mac[6]; WiFi.macAddress(mac);
        snprintf(_uniqueId, sizeof(_uniqueId), "bl_%02x%02x%02x", mac[3], mac[4], mac[5]);

        // Topics
        char base[96];
        snprintf(base, sizeof(base), "batterylight/%s", _deviceName);
        snprintf(_stateTopic, sizeof(_stateTopic), "%s/state",        base);
        snprintf(_cmdTopic,   sizeof(_cmdTopic),   "%s/set",          base);
        snprintf(_availTopic, sizeof(_availTopic), "%s/availability", base);
        snprintf(_discTopic,  sizeof(_discTopic),  "homeassistant/light/%s/config", _uniqueId);

        _client.setClient(_wifi);
        _client.setServer(_host, _port);
        _client.setKeepAlive(30);
        _client.setBufferSize(1024);
        _instance = this;
        _client.setCallback([](char* t, uint8_t* p, unsigned int l) {
            if (_instance) _instance->_handleMsg(t, p, l);
        });

        _enabled = true;
        Logger::i("[mqtt] enabled — broker %s:%u  client %s", _host, _port, _deviceName);
        _connect();
    }

    void loop() {
        if (!_enabled) return;
        if (!_client.connected()) {
            uint32_t now = millis();
            if (now - _lastAttempt > 10000) { _lastAttempt = now; _connect(); }
            return;
        }
        _client.loop();
        if (_publishPending) { _publishPending = false; _doPublish(_pendingLight); }
    }

    // Queue a state publish — safe to call from any context.
    void publishState(const LightConfig& cfg) {
        _pendingLight   = cfg;
        _publishPending = true;
    }

    bool connected() { return _enabled && _client.connected(); }
    bool enabled() const { return _enabled; }

private:
    inline static MqttManager* _instance = nullptr;

    WiFiClient   _wifi;
    PubSubClient _client{_wifi};
    CommandCb    _onCommand;

    bool     _enabled       = false;
    bool     _publishPending = false;
    uint32_t _lastAttempt   = 0;
    LightConfig _pendingLight;

    char     _deviceName[32] = {};
    char     _uniqueId[24]   = {};
    char     _host[64]       = {};
    uint16_t _port           = 1883;
    char     _user[32]       = {};
    char     _pass[64]       = {};
    char     _stateTopic[128] = {};
    char     _cmdTopic[128]   = {};
    char     _availTopic[128] = {};
    char     _discTopic[160]  = {};

    void _connect() {
        Logger::i("[mqtt] connecting to %s:%u ...", _host, _port);
        bool ok = (strlen(_user) > 0)
            ? _client.connect(_deviceName, _user, _pass, _availTopic, 0, true, "offline")
            : _client.connect(_deviceName, nullptr, nullptr, _availTopic, 0, true, "offline");
        if (ok) {
            Logger::i("[mqtt] connected");
            _client.publish(_availTopic, "online", /*retain=*/true);
            _client.subscribe(_cmdTopic);
            _publishDiscovery();
        } else {
            Logger::w("[mqtt] connect failed rc=%d — retry in 10s", _client.state());
        }
    }

    void _publishDiscovery() {
        JsonDocument doc;
        doc["name"]       = _deviceName;
        doc["unique_id"]  = _uniqueId;
        doc["schema"]     = "json";
        doc["state_topic"]          = _stateTopic;
        doc["command_topic"]        = _cmdTopic;
        doc["availability_topic"]   = _availTopic;
        doc["payload_available"]    = "online";
        doc["payload_not_available"] = "offline";
        doc["brightness"]  = true;
        doc["color_mode"]  = true;
        doc["supported_color_modes"].to<JsonArray>().add("rgb");
        doc["effect"]      = true;
        auto fx = doc["effect_list"].to<JsonArray>();
        fx.add("Static"); fx.add("Breathing"); fx.add("Color Cycle");
        fx.add("Strobe");  fx.add("Candle");
        auto dev = doc["device"].to<JsonObject>();
        dev["name"]  = _deviceName;
        dev["model"] = "Battery Light";
        dev["manufacturer"] = "DIY";
        dev["identifiers"].to<JsonArray>().add(_uniqueId);

        String s; serializeJson(doc, s);
        _client.publish(_discTopic, s.c_str(), /*retain=*/true);
        Logger::i("[mqtt] discovery published to %s", _discTopic);
    }

    void _doPublish(const LightConfig& cfg) {
        JsonDocument doc;
        doc["state"]      = cfg.brightness > 0 ? "ON" : "OFF";
        doc["brightness"] = cfg.brightness;
        doc["color_mode"] = "rgb";
        doc["effect"]     = _effectName(cfg.pattern);
        doc["speed"]      = cfg.speed;
        if (cfg.pattern != PatternId::Candle) {
            auto c = doc["color"].to<JsonObject>();
            c["r"] = cfg.color.r; c["g"] = cfg.color.g; c["b"] = cfg.color.b;
        }
        String s; serializeJson(doc, s);
        _client.publish(_stateTopic, s.c_str(), /*retain=*/true);
        Logger::d("[mqtt] state published");
    }

    void _handleMsg(char* topic, uint8_t* payload, unsigned int len) {
        if (strcmp(topic, _cmdTopic) != 0) return;
        JsonDocument doc;
        if (deserializeJson(doc, payload, len)) { Logger::w("[mqtt] bad command JSON"); return; }
        Logger::d("[mqtt] command rx");

        LightConfig cfg = Config::light();  // start from current state

        if (!doc["state"].isNull() && strcmp((const char*)doc["state"], "OFF") == 0)
            cfg.brightness = 0;
        if (!doc["brightness"].isNull()) cfg.brightness = (uint8_t)(int)doc["brightness"];
        if (!doc["color"].isNull()) {
            cfg.color.r = doc["color"]["r"] | (int)cfg.color.r;
            cfg.color.g = doc["color"]["g"] | (int)cfg.color.g;
            cfg.color.b = doc["color"]["b"] | (int)cfg.color.b;
        }
        if (!doc["effect"].isNull()) cfg.pattern = _effectId(doc["effect"]);
        if (!doc["speed"].isNull())  cfg.speed   = (float)doc["speed"];
        cfg.seq++;

        if (_onCommand) _onCommand(cfg);
    }

    static const char* _effectName(PatternId p) {
        switch (p) {
            case PatternId::Breathing:  return "Breathing";
            case PatternId::ColorCycle: return "Color Cycle";
            case PatternId::Strobe:     return "Strobe";
            case PatternId::Candle:     return "Candle";
            default:                    return "Static";
        }
    }

    static PatternId _effectId(const char* name) {
        if (strcmp(name, "Breathing")   == 0) return PatternId::Breathing;
        if (strcmp(name, "Color Cycle") == 0) return PatternId::ColorCycle;
        if (strcmp(name, "Strobe")      == 0) return PatternId::Strobe;
        if (strcmp(name, "Candle")      == 0) return PatternId::Candle;
        return PatternId::Static;
    }
};
