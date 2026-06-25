#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
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
        doc["fc"]   = 1;
        JsonArray frames = doc["frames"].to<JsonArray>();
        JsonArray defaultFrame = frames.add<JsonArray>();
        for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
            defaultFrame.add("000000");
        }
        File f = LittleFS.open(_path(id.c_str()), "w");
        if (!f) return "";
        serializeJson(doc, f);
        f.close();
        // Remove from tombstones if it was previously deleted
        removeTombstone(id.c_str());
        Logger::i("[scene] created %s \"%s\" %ux%u with default frame", id.c_str(), name, w, h);
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

        // Remove from tombstones if it was previously deleted
        removeTombstone(id.c_str());
        Logger::d("[scene] saved %s (%u bytes)", id, (unsigned)len);
        return true;
    }

    // Save raw bytes received over the mesh for a known scene ID.
    static bool saveRaw(const char* id, const uint8_t* data, size_t len) {
        init();
        File f = LittleFS.open(_path(id), "w");
        if (!f) {
            Logger::e("[scene] saveRaw: unable to open %s for write", id);
            return false;
        }
        size_t written = f.write(data, len);
        f.close();
        if (written != len) {
            Logger::e("[scene] saveRaw: write incomplete for %s (%u/%u bytes)", id, (unsigned)written, (unsigned)len);
            return false;
        }
        removeTombstone(id);
        Logger::d("[scene] saveRaw %s (%u bytes)", id, (unsigned)len);
        return true;
    }

    static bool remove(const char* id) {
        bool ok = LittleFS.remove(_path(id));
        if (ok) addTombstone(id);
        return ok;
    }

    static String path(const char* id) { return _path(id); }

    // ── CRC32 ────────────────────────────────────────────────────────────────

    // Compute CRC32 of a scene file. Returns 0 if file not found.
    static uint32_t crc32(const char* id) {
        File f = LittleFS.open(_path(id), "r");
        if (!f) return 0;
        uint32_t crc = 0xFFFFFFFF;
        uint8_t  buf[256];
        while (f.available()) {
            size_t n = f.read(buf, sizeof(buf));
            for (size_t i = 0; i < n; i++) {
                crc ^= buf[i];
                for (int b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
            }
        }
        f.close();
        uint32_t result = ~crc;
        // Map 0 to 1 so 0 remains exclusively the deletion sentinel
        return result == 0 ? 1 : result;
    }

    static uint32_t crc32OfData(const uint8_t* data, size_t len) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
        uint32_t result = ~crc;
        return result == 0 ? 1 : result;
    }

    // ── Tombstones ───────────────────────────────────────────────────────────

    static void addTombstone(const char* id) {
        if (isTombstone(id)) return;
        File f = LittleFS.open(_tombstonePath(), "a");
        if (!f) return;
        f.printf("%s\n", id);
        f.close();
        Logger::d("[scene] tombstone added: %s", id);
    }

    static void removeTombstone(const char* id) {
        if (!LittleFS.exists(_tombstonePath())) return;
        File f = LittleFS.open(_tombstonePath(), "r");
        if (!f) return;
        String out;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() > 0 && line != id)
                out += line + "\n";
        }
        f.close();
        File w = LittleFS.open(_tombstonePath(), "w");
        if (w) { w.print(out); w.close(); }
    }

    static bool isTombstone(const char* id) {
        if (!LittleFS.exists(_tombstonePath())) return false;
        File f = LittleFS.open(_tombstonePath(), "r");
        if (!f) return false;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line == id) { f.close(); return true; }
        }
        f.close();
        return false;
    }

    // Fill entries array with {id, hash} for all local scenes + tombstones.
    // Returns total count of entries written.
    struct ManifestEntry { char id[32]; uint32_t hash; };

    static uint8_t buildManifestEntries(ManifestEntry* entries, uint8_t maxEntries) {
        uint8_t count = 0;
        File dir = LittleFS.open("/scenes");
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f && count < maxEntries) {
                if (!f.isDirectory()) {
                    String fname = String(f.name());
                    if (!fname.startsWith(".") && fname.endsWith(".json")) {
                        // Read id field from the file (authoritative source)
                        JsonDocument filter; filter["id"] = true;
                        JsonDocument doc;
                        DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
                        if (!err && !doc["id"].isNull()) {
                            const char* id = doc["id"];
                            strlcpy(entries[count].id, id, 32);
                            entries[count].hash = crc32(id);
                            count++;
                        }
                    }
                }
                f.close();
                f = dir.openNextFile();
            }
            dir.close();
        }
        // Tombstones
        if (LittleFS.exists(_tombstonePath())) {
            File tf = LittleFS.open(_tombstonePath(), "r");
            if (tf) {
                while (tf.available() && count < maxEntries) {
                    String line = tf.readStringUntil('\n');
                    line.trim();
                    if (line.length() > 0) {
                        strlcpy(entries[count].id, line.c_str(), 32);
                        entries[count].hash = 0;
                        count++;
                    }
                }
                tf.close();
            }
        }
        return count;
    }

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

    static const char* _tombstonePath() { return "/scenes/.tombstones"; }

    static String _makeId() {
        static uint8_t ctr = 0;
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char buf[14];
        // Embed last 2 MAC bytes to make IDs globally unique across devices
        snprintf(buf, sizeof(buf), "%02x%02x%07lx%02x",
                 mac[4], mac[5], millis() & 0x0FFFFFFF, ctr++);
        return String(buf);  // 13 chars
    }
};
