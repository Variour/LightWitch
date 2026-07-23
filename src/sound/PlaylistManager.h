#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_random.h>

#include <set>

#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../mesh/MeshTypes.h"

// Persisted, named playlist: an ordered list of SD-card filenames + a loop
// flag. Mirrors SceneManager's LittleFS storage/id/CRC32/tombstone model
// (see src/scenes/SceneManager.h) so playlist metadata can be synced over
// mesh the same way scenes are — see PlaylistSyncManager. Deliberately not
// shared code with SceneManager: playlists and scenes are independent
// entities that happen to reuse the same proven storage pattern.
//
// A playlist only ever references audio files by filename — it does not
// carry or distribute the files themselves. Playing a playlist on a device
// missing one of its files simply skips that entry on that device (see
// PlayAudioMsg).
class PlaylistManager {
   public:
    static constexpr uint8_t MAX_FILES_PER_PLAYLIST = 32;

    static bool extractId(const char* json, size_t len, String& out) {
        return _extractId(json, len, out);
    }

    static void init() {
        if (!LittleFS.exists("/pl")) LittleFS.mkdir("/pl");
    }

    // Build a JSON object {playlists:[{id,name,loop,files:[...]}, ...]} for the
    // list endpoint. Playlist files are small (a handful of filenames), unlike
    // scene frame data, so the full content is returned — no metadata filtering.
    static void buildList(JsonDocument& resp) {
        JsonArray arr = resp["playlists"].to<JsonArray>();
        File dir = LittleFS.open("/pl");
        if (!dir || !dir.isDirectory()) {
            Logger::w("[playlist] buildList: /pl dir missing or not a directory");
            dir.close();
            return;
        }
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, f);
                if (err) {
                    Logger::w("[playlist] skip invalid %s: %s", f.name(), err.c_str());
                    f.close();
                    f = dir.openNextFile();
                    continue;
                }
                if (!doc["id"].isNull()) {
                    JsonObject o = arr.add<JsonObject>();
                    o["id"] = doc["id"];
                    o["name"] = doc["name"];
                    o["loop"] = doc["loop"] | false;
                    JsonArray files = o["files"].to<JsonArray>();
                    for (JsonVariant v : doc["files"].as<JsonArray>()) files.add(v.as<String>());
                } else {
                    Logger::w("[playlist] list: file %s has no id field, skipping", f.name());
                }
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    // Create a new empty playlist. Returns the new id, or "" on failure.
    static String create(const char* name) {
        String id = _makeId();
        init();
        JsonDocument doc;
        doc["id"] = id;
        doc["name"] = name;
        doc["loop"] = false;
        doc["files"].to<JsonArray>();
        File f = LittleFS.open(_path(id.c_str()), "w");
        if (!f) return "";
        serializeJson(doc, f);
        f.close();
        removeTombstone(id.c_str());
        Logger::i("[playlist] created %s \"%s\"", id.c_str(), name);
        return id;
    }

    // Write raw JSON bytes sent by the browser directly to the playlist file.
    // The browser is responsible for including a valid "id" field.
    static bool save(const char* json, size_t len) {
        String id;
        if (!_extractId(json, len, id)) {
            Logger::e("[playlist] save: missing or invalid id");
            return false;
        }

        init();
        File f = LittleFS.open(_path(id.c_str()), "w");
        if (!f) {
            Logger::e("[playlist] save: unable to open %s for write", id);
            return false;
        }

        size_t written = f.write((const uint8_t*)json, len);
        f.close();

        if (written != len) {
            Logger::e("[playlist] save: write incomplete for %s (%u/%u bytes)", id,
                      (unsigned)written, (unsigned)len);
            return false;
        }

        removeTombstone(id.c_str());
        Logger::d("[playlist] saved %s (%u bytes)", id, (unsigned)len);
        return true;
    }

    // Save raw bytes received over the mesh for a known playlist ID.
    static bool saveRaw(const char* id, const uint8_t* data, size_t len) {
        init();
        File f = LittleFS.open(_path(id), "w");
        if (!f) {
            Logger::e("[playlist] saveRaw: unable to open %s for write", id);
            return false;
        }
        size_t written = f.write(data, len);
        f.close();
        if (written != len) {
            Logger::e("[playlist] saveRaw: write incomplete for %s (%u/%u bytes)", id,
                      (unsigned)written, (unsigned)len);
            return false;
        }
        removeTombstone(id);
        Logger::d("[playlist] saveRaw %s (%u bytes)", id, (unsigned)len);
        return true;
    }

    static bool remove(const char* id) {
        bool ok = LittleFS.remove(_path(id));
        if (ok) addTombstone(id);
        return ok;
    }

    static String path(const char* id) { return _path(id); }

    // Reads a playlist's ordered file list into `out` (up to maxFiles entries),
    // and its loop flag into `loop`. Returns the number of files, or 0 if the
    // playlist doesn't exist / is empty.
    static uint8_t readFiles(const char* id, String* out, uint8_t maxFiles, bool& loop) {
        File f = LittleFS.open(_path(id), "r");
        if (!f) return 0;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return 0;
        loop = doc["loop"] | false;
        uint8_t count = 0;
        for (JsonVariant v : doc["files"].as<JsonArray>()) {
            if (count >= maxFiles) break;
            out[count++] = v.as<String>();
        }
        return count;
    }

    // ── CRC32 ────────────────────────────────────────────────────────────────

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
        Logger::d("[playlist] tombstone added: %s", id);
    }

    static void removeTombstone(const char* id) { _tombstones().erase(String(id)); }

    static bool isTombstone(const char* id) { return _tombstones().count(String(id)) > 0; }

    struct ManifestEntry {
        char id[PLAYLIST_ID_LEN];
        uint32_t hash;
    };

    static uint8_t buildManifestEntries(ManifestEntry* entries, uint8_t maxEntries) {
        uint8_t count = 0;
        File dir = LittleFS.open("/pl");
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f && count < maxEntries) {
                if (!f.isDirectory()) {
                    String fname = String(f.name());
                    if (!fname.startsWith(".") && fname.endsWith(".json")) {
                        JsonDocument filter;
                        filter["id"] = true;
                        JsonDocument doc;
                        DeserializationError err =
                            deserializeJson(doc, f, DeserializationOption::Filter(filter));
                        if (!err && !doc["id"].isNull()) {
                            const char* id = doc["id"];
                            strlcpy(entries[count].id, id, PLAYLIST_ID_LEN);
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
        for (const String& tid : _tombstones()) {
            if (count >= maxEntries) break;
            strlcpy(entries[count].id, tid.c_str(), PLAYLIST_ID_LEN);
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
    // produces, so a client- or peer-supplied id can never escape /pl/ via '/', '\',
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

    static String _path(const char* id) {
        if (!_isValidId(id)) return String("/pl/invalid");
        return String("/pl/") + id + ".json";
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
