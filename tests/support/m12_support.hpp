#pragma once

// Shared fixtures for the M12 stage-E tests (DEC-027/028): an App Model
// builder, a host-side "screen device" supplying the recognition boundary
// (DEC-011: recognition is host work; Mira Core only consumes snapshots) and
// edge-action tools. Consumers: tests/m12/*.cpp.

#include "m9_support.hpp"

#include <mira/workflow_navigation.hpp>

#include <atomic>
#include <mutex>

namespace mira::testing {

// --- App Model builder -------------------------------------------------------

[[nodiscard]] inline AppModelState ui_state(const std::string &id, const std::string &page,
                                            std::optional<std::string> modal = std::nullopt) {
    AppModelState state;
    state.id = id;
    state.page = page;
    state.modal = std::move(modal);
    state.confidence = ConfidenceRecord{};
    return state;
}

[[nodiscard]] inline JsonValue edge_action(const std::string &tool) {
    JsonValue::Object action;
    action.emplace_back("tool", tool);
    return JsonValue{std::move(action)};
}

[[nodiscard]] inline AppModelTransition ui_transition(std::string id, std::string from,
                                                      std::string to, std::string tool,
                                                      double latency_ms = 100.0,
                                                      double failure_probability = 0.0) {
    AppModelTransition transition;
    transition.id = std::move(id);
    transition.from_state = std::move(from);
    transition.to_state = std::move(to);
    transition.action = edge_action(tool);
    transition.costs.latency_ms = latency_ms;
    transition.costs.failure_probability = failure_probability;
    transition.confidence = ConfidenceRecord{};
    return transition;
}

// Four states, three edges: home -> chat_list -> chat_view -> composer, plus
// a fast-but-risky shortcut home -> chat_view.
[[nodiscard]] inline AppModel device_model() {
    AppModel model;
    model.schema_version = SchemaVersion{1, 0};
    model.app_id = "com.example.chat";
    model.name = "chat-app";
    model.summary = "m12 fixture app model";
    model.states = {ui_state("home", "Launcher"), ui_state("chat_list", "ChatList"),
                    ui_state("chat_view", "Chat", "idle"),
                    ui_state("composer", "Chat", "composer_open")};
    model.transitions = {
        ui_transition("t-open-chats", "home", "chat_list", "open_chats", 100.0),
        ui_transition("t-open-chat", "chat_list", "chat_view", "open_chat", 200.0),
        ui_transition("t-open-composer", "chat_view", "composer", "tap_input", 50.0),
        ui_transition("t-deep-link", "home", "chat_view", "deep_link", 20.0, 0.9),
    };
    return model;
}

// --- Host-side device with recognition boundary -------------------------------

// Simulates the host side of the DEC-011 boundary: it owns the recognized
// screen state (whatever produced it) and registers edge-action tools whose
// dispatch moves the state. `stuck` tools dispatch successfully without
// moving the state (arrival unverified); `failing` tools return errors.
class ScreenDevice final {
  public:
    explicit ScreenDevice(std::string initial) : current_{std::move(initial)} {}

    [[nodiscard]] ScreenStateProvider provider() {
        return [this]() -> std::optional<ScreenStateSnapshot> {
            std::lock_guard lock(mutex_);
            if (current_.empty()) {
                return std::nullopt;
            }
            return ScreenStateSnapshot{current_, observed_at_ms_};
        };
    }

    [[nodiscard]] BuiltinToolRegistration action(std::string wire, std::string to,
                                                 bool side_effects = true) {
        return registration_of(std::move(wire), std::move(to), side_effects, false);
    }

    // Dispatches successfully but never moves the state.
    [[nodiscard]] BuiltinToolRegistration stuck_action(std::string wire) {
        return registration_of(std::move(wire), {}, true, true);
    }

    [[nodiscard]] int dispatches() const { return dispatches_.load(); }
    [[nodiscard]] std::string current() {
        std::lock_guard lock(mutex_);
        return current_;
    }
    void set_current(std::string next) {
        std::lock_guard lock(mutex_);
        current_ = std::move(next);
    }

  private:
    [[nodiscard]] BuiltinToolRegistration registration_of(std::string wire, std::string to,
                                                          bool side_effects, bool stuck) {
        BuiltinToolRegistration registration;
        registration.spec.wire_name = std::move(wire);
        registration.spec.description = "m12 screen device action";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {},
            "additionalProperties": false
        })json")
                                                             .value()};
        registration.spec.has_side_effects = side_effects;
        registration.handler = [this, to = std::move(to),
                                stuck](const JsonValue &,
                                       const OperationContext &) -> Result<JsonValue> {
            ++dispatches_;
            if (!stuck) {
                std::lock_guard lock(mutex_);
                current_ = to;
            }
            return JsonValue{"moved"};
        };
        return registration;
    }

    std::mutex mutex_;
    std::string current_;
    std::uint64_t observed_at_ms_ = 1'000;
    std::atomic<int> dispatches_{0};
};

} // namespace mira::testing
