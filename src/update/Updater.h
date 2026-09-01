#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFiClientSecure.h>

#include <atomic>
#include <vector>

#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../scenes/SceneManager.h"
#include "../sound/PlaylistManager.h"
#include "../version.h"
#include "CaBundle.h"

// GitHub API-based OTA updater.
//
// Checks the latest release in the configured repo, then flashes firmware
// and filesystem images if a newer version is available. A PAT is optional
// (Config::githubToken) — requests are sent unauthenticated if it's empty,
// which works fine against a public repo but is subject to GitHub's lower
// per-IP rate limit for unauthenticated API calls. If Config::prOtaEnabled
// is set and Config::prTrack names a PR
// (see applyPrAsync), the same check/apply flow instead follows that PR's
// prerelease (tag "pr-<n>") so a device already running a PR build keeps
// picking up newer pushes to that PR through the ordinary "Check"/"Install
// update" UI, without needing to reopen the PR picker each time.

class Updater {
   public:
    enum class State { Idle, Checking, Downloading, Error, Done };
    enum class PrListState { Idle, Loading, Done, Error };

    struct Status {
        State state = State::Idle;
        String currentVersion = FW_VERSION;
        String latestVersion;
        bool hasUpdate = false;
        int progress = 0;  // 0-100
        const char* error = nullptr;
    };

    struct PrBuild {
        int number = 0;
        String title;
        String tag;
    };

    struct PrListStatus {
        PrListState state = PrListState::Idle;
        std::vector<PrBuild> builds;
        const char* error = nullptr;
    };

    static Status& status() { return _status; }
    static PrListStatus& prListStatus() { return _prListStatus; }

    // True for the board families CI publishes PR prerelease builds for
    // (esp32dev, esp32s3 — see .github/workflows/firmware.yml). esp32c3 is
    // not built for PRs, so it stays excluded.
    static constexpr bool supportsPrOta() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
        return false;
#else
        return true;
#endif
    }

    // Non-blocking check: spawns a task. Safe to call from web handler.
    static void checkAsync() {
        if (_status.state == State::Checking || _status.state == State::Downloading) return;
        if (WiFi.status() != WL_CONNECTED) {
            _status = Status{};
            _status.state = State::Error;
            _status.error = "not connected to WiFi";
            return;
        }
        _status = Status{};
        _status.state = State::Checking;
        xTaskCreate(_checkTask, "fw_check", 8192, nullptr, 1, nullptr);
    }

    // Non-blocking update: spawns a task. Safe to call from web handler.
    // Installs whatever the last successful check found — the latest
    // tagged release, or the tracked PR's latest build if Config::prTrack
    // is set (see class comment).
    static void applyAsync() {
        if (_status.state == State::Downloading) return;
        if (!_status.hasUpdate) return;
        if (WiFi.status() != WL_CONNECTED) {
            _status.state = State::Error;
            _status.error = "not connected to WiFi";
            return;
        }
        _status.state = State::Downloading;
        _status.progress = 0;
        xTaskCreate(_applyTask, "fw_apply", 8192, nullptr, 1, nullptr);
    }

    // Trigger: applies immediately if update already known, otherwise checks first
    // and auto-applies once the check completes. Safe to call from web/mesh handler.
    static void triggerAsync() {
        if (_status.state == State::Downloading) return;
        if (WiFi.status() != WL_CONNECTED) {
            _status.state = State::Error;
            _status.error = "not connected to WiFi";
            return;
        }
        if (_status.hasUpdate) {
            applyAsync();
        } else {
            _triggerPending = true;
            if (_status.state != State::Checking) checkAsync();
        }
    }

    // Non-blocking: lists open PRs that currently have a published firmware
    // prerelease (tag "pr-<n>"). Only ever runs when explicitly requested —
    // gated on Config::prOtaEnabled so it costs nothing unless the user has
    // both opted in to the feature and pressed "Load open PRs".
    static void listPrBuildsAsync() {
        if (!Config::get().prOtaEnabled) {
            _prListStatus.state = PrListState::Error;
            _prListStatus.error = "PR installs are disabled";
            return;
        }
        if (_prListStatus.state == PrListState::Loading) return;
        if (WiFi.status() != WL_CONNECTED) {
            _prListStatus.state = PrListState::Error;
            _prListStatus.error = "not connected to WiFi";
            return;
        }
        _prListStatus.state = PrListState::Loading;
        _prListStatus.error = nullptr;
        xTaskCreate(_listPrBuildsTask, "fw_prlist", 8192, nullptr, 1, nullptr);
    }

    // Non-blocking: installs a specific build directly (no hasUpdate gate —
    // this is an explicit "install this" action, not a check-then-apply).
    // tag == "" installs the latest tagged release and clears the PR track;
    // tag == "pr-<n>" installs that PR's current build and starts tracking
    // it, so the ordinary check/apply flow follows it from then on. Gated
    // on Config::prOtaEnabled, same as listPrBuildsAsync().
    static void applyPrAsync(const String& tag) {
        if (!Config::get().prOtaEnabled) {
            _status.state = State::Error;
            _status.error = "PR installs are disabled";
            return;
        }
        if (_status.state == State::Downloading) return;
        if (WiFi.status() != WL_CONNECTED) {
            _status.state = State::Error;
            _status.error = "not connected to WiFi";
            return;
        }
        _pendingTag = tag;
        _status = Status{};
        _status.state = State::Downloading;
        _status.progress = 0;
        xTaskCreate(_applyPrTask, "fw_applypr", 8192, nullptr, 1, nullptr);
    }

   private:
    static Status _status;
    static PrListStatus _prListStatus;
    static std::atomic<bool> _triggerPending;
    static String _pendingTag;

    // Opens a GET request against the GitHub API and returns the HTTP status code.
    // Authenticates with the configured PAT if one is set, otherwise sends the
    // request unauthenticated (fine for a public repo, just a lower rate limit).
    // tls/http are owned by the caller and must stay alive while the response body/stream is read.
    static int _httpGet(WiFiClientSecure& tls, HTTPClient& http, const String& url,
                        const char* accept) {
        tls.setCACertBundle(CA_BUNDLE);
        http.begin(tls, url);
        if (strlen(Config::get().githubToken) > 0) {
            http.addHeader("Authorization", String("Bearer ") + Config::get().githubToken);
        }
        http.addHeader("Accept", accept);
        http.addHeader("X-GitHub-Api-Version", "2022-11-28");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        return http.GET();
    }

    // Fetches release metadata for `tag` ("" = /releases/latest, else
    // /releases/tags/<tag>). On success fills _firmwareAssetId/_fsAssetId
    // and outTagName, and returns true. Does not touch _status.hasUpdate/
    // latestVersion — callers interpret the result themselves, since a
    // normal release and a PR prerelease need different comparison logic
    // (see _checkForUpdate).
    static bool _fetchRelease(const String& tag, String& outTagName) {
        auto& c = Config::get();
        String url = "https://api.github.com/repos/";
        url += c.githubRepo;
        url += tag.isEmpty() ? "/releases/latest" : ("/releases/tags/" + tag);

        WiFiClientSecure tls;
        HTTPClient http;
        int code = _httpGet(tls, http, url, "application/vnd.github+json");
        if (code != 200) {
            Logger::e("[upd] releases API returned %d", code);
            snprintf(_errorBuf, sizeof(_errorBuf), "GitHub API error (HTTP %d)", code);
            _status.error = _errorBuf;
            http.end();
            return false;
        }

        String body = http.getString();
        http.end();

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            Logger::e("[upd] JSON parse error: %s", err.c_str());
            snprintf(_errorBuf, sizeof(_errorBuf), "JSON parse error: %s", err.c_str());
            _status.error = _errorBuf;
            return false;
        }

        const char* tagName = doc["tag_name"] | "";
        if (!tagName[0]) {
            _status.error = "no tag_name in release";
            return false;
        }
        outTagName = tagName;

        // Find asset IDs for firmware and filesystem
        _firmwareAssetId = 0;
        _fsAssetId = 0;
        for (JsonVariant asset : doc["assets"].as<JsonArray>()) {
            const char* name = asset["name"] | "";
            uint32_t id = asset["id"] | (uint32_t)0;
            if (!_firmwareAssetId && _isFirmwareAsset(name)) _firmwareAssetId = id;
            if (!_fsAssetId && strcmp(name, "littlefs.bin") == 0) _fsAssetId = id;
        }

        return true;
    }

    // Used by the normal check/apply/trigger flow. Follows Config::prTrack
    // (the currently-tracked PR) when prOtaEnabled is on and a track is
    // set, otherwise behaves exactly as before: compares against the
    // latest tagged release.
    static bool _checkForUpdate() {
        auto& c = Config::get();
        bool tracking = c.prOtaEnabled && strlen(c.prTrack) > 0;
        String tag = tracking ? String(c.prTrack) : String();
        String tagName;
        if (!_fetchRelease(tag, tagName)) return false;

        if (!tracking) {
            // Strip leading 'v'
            _status.latestVersion = (tagName[0] == 'v') ? tagName.substring(1) : tagName;
            _status.hasUpdate = _status.latestVersion != FW_VERSION;
        } else {
            // Prerelease tags are stable ("pr-<n>") across pushes to the
            // same PR, so the tag itself can't signal "is there a newer
            // build" — compare the firmware asset's id instead, which
            // changes every time CI republishes the release.
            _status.latestVersion = tag;
            _status.hasUpdate = _firmwareAssetId != 0 && _firmwareAssetId != c.prTrackAssetId;
        }

        Logger::i("[upd] current=%s latest=%s hasUpdate=%d", FW_VERSION,
                  _status.latestVersion.c_str(), _status.hasUpdate);

        if (_status.hasUpdate) {
            if (!_firmwareAssetId) Logger::w("[upd] no firmware asset found in release");
            if (!tracking && !_fsAssetId) Logger::w("[upd] no littlefs asset found in release");
        }

        return true;
    }

    // Returns true if the asset name matches this device's firmware binary.
    static bool _isFirmwareAsset(const char* name) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
        return strcmp(name, "firmware-esp32c3.bin") == 0;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
        return strcmp(name, "firmware-esp32s3.bin") == 0;
#else
        return strcmp(name, "firmware-esp32dev.bin") == 0;
#endif
    }

    // Download a release asset by ID and feed it to the Update library.
    // type is U_FLASH or U_SPIFFS.
    static bool _downloadAndFlash(uint32_t assetId, int type, const char* label) {
        auto& c = Config::get();
        String url = "https://api.github.com/repos/";
        url += c.githubRepo;
        url += "/releases/assets/";
        url += assetId;

        WiFiClientSecure tls;
        HTTPClient http;
        int code = _httpGet(tls, http, url, "application/octet-stream");
        if (code != 200) {
            Logger::e("[upd] asset %u GET returned %d", assetId, code);
            _status.error = "asset download error";
            http.end();
            return false;
        }

        int total = http.getSize();
        Logger::i("[upd] flashing %s: %d bytes", label, total);

        if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN, type)) {
            Logger::e("[upd] Update.begin failed: %s", Update.errorString());
            _status.error = "Update.begin failed";
            http.end();
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[512];
        int written = 0;

        while (http.connected() && (total < 0 || written < total)) {
            int avail = stream->available();
            if (!avail) {
                delay(1);
                continue;
            }
            int chunk = stream->readBytes(buf, min((int)sizeof(buf), avail));
            if (chunk <= 0) break;
            if (Update.write(buf, chunk) != (size_t)chunk) {
                Logger::e("[upd] Update.write mismatch at %d", written);
                _status.error = "flash write error";
                http.end();
                return false;
            }
            written += chunk;
            if (total > 0) _status.progress = written * 100 / total;
        }
        http.end();

        if (!Update.end(true)) {
            Logger::e("[upd] Update.end failed: %s", Update.errorString());
            _status.error = "Update.end failed";
            return false;
        }

        Logger::i("[upd] %s flash OK (%d bytes)", label, written);
        return true;
    }

    static void _checkTask(void*) {
        bool ok = _checkForUpdate();
        if (!ok && !_status.error) _status.error = "check failed";
        _status.state = ok ? State::Idle : State::Error;
        if (_triggerPending && _status.hasUpdate) {
            _triggerPending = false;
            applyAsync();
        } else {
            _triggerPending = false;
        }
        vTaskDelete(nullptr);
    }

    // Read all scene files from LittleFS into memory before the FS image overwrites them.
    static std::vector<String> _backupScenes() {
        std::vector<String> result;
        File dir = LittleFS.open("/sc");
        if (!dir || !dir.isDirectory()) {
            dir.close();
            return result;
        }
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String data = f.readString();
                if (data.length() > 0) result.push_back(std::move(data));
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
        Logger::i("[upd] backed up %u scene(s)", (unsigned)result.size());
        return result;
    }

    // Remounts LittleFS after the new filesystem image has been flashed, so scene/
    // playlist restoration below sees the freshly-flashed filesystem instead of a
    // stale pre-flash mount. Shared by _restoreScenes/_restorePlaylists — call once
    // before either, only when there's actually something to restore.
    static bool _remountLittleFsAfterFlash() {
        LittleFS.end();
        if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
            Logger::e("[upd] failed to remount LittleFS — restore skipped");
            return false;
        }
        return true;
    }

    static void _restoreScenes(const std::vector<String>& scenes) {
        SceneManager::init();
        unsigned restored = 0;
        for (const String& json : scenes) {
            if (SceneManager::save(json.c_str(), json.length())) restored++;
        }
        Logger::i("[upd] restored %u/%u scene(s)", restored, (unsigned)scenes.size());
    }

    // Read all playlist files from LittleFS into memory before the FS image
    // overwrites them — mirrors _backupScenes()/_restoreScenes(), see there.
    static std::vector<String> _backupPlaylists() {
        std::vector<String> result;
        File dir = LittleFS.open("/pl");
        if (!dir || !dir.isDirectory()) {
            dir.close();
            return result;
        }
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String data = f.readString();
                if (data.length() > 0) result.push_back(std::move(data));
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
        Logger::i("[upd] backed up %u playlist(s)", (unsigned)result.size());
        return result;
    }

    static void _restorePlaylists(const std::vector<String>& playlists) {
        PlaylistManager::init();
        unsigned restored = 0;
        for (const String& json : playlists) {
            if (PlaylistManager::save(json.c_str(), json.length())) restored++;
        }
        Logger::i("[upd] restored %u/%u playlist(s)", restored, (unsigned)playlists.size());
    }

    // Flashes whatever _firmwareAssetId/_fsAssetId currently point at.
    // Shared by the normal apply flow and explicit PR installs.
    static bool _flashAndMaybeReboot() {
        bool ok = true;
        if (_firmwareAssetId) {
            ok = _downloadAndFlash(_firmwareAssetId, U_FLASH, "firmware");
        }
        if (ok && _fsAssetId) {
            _status.progress = 0;
            std::vector<String> scenes = _backupScenes();
            std::vector<String> playlists = _backupPlaylists();
            ok = _downloadAndFlash(_fsAssetId, U_SPIFFS, "filesystem");
            if (ok && (!scenes.empty() || !playlists.empty()) && _remountLittleFsAfterFlash()) {
                if (!scenes.empty()) _restoreScenes(scenes);
                if (!playlists.empty()) _restorePlaylists(playlists);
            }
        }
        return ok;
    }

    static void _applyTask(void*) {
        // Re-fetch to get asset IDs if needed
        if (!_firmwareAssetId && !_fsAssetId) {
            if (!_checkForUpdate()) {
                _status.state = State::Error;
                vTaskDelete(nullptr);
                return;
            }
        }

        bool ok = _flashAndMaybeReboot();

        if (ok) {
            // If this device is tracking a PR, remember which build we just
            // installed so the next check doesn't immediately re-offer it.
            auto& c = Config::get();
            if (c.prOtaEnabled && strlen(c.prTrack) > 0) {
                c.prTrackAssetId = _firmwareAssetId;
                Config::save();
            }
            Logger::i("[upd] update complete — rebooting");
            _status.state = State::Done;
            delay(500);
            ESP.restart();
        } else {
            _status.state = State::Error;
        }
        vTaskDelete(nullptr);
    }

    static void _applyPrTask(void*) {
        String tag = _pendingTag;
        String tagName;
        if (!_fetchRelease(tag, tagName)) {
            _status.state = State::Error;
            vTaskDelete(nullptr);
            return;
        }
        if (!_firmwareAssetId) {
            _status.error = "no firmware asset found for that build";
            _status.state = State::Error;
            vTaskDelete(nullptr);
            return;
        }

        bool ok = _flashAndMaybeReboot();

        if (ok) {
            auto& c = Config::get();
            strlcpy(c.prTrack, tag.c_str(), sizeof(c.prTrack));
            c.prTrackAssetId = tag.isEmpty() ? 0 : _firmwareAssetId;
            Config::save();
            Logger::i("[upd] PR install complete (tag=%s) — rebooting",
                      tag.isEmpty() ? "latest" : tag.c_str());
            _status.state = State::Done;
            delay(500);
            ESP.restart();
        } else {
            _status.state = State::Error;
        }
        vTaskDelete(nullptr);
    }

    // Fetches the numbers of currently open pull requests. Returns false
    // (and sets _prListStatus.error) only if the request itself failed —
    // zero open PRs is not an error.
    static bool _fetchOpenPrNumbers(std::vector<int>& outNumbers) {
        auto& c = Config::get();
        String url = "https://api.github.com/repos/";
        url += c.githubRepo;
        url += "/pulls?state=open&per_page=100";

        WiFiClientSecure tls;
        HTTPClient http;
        int code = _httpGet(tls, http, url, "application/vnd.github+json");
        if (code != 200) {
            Logger::e("[upd] pulls list API returned %d", code);
            snprintf(_prListErrorBuf, sizeof(_prListErrorBuf), "GitHub API error (HTTP %d)", code);
            _prListStatus.error = _prListErrorBuf;
            http.end();
            return false;
        }

        // Each PR object also carries its body, labels, user, head/base repo
        // info, etc. — far more JSON than the device can buffer for up to
        // 100 open PRs, even before parsing. Filter down to the one field we
        // need and stream straight from the response instead of buffering
        // the whole body into a String first.
        JsonDocument filter;
        filter[0]["number"] = true;

        JsonDocument doc;
        DeserializationError err =
            deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
        http.end();
        if (err) {
            Logger::e("[upd] JSON parse error: %s", err.c_str());
            snprintf(_prListErrorBuf, sizeof(_prListErrorBuf), "JSON parse error: %s", err.c_str());
            _prListStatus.error = _prListErrorBuf;
            return false;
        }

        for (JsonVariant pr : doc.as<JsonArray>()) {
            int number = pr["number"] | 0;
            if (number > 0) outNumbers.push_back(number);
        }
        return true;
    }

    // Looks up the release for one PR's tag (pr-<n>). A 404 just means that
    // PR has no published build yet (e.g. CI hasn't touched firmware-relevant
    // paths) and is not treated as an error; other failures are logged but
    // likewise just skip this PR rather than aborting the whole list.
    static bool _fetchPrRelease(const String& tag, int number, PrBuild& out) {
        auto& c = Config::get();
        String url = "https://api.github.com/repos/";
        url += c.githubRepo;
        url += "/releases/tags/";
        url += tag;

        WiFiClientSecure tls;
        HTTPClient http;
        int code = _httpGet(tls, http, url, "application/vnd.github+json");
        if (code == 404) {
            http.end();
            return false;
        }
        if (code != 200) {
            Logger::w("[upd] release lookup for %s returned %d", tag.c_str(), code);
            http.end();
            return false;
        }

        String body = http.getString();
        http.end();

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            Logger::w("[upd] JSON parse error for %s: %s", tag.c_str(), err.c_str());
            return false;
        }

        out.number = number;
        out.tag = tag;
        out.title = doc["name"] | tag.c_str();
        return true;
    }

    // Lists open PRs that currently have a published firmware prerelease.
    // Queries open PR numbers first, then looks up each one's release by its
    // exact tag — small, targeted requests instead of paging through every
    // release this repo has ever published (whose bodies/assets can add up
    // to more JSON than the device can reliably buffer and parse).
    static void _listPrBuildsTask(void*) {
        std::vector<int> prNumbers;
        if (!_fetchOpenPrNumbers(prNumbers)) {
            _prListStatus.state = PrListState::Error;
            vTaskDelete(nullptr);
            return;
        }

        std::vector<PrBuild> builds;
        for (int number : prNumbers) {
            PrBuild b;
            if (_fetchPrRelease("pr-" + String(number), number, b)) builds.push_back(std::move(b));
        }

        _prListStatus.builds = std::move(builds);
        _prListStatus.state = PrListState::Done;
        Logger::i("[upd] found %u open PR build(s)", (unsigned)_prListStatus.builds.size());
        vTaskDelete(nullptr);
    }

    static uint32_t _firmwareAssetId;
    static uint32_t _fsAssetId;
    static char _errorBuf[48];
    static char _prListErrorBuf[48];
};

inline Updater::Status Updater::_status;
inline Updater::PrListStatus Updater::_prListStatus;
inline std::atomic<bool> Updater::_triggerPending{false};
inline String Updater::_pendingTag;
inline uint32_t Updater::_firmwareAssetId = 0;
inline uint32_t Updater::_fsAssetId = 0;
inline char Updater::_errorBuf[48] = {};
inline char Updater::_prListErrorBuf[48] = {};
