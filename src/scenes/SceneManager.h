#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_random.h>

#include <set>
#include <vector>

#include "../config/Config.h"
#include "../logging/Logger.h"

class SceneManager {
   public:
    static bool extractId(const char* json, size_t len, String& out) {
        return _extractId(json, len, out);
    }

    // Reads a scene's frames as spatial pixel grids (row-major, w*h colors
    // each) plus its w/h dimensions, for renderers that map a scene onto a
    // light's physical layout (as opposed to GradientCommon::loadPalette's
    // flattened, non-spatial color list). Returns false, leaving frames
    // empty and w/h at 0, if the scene id is blank or the file is missing
    // or unparseable.
    static bool loadFrames(const char* sceneId, std::vector<std::vector<Color>>& frames,
                           uint16_t& w, uint16_t& h) {
        frames.clear();
        w = h = 0;
        if (!sceneId || !sceneId[0]) return false;
        File f = LittleFS.open(_path(sceneId), "r");
        if (!f) return false;
        JsonDocument doc;
        if (deserializeJson(doc, f)) {
            f.close();
            return false;
        }
        f.close();
        w = doc["w"] | (uint16_t)0;
        h = doc["h"] | (uint16_t)0;
        JsonArray fs = doc["frames"].as<JsonArray>();
        if (!fs || !fs.size()) return false;
        for (JsonArray fr : fs) {
            std::vector<Color> pixels;
            pixels.reserve(fr.size());
            for (JsonVariant v : fr) {
                const char* hex = v | "";
                if (strlen(hex) < 6) {
                    pixels.push_back({});
                    continue;
                }
                unsigned long rgb = strtoul(hex, nullptr, 16);
                pixels.push_back({(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb});
            }
            frames.push_back(std::move(pixels));
        }
        return true;
    }

    static void init() {
        if (!LittleFS.exists("/sc")) LittleFS.mkdir("/sc");
    }

    // Build a JSON object {scenes:[{id,name,w,h,fc},...]} for the list endpoint.
    // Reads only metadata fields from each scene file (via ArduinoJson filter).
    static void buildList(JsonDocument& resp) {
        JsonArray arr = resp["scenes"].to<JsonArray>();
        File dir = LittleFS.open("/sc");
        if (!dir || !dir.isDirectory()) {
            Logger::w("[scene] buildList: /sc dir missing or not a directory");
            dir.close();
            return;
        }
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonDocument filter;
                filter["id"] = true;
                filter["name"] = true;
                filter["w"] = true;
                filter["h"] = true;
                filter["fc"] = true;
                JsonDocument doc;
                DeserializationError err =
                    deserializeJson(doc, f, DeserializationOption::Filter(filter));
                if (err) {
                    Logger::w("[scene] skip invalid metadata in %s: %s", f.name(), err.c_str());
                    f.close();
                    f = dir.openNextFile();
                    continue;
                }
                if (!doc["id"].isNull()) {
                    Logger::d("[scene] list entry: %s \"%s\" %ux%u fc=%u",
                              doc["id"].as<const char*>(), doc["name"].as<const char*>(),
                              (unsigned)(doc["w"] | 0), (unsigned)(doc["h"] | 0),
                              (unsigned)(doc["fc"] | 0));
                    JsonObject o = arr.add<JsonObject>();
                    o["id"] = doc["id"];
                    o["name"] = doc["name"];
                    o["w"] = doc["w"];
                    o["h"] = doc["h"];
                    o["fc"] = doc["fc"] | 0;
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
        doc["id"] = id;
        doc["name"] = name;
        doc["w"] = w;
        doc["h"] = h;
        doc["fc"] = 1;
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
            Logger::e("[scene] save: write incomplete for %s (%u/%u bytes)", id, (unsigned)written,
                      (unsigned)len);
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
            Logger::e("[scene] saveRaw: write incomplete for %s (%u/%u bytes)", id,
                      (unsigned)written, (unsigned)len);
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
        uint8_t buf[256];
        while (f.available()) {
            size_t n = f.read(buf, sizeof(buf));
            crc = _crc32Update(crc, buf, n);
        }
        f.close();
        return _crc32Finalize(crc);
    }

    static uint32_t crc32OfData(const uint8_t* data, size_t len) {
        return _crc32Finalize(_crc32Update(0xFFFFFFFF, data, len));
    }

    // ── Tombstones (in-memory only, cleared on reboot) ───────────────────────

    static void addTombstone(const char* id) {
        _tombstones().insert(String(id));
        Logger::d("[scene] tombstone added: %s", id);
    }

    static void removeTombstone(const char* id) { _tombstones().erase(String(id)); }

    static bool isTombstone(const char* id) { return _tombstones().count(String(id)) > 0; }

    // Fill entries array with {id, hash} for all local scenes + tombstones.
    // Returns total count of entries written.
    struct ManifestEntry {
        char id[SCENE_ID_LEN];
        uint32_t hash;
    };

    static uint8_t buildManifestEntries(ManifestEntry* entries, uint8_t maxEntries) {
        uint8_t count = 0;
        File dir = LittleFS.open("/sc");
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f && count < maxEntries) {
                if (!f.isDirectory()) {
                    String fname = String(f.name());
                    if (!fname.startsWith(".") && fname.endsWith(".json")) {
                        // Read id field from the file (authoritative source)
                        JsonDocument filter;
                        filter["id"] = true;
                        JsonDocument doc;
                        DeserializationError err =
                            deserializeJson(doc, f, DeserializationOption::Filter(filter));
                        if (!err && !doc["id"].isNull()) {
                            const char* id = doc["id"];
                            strlcpy(entries[count].id, id, SCENE_ID_LEN);
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
        // Tombstones (in-memory)
        for (const String& tid : _tombstones()) {
            if (count >= maxEntries) break;
            strlcpy(entries[count].id, tid.c_str(), SCENE_ID_LEN);
            entries[count].hash = 0;
            count++;
        }
        return count;
    }

   private:
    static uint32_t _crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
        return crc;
    }

    // Map 0 to 1 so 0 remains exclusively the deletion sentinel
    static uint32_t _crc32Finalize(uint32_t crc) {
        uint32_t result = ~crc;
        return result == 0 ? 1 : result;
    }

    static bool _extractId(const char* json, size_t len, String& out) {
        const char* end = json + len;
        const char* key = strstr(json, "\"id\"");
        if (!key || key + 4 >= end) return false;

        const char* colon = key + 3;
        while (colon < end && *colon != ':') ++colon;
        if (colon >= end) return false;

        const char* value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\n' || *value == '\r' || *value == '\t'))
            ++value;
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
        return _isValidId(out.c_str());
    }

    // Reject ids containing anything other than the alphanumeric charset _makeId()
    // produces, so a client- or peer-supplied id can never escape /sc/ via '/', '\',
    // or '..'.
    static bool _isValidId(const char* id) {
        if (!id || !id[0]) return false;
        for (const char* c = id; *c; ++c) {
            bool alnum =
                (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z');
            if (!alnum) return false;
        }
        return true;
    }

    // Invalid ids map to a fixed, unwritable placeholder within /sc/ rather than an
    // empty string, so callers can't accidentally end up operating on the /sc/ or
    // LittleFS root regardless of how the underlying FS handles empty paths.
    static String _path(const char* id) {
        if (!_isValidId(id)) return String("/sc/invalid");
        return String("/sc/") + id + ".json";
    }

    static std::set<String>& _tombstones() {
        static std::set<String> s;
        return s;
    }

    static String _makeId() {
        uint8_t b[12];
        for (int i = 0; i < 3; i++) {
            uint32_t r = esp_random();
            memcpy(b + i * 4, &r, 4);
        }
        char buf[25];
        for (int i = 0; i < 12; i++) snprintf(buf + i * 2, 3, "%02x", b[i]);
        return String(buf);  // 24 hex chars = 96 bits of entropy
    }
};
