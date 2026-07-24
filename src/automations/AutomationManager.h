#pragma once
#include <Arduino.h>

#include "../actions/ActionExecutor.h"
#include "../config/Config.h"

// Dispatches inbound mesh events (currently just GenericEvent, see
// MeshManager::setOnGenericEvent) against the configured AutomationBinding
// table and fires the matching rule's actions through ActionExecutor — the
// decentralized automation engine's seed (issue #439, part of #437).
// Purely reactive (no tick/poll, unlike ButtonManager): each call evaluates
// Config's current state directly, so an edit via the web UI takes effect
// on the next event with no reconfigure step needed.
class AutomationManager {
   public:
    void setExecutor(ActionExecutor* executor) { _executor = executor; }

    // Called from MeshManager::setOnGenericEvent. mac (the sender) is
    // currently unused — bindings match on eventType only, not sender
    // identity — but is accepted for parity with the mesh callback shape and
    // future trigger types that may need it.
    void onGenericEvent(const uint8_t* mac, const char* eventType, uint16_t payload) {
        (void)mac;
        if (!_executor) return;
        Config::forEachAutomation([&](uint8_t, AutomationBinding& b) {
            if (b.triggerType != TriggerType::GenericEvent) return;
            if (strcmp(b.eventType, eventType) != 0) return;
            _fireMatchingRule(b, payload);
        });
    }

   private:
    ActionExecutor* _executor = nullptr;

    // Evaluates a binding's rules in order and fires the first match's
    // actions (fan-out) — rules are mutually exclusive branches, not "all
    // matching rules fire".
    void _fireMatchingRule(AutomationBinding& b, uint16_t payload) {
        for (uint8_t r = 0; r < MAX_RULES_PER_BINDING; r++) {
            AutomationRule& rule = b.rules[r];
            if (!rule.exists) continue;
            if (payload < rule.valueMin || payload > rule.valueMax) continue;
            for (uint8_t a = 0; a < MAX_ACTIONS_PER_RULE; a++) {
                if (rule.actions[a].action != ActionId::None) _executor->execute(rule.actions[a]);
            }
            return;
        }
    }
};
