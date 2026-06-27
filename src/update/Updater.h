#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <atomic>
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../version.h"

// GitHub API-based OTA updater.
// Checks the latest release in the configured private repo using a PAT,
// then flashes firmware and filesystem images if a newer version is available.

class Updater {
public:
    enum class State { Idle, Checking, Downloading, Error, Done };

    struct Status {
        State       state          = State::Idle;
        String      currentVersion = FW_VERSION;
        String      latestVersion;
        bool        hasUpdate      = false;
        int         progress       = 0;   // 0-100
        const char* error          = nullptr;
    };

    static Status& status() { return _status; }

    // Non-blocking check: spawns a task. Safe to call from web handler.
    static void checkAsync() {
        if (_status.state == State::Checking || _status.state == State::Downloading) return;
        _status = Status{};
        _status.state = State::Checking;
        xTaskCreate(_checkTask, "fw_check", 8192, nullptr, 1, nullptr);
    }

    // Non-blocking update: spawns a task. Safe to call from web handler.
    static void applyAsync() {
        if (_status.state == State::Downloading) return;
        if (!_status.hasUpdate) return;
        _status.state    = State::Downloading;
        _status.progress = 0;
        xTaskCreate(_applyTask, "fw_apply", 8192, nullptr, 1, nullptr);
    }

    // Trigger: applies immediately if update already known, otherwise checks first
    // and auto-applies once the check completes. Safe to call from web/mesh handler.
    static void triggerAsync() {
        if (_status.state == State::Downloading) return;
        if (_status.hasUpdate) {
            applyAsync();
        } else {
            _triggerPending = true;
            if (_status.state != State::Checking)
                checkAsync();
        }
    }

private:
    static Status _status;
    static std::atomic<bool> _triggerPending;

    static String _authHeader() {
        String h = "Bearer ";
        h += Config::get().githubToken;
        return h;
    }

    // Returns false on transport/parse error. Fills latestVersion and _firmwareAssetId/_fsAssetId.
    static bool _fetchLatestRelease() {
        auto& c = Config::get();
        if (strlen(c.githubToken) == 0) {
            _status.error = "no GitHub token configured";
            return false;
        }

        String url = "https://api.github.com/repos/";
        url += c.githubRepo;
        url += "/releases/latest";

        WiFiClientSecure tls;
        tls.setInsecure();
        HTTPClient http;
        http.begin(tls, url);
        http.addHeader("Authorization", _authHeader());
        http.addHeader("Accept", "application/vnd.github+json");
        http.addHeader("X-GitHub-Api-Version", "2022-11-28");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        http.setTimeout(15000);
        int code = http.GET();
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
            _status.error = "JSON parse error";
            return false;
        }

        const char* tag = doc["tag_name"] | "";
        if (!tag[0]) {
            _status.error = "no tag_name in release";
            return false;
        }

        // Strip leading 'v'
        _status.latestVersion = (tag[0] == 'v') ? String(tag + 1) : String(tag);
        _status.hasUpdate     = _status.latestVersion != FW_VERSION;

        Logger::i("[upd] current=%s latest=%s hasUpdate=%d",
                  FW_VERSION, _status.latestVersion.c_str(), _status.hasUpdate);

        // Find asset IDs for firmware and filesystem
        _firmwareAssetId = 0;
        _fsAssetId       = 0;
        for (JsonVariant asset : doc["assets"].as<JsonArray>()) {
            const char* name = asset["name"] | "";
            uint32_t    id   = asset["id"]   | (uint32_t)0;
            if (!_firmwareAssetId && _isFirmwareAsset(name)) _firmwareAssetId = id;
            if (!_fsAssetId       && strcmp(name, "littlefs.bin") == 0) _fsAssetId = id;
        }

        if (_status.hasUpdate) {
            if (!_firmwareAssetId) Logger::w("[upd] no firmware asset found in release");
            if (!_fsAssetId)       Logger::w("[upd] no littlefs asset found in release");
        }

        return true;
    }

    // Returns true if the asset name matches this device's firmware binary.
    static bool _isFirmwareAsset(const char* name) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
        return strcmp(name, "firmware-esp32c3.bin") == 0;
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
        tls.setInsecure();
        HTTPClient http;
        http.begin(tls, url);
        http.addHeader("Authorization", _authHeader());
        http.addHeader("Accept", "application/octet-stream");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        int code = http.GET();
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
        uint8_t     buf[512];
        int         written = 0;

        while (http.connected() && (total < 0 || written < total)) {
            int avail = stream->available();
            if (!avail) { delay(1); continue; }
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
        bool ok = _fetchLatestRelease();
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

    static void _applyTask(void*) {
        // Re-fetch to get asset IDs if needed
        if (!_firmwareAssetId && !_fsAssetId) {
            if (!_fetchLatestRelease()) {
                _status.state = State::Error;
                vTaskDelete(nullptr);
                return;
            }
        }

        bool ok = true;
        if (_firmwareAssetId) {
            ok = _downloadAndFlash(_firmwareAssetId, U_FLASH, "firmware");
        }
        if (ok && _fsAssetId) {
            _status.progress = 0;
            ok = _downloadAndFlash(_fsAssetId, U_SPIFFS, "filesystem");
        }

        if (ok) {
            Logger::i("[upd] update complete — rebooting");
            _status.state = State::Done;
            delay(500);
            ESP.restart();
        } else {
            _status.state = State::Error;
        }
        vTaskDelete(nullptr);
    }

    static uint32_t _firmwareAssetId;
    static uint32_t _fsAssetId;
    static char     _errorBuf[48];
};

inline Updater::Status Updater::_status;
inline std::atomic<bool> Updater::_triggerPending{false};
inline uint32_t        Updater::_firmwareAssetId = 0;
inline uint32_t        Updater::_fsAssetId       = 0;
inline char            Updater::_errorBuf[48]    = {};
