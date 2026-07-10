#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <functional>
#include "../config/Config.h"
#include "../scenes/SceneManager.h"
#include "../logging/Logger.h"

// Executes a ButtonAction against the group it targets. Deliberately
// decoupled from *how* it was invoked — a button press today, but the same
// execute() is meant to be reusable from MQTT/API/mesh trigger sources later.
// Mutation + propagation is delegated to injected callbacks so this class
// doesn't need to know about mesh/mqtt/runners itself.
class ActionExecutor {
public:
    using ApplyFn              = std::function<void(uint8_t groupId, const LightConfig&)>;
    using BroadcastGroupSyncFn = std::function<void(const GroupConfig&)>;

    // Applies + propagates a LightConfig change for a group (e.g. main.cpp's
    // applyAndPropagateLightConfig — save, runners, mesh broadcast, mqtt publish).
    void setApplyFn(ApplyFn fn) { _apply = fn; }
    // Broadcasts a GroupConfig change over mesh (used only by GroupSyncToggle,
    // which mutates GroupConfig.syncEnabled rather than LightConfig).
    void setBroadcastGroupSyncFn(BroadcastGroupSyncFn fn) { _broadcastGroupSync = fn; }

    void execute(const ButtonAction& action) {
        if (action.action == ActionId::None) return;
        GroupConfig* g = Config::group(action.groupId);
        if (!g) { Logger::w("[action] group %u not found — ignored", action.groupId); return; }

        if (action.action == ActionId::GroupSyncToggle) {
            g->syncEnabled = !g->syncEnabled;
            Config::bumpGroupRevision(*g);
            Config::save();
            if (_broadcastGroupSync) _broadcastGroupSync(*g);
            return;
        }

        LightConfig cfg = g->light;
        const ActionParams& p = action.params;

        switch (action.action) {
            case ActionId::BrightnessStep:
                cfg.brightness = (uint8_t)constrain((int)cfg.brightness + (int)p.numberValue, 0, 255);
                break;
            case ActionId::BrightnessSet:
                cfg.brightness = (uint8_t)constrain((int)p.numberValue, 0, 255);
                break;
            case ActionId::ColorSet:
                cfg.color = p.colorValue;
                break;
            case ActionId::SpeedStep:
                cfg.speed = max(0.0f, cfg.speed + p.numberValue);
                break;
            case ActionId::ModeSet:
                cfg.mode = (GroupMode)constrain((int)p.numberValue, 0, 5);
                break;
            case ActionId::PatternNext:
                cfg.pattern = (PatternId)(((uint8_t)cfg.pattern + 1) % 5);
                break;
            case ActionId::PatternPrev:
                cfg.pattern = (PatternId)(((uint8_t)cfg.pattern + 4) % 5);
                break;
            case ActionId::PatternSet:
                cfg.pattern = (PatternId)constrain((int)p.numberValue, 0, 4);
                break;
            case ActionId::SceneNext:
            case ActionId::ScenePrev:
            case ActionId::SceneRandom:
            case ActionId::GradientPaletteNext:
            case ActionId::GradientPalettePrev:
            case ActionId::GradientPaletteRandom:
                _cycleSceneId(cfg, action.action);
                break;
            case ActionId::SceneSet:
            case ActionId::GradientPaletteSet:
                strlcpy(cfg.sceneId, p.stringValue, sizeof(cfg.sceneId));
                break;
            case ActionId::FrameDurationStep:
                cfg.frameDuration = max(0.0f, cfg.frameDuration + p.numberValue);
                break;
            case ActionId::TransitionToggle:
                cfg.transitionEnabled = !cfg.transitionEnabled;
                break;
            case ActionId::TransitionTimeSet:
                cfg.transitionTime = max(0.0f, p.numberValue);
                break;
            case ActionId::SceneUniformColorToggle:
                cfg.sceneUniformColor = !cfg.sceneUniformColor;
                break;
            case ActionId::ProximityScaleStep:
                cfg.proximityScale = max(0.0f, cfg.proximityScale + p.numberValue);
                break;
            case ActionId::ProximityScaleSet:
                cfg.proximityScale = max(0.0f, p.numberValue);
                break;
            case ActionId::MorphToggle:
                cfg.morphEnabled = !cfg.morphEnabled;
                break;
            case ActionId::GradientStopCountSet:
                cfg.gradientStopCount = (uint8_t)constrain((int)p.numberValue, 0, 255);
                break;
            case ActionId::TextSet:
                strlcpy(cfg.text, p.stringValue, sizeof(cfg.text));
                break;
            case ActionId::TextAnimationSet:
                cfg.textAnimation = (TextAnimation)constrain((int)p.numberValue, 0, 1);
                break;
            case ActionId::Time24hToggle:
                cfg.time24h = !cfg.time24h;
                break;
            default:
                break;
        }

        cfg.seq = g->light.seq + 1;
        if (_apply) _apply(action.groupId, cfg);
    }

private:
    ApplyFn              _apply;
    BroadcastGroupSyncFn _broadcastGroupSync;

    // Next/Prev/Random cycle through scene files as returned by SceneManager's
    // directory listing (shared by Scene and Gradient modes — both reference
    // scenes by the same sceneId/file namespace). Order follows LittleFS
    // directory iteration, not a sorted order; consistent across calls as
    // long as scenes aren't added/removed mid-cycle.
    void _cycleSceneId(LightConfig& cfg, ActionId which) {
        JsonDocument doc;
        SceneManager::buildList(doc);
        JsonArray arr = doc["scenes"].as<JsonArray>();
        size_t n = arr.size();
        if (n == 0) return;

        bool random = (which == ActionId::SceneRandom || which == ActionId::GradientPaletteRandom);
        if (random) {
            uint32_t r = esp_random() % n;
            strlcpy(cfg.sceneId, arr[r]["id"] | "", sizeof(cfg.sceneId));
            return;
        }

        int currentIdx = -1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(arr[i]["id"] | "", cfg.sceneId) == 0) { currentIdx = (int)i; break; }
        }
        bool next = (which == ActionId::SceneNext || which == ActionId::GradientPaletteNext);
        int newIdx = currentIdx < 0 ? 0
                   : next ? (int)((currentIdx + 1) % n)
                          : (int)((currentIdx + (int)n - 1) % n);
        strlcpy(cfg.sceneId, arr[newIdx]["id"] | "", sizeof(cfg.sceneId));
    }
};
