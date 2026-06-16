#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "../logging/Logger.h"

class SceneManager {
public:
    static bool extractId(const char* json, size_t len, String& out) {
        return _extractId(json, len, out);
    }

    static void init() {
        if (!LittleFS.exists("/scenes")) LittleFS.mkdir("/scenes");
    }

    // Build a JSON object {scenes:[{id,name,w,h,fc},...]} for the list endpoint.
    // Reads only metadata fields from each scene file (via ArduinoJson filter).
    static void buildList(JsonDocument& resp) {
        JsonArray arr = resp["scenes"].to<JsonArray>();
        File dir = LittleFS.open("/scenes");
        if (!dir || !dir.isDirectory()) {
            Logger::w("[scene] buildList: /scenes dir missing or not a directory");
            dir.close();
            return;
        }
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonDocument filter;
                filter["id"]   = true;
                filter["name"] = true;
                filter["w"]    = true;
                filter["h"]    = true;
                filter["fc"]   = true;
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
                if (err) {
                    Logger::w("[scene] skip invalid metadata in %s: %s", f.name(), err.c_str());
                    f.close();
                    f = dir.openNextFile();
                    continue;
                }
                if (!doc["id"].isNull()) {
                    Logger::d("[scene] list entry: %s \"%s\" %ux%u fc=%u",
                              doc["id"].as<const char*>(), doc["name"].as<const char*>(),
                              (unsigned)(doc["w"] | 0), (unsigned)(doc["h"] | 0), (unsigned)(doc["fc"] | 0));
                    JsonObject o = arr.add<JsonObject>();
                    o["id"]   = doc["id"];
                    o["name"] = doc["name"];
                    o["w"]    = doc["w"];
                    o["h"]    = doc["h"];
                    o["fc"]   = doc["fc"] | 0;
                } else {
                    Logger::w("[scene] list: file %s has no id field, skipping", f.name());
                }
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    // Create a new empty scene. Returns the new id, or "" on failure.
    static String create(const char* name, uint16_t w, uint16_t h) {
        String id = _makeId();
        init();
        JsonDocument doc;
        doc["id"]   = id;
        doc["name"] = name;
        doc["w"]    = w;
        doc["h"]    = h;
        doc["fc"]   = 0;
        doc["frames"].to<JsonArray>();
        File f = LittleFS.open(_path(id.c_str()), "w");
        if (!f) return "";
        serializeJson(doc, f);
        f.close();
        Logger::i("[scene] created %s \"%s\" %ux%u", id.c_str(), name, w, h);
        return id;
    }

    // Write raw JSON bytes sent by the browser directly to the scene file.
    // The browser is responsible for including a valid "id" field.
    static bool save(const char* json, size_t len) {
        String id;
        if (!_extractId(json, len, id)) {
            Logger::e("[scene] save: missing or invalid id");
            return false;
        }

        init();
        File f = LittleFS.open(_path(id.c_str()), "w");
        if (!f) {
            Logger::e("[scene] save: unable to open %s for write", id);
            return false;
        }

        size_t written = f.write((const uint8_t*)json, len);
        f.close();

        if (written != len) {
            Logger::e("[scene] save: write incomplete for %s (%u/%u bytes)", id, (unsigned)written, (unsigned)len);
            return false;
        }

        Logger::d("[scene] saved %s (%u bytes)", id, (unsigned)len);
        return true;
    }

    static bool remove(const char* id) {
        return LittleFS.remove(_path(id));
    }

    static String path(const char* id) { return _path(id); }

private:
    static bool _extractId(const char* json, size_t len, String& out) {
        const char* end = json + len;
        const char* key = strstr(json, "\"id\"");
        if (!key || key + 4 >= end) return false;

        const char* colon = key + 3;
        while (colon < end && *colon != ':') ++colon;
        if (colon >= end) return false;

        const char* value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\n' || *value == '\r' || *value == '\t')) ++value;
        if (value >= end || *value != '"') return false;

        const char* str = value + 1;
        const char* strEnd = str;
        while (strEnd < end) {
            if (*strEnd == '\\') {
                strEnd += 2;
                continue;
            }
            if (*strEnd == '"') break;
            ++strEnd;
        }
        if (strEnd >= end) return false;

        out = String();
        for (const char* c = str; c < strEnd; ++c) {
            if (*c == '\\' && c + 1 < strEnd) {
                ++c;
                out += *c;
            } else {
                out += *c;
            }
        }
        return !out.isEmpty();
    }

    static String _path(const char* id) {
        return String("/scenes/") + id + ".json";
    }

    static String _makeId() {
        static uint8_t ctr = 0;
        char buf[10];
        snprintf(buf, sizeof(buf), "%07lx%02x", millis() & 0x0FFFFFFF, ctr++);
        return String(buf);
    }
};
