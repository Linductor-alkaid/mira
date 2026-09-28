// M27 (DEC-041 first stage) World State wire layer: canonical JSON building
// for mira.worldstate.v1 (§4.4), the DEC-002 reader (version gate, fail-closed
// key sets, value-layer folding to StateInvalid), the input-record canonical
// digests (§4.3 M27 frozen source_event_digest semantics) and the to_json /
// digest member definitions. No clock read, no thread, no Executor
// registration (§4.6). Core value semantics and operators live in
// world_state.cpp; the private helper declarations live in
// world_state_wire.hpp.

#include "world_state_wire.hpp"

#include <mira/world_state.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace mira {

namespace {

// Closed wire name sets (§4.4): belief status and change kinds use the
// lowercased enumerator names; ElementSource keeps the exact enumerator names
// of the frozen observation contract.
constexpr std::string_view kBeliefNames[] = {"believed", "stale", "unknown"};
constexpr std::string_view kChangeKindNames[] = {
    "pageassumed", "entityobserved",      "entitystaled",   "navigationobserved",
    "toolsettled", "verificationsettled", "conflictmarked", "entityevicted"};
constexpr std::string_view kElementSourceNames[] = {"UiTree", "Ocr", "Detector", "Fused",
                                                    "Unknown"};

[[nodiscard]] std::int64_t wall_ns(const Timestamp &time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.wall.time_since_epoch())
        .count();
}

[[nodiscard]] Timestamp timestamp_from_wall_ns(std::int64_t nanoseconds) {
    Timestamp time;
    time.wall = WallTimePoint(std::chrono::nanoseconds(nanoseconds));
    return time;
}

[[nodiscard]] JsonValue json_string(std::string_view text) { return JsonValue(std::string(text)); }

[[nodiscard]] JsonValue json_int(std::uint64_t value) {
    return JsonValue(static_cast<std::int64_t>(value));
}

[[nodiscard]] Error malformed_error(std::string detail) {
    return make_world_state_error(WorldStateDomainCode::PayloadMalformed, std::move(detail));
}

[[nodiscard]] Error invalid_value_error(std::string detail) {
    return make_world_state_error(WorldStateDomainCode::StateInvalid, std::move(detail));
}

// Every object member must be inside the frozen key set and every frozen key
// must be present (unknown member / missing member fail closed, §4.4).
[[nodiscard]] std::optional<Error> check_member_set(const JsonValue &value, const char *what,
                                                    std::initializer_list<std::string_view> keys) {
    const auto *object = value.as_object();
    if (object == nullptr) {
        return malformed_error(std::string(what) + " must be a JSON object");
    }
    for (const auto &member : *object) {
        bool known = false;
        for (const auto key : keys) {
            if (member.first == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            return malformed_error(std::string(what) + " has unknown member '" + member.first +
                                   "'");
        }
    }
    for (const auto key : keys) {
        if (value.find(key) == nullptr) {
            return malformed_error(std::string(what) + " is missing member '" + std::string(key) +
                                   "'");
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_string(const JsonValue &object, const char *key,
                                               std::string &out) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return malformed_error(std::string("member '") + key + "' must be a JSON string");
    }
    out = *value->as_string();
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_bool(const JsonValue &object, const char *key, bool &out) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_boolean()) {
        return malformed_error(std::string("member '") + key + "' must be a JSON boolean");
    }
    out = value->as_boolean().value();
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_confidence(const JsonValue &object, const char *key,
                                                   double &out) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_number()) {
        return malformed_error(std::string("member '") + key + "' must be a JSON number");
    }
    out = value->as_number().value();
    return std::nullopt;
}

// Unsigned integer fields: a non-integer JSON type is malformed, a negative
// integer is an illegal value (StateInvalid, §4.1 value-layer rule).
[[nodiscard]] std::optional<Error> read_unsigned(const JsonValue &object, const char *key,
                                                 std::uint64_t &out) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_integer()) {
        return malformed_error(std::string("member '") + key + "' must be a JSON integer");
    }
    const auto raw = value->as_integer().value();
    if (raw < 0) {
        return invalid_value_error(std::string("member '") + key + "' must be non-negative");
    }
    out = static_cast<std::uint64_t>(raw);
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_timestamp(const JsonValue &object, const char *key,
                                                  Timestamp &out) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_integer()) {
        return malformed_error(std::string("member '") + key +
                               "' must be a JSON integer (wall ns)");
    }
    out = timestamp_from_wall_ns(value->as_integer().value());
    return std::nullopt;
}

template <typename Id>
[[nodiscard]] std::optional<Error> read_id(const JsonValue &object, const char *key, Id &out) {
    std::string text;
    if (auto error = read_string(object, key, text)) {
        return error;
    }
    const auto parsed = Id::parse(text);
    if (!parsed.has_value()) {
        return invalid_value_error(std::string("member '") + key + "' is not a valid id");
    }
    out = *parsed;
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_digest(const JsonValue &object, const char *key,
                                               Sha256Digest &out) {
    std::string text;
    if (auto error = read_string(object, key, text)) {
        return error;
    }
    const auto parsed = digest_from_hex(text);
    if (!parsed.has_value()) {
        return invalid_value_error(std::string("member '") + key +
                                   "' is not a valid sha256 digest");
    }
    out = *parsed;
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> read_belief_status(const JsonValue &object, const char *key,
                                                      WorldBeliefStatus &out) {
    std::string text;
    if (auto error = read_string(object, key, text)) {
        return error;
    }
    for (std::size_t index = 0; index < std::size(kBeliefNames); ++index) {
        if (text == kBeliefNames[index]) {
            out = static_cast<WorldBeliefStatus>(index);
            return std::nullopt;
        }
    }
    return invalid_value_error(std::string("member '") + key +
                               "' is outside the belief closed set");
}

[[nodiscard]] std::optional<Error> read_change_kind(const JsonValue &object, const char *key,
                                                    WorldChangeKind &out) {
    std::string text;
    if (auto error = read_string(object, key, text)) {
        return error;
    }
    for (std::size_t index = 0; index < std::size(kChangeKindNames); ++index) {
        if (text == kChangeKindNames[index]) {
            out = static_cast<WorldChangeKind>(index);
            return std::nullopt;
        }
    }
    return invalid_value_error(std::string("member '") + key +
                               "' is outside the change-kind closed set");
}

[[nodiscard]] std::optional<Error> read_element(const JsonValue &value, ElementRef &out) {
    if (auto error = check_member_set(value, "element",
                                      {"bounds", "environment_epoch", "evidence_digest",
                                       "observation_id", "source", "space", "stable_hint"})) {
        return error;
    }
    const auto *bounds = value.find("bounds");
    if (auto error =
            check_member_set(*bounds, "element bounds", {"bottom", "left", "right", "top"})) {
        return error;
    }
    for (const auto *key : {"left", "top", "right", "bottom"}) {
        const auto *number = bounds->find(key);
        if (number == nullptr || !number->is_number()) {
            return malformed_error(std::string("element bounds member '") + key +
                                   "' must be a JSON number");
        }
        const auto coordinate = number->as_number().value();
        if (key == std::string_view{"left"}) {
            out.bounds.left = coordinate;
        } else if (key == std::string_view{"top"}) {
            out.bounds.top = coordinate;
        } else if (key == std::string_view{"right"}) {
            out.bounds.right = coordinate;
        } else {
            out.bounds.bottom = coordinate;
        }
    }
    if (auto error = read_unsigned(value, "environment_epoch", out.environment_epoch)) {
        return error;
    }
    if (auto error = read_digest(value, "evidence_digest", out.evidence_digest)) {
        return error;
    }
    if (auto error = read_id(value, "observation_id", out.observation_id)) {
        return error;
    }
    {
        std::string text;
        if (auto error = read_string(value, "source", text)) {
            return error;
        }
        bool known = false;
        for (std::size_t index = 0; index < std::size(kElementSourceNames); ++index) {
            if (text == kElementSourceNames[index]) {
                out.source = static_cast<ElementSource>(index);
                known = true;
                break;
            }
        }
        if (!known) {
            return invalid_value_error("element source is outside the closed set");
        }
    }
    if (auto error = read_id(value, "space", out.space)) {
        return error;
    }
    return read_string(value, "stable_hint", out.stable_hint.hint);
}

[[nodiscard]] JsonValue entity_to_json(const WorldEntity &entity) {
    JsonValue::Object object;
    object.emplace_back("confidence", JsonValue(entity.confidence));
    object.emplace_back("element", ws_detail::element_to_json(entity.element));
    object.emplace_back("last_seen_at", JsonValue(wall_ns(entity.last_seen_at)));
    object.emplace_back("source_event_digest", json_string(entity.source_event_digest.to_string()));
    object.emplace_back("stale", JsonValue(entity.stale));
    return JsonValue(std::move(object));
}

[[nodiscard]] std::optional<Error> read_entity(const JsonValue &value, WorldEntity &out) {
    if (auto error = check_member_set(
            value, "entity",
            {"confidence", "element", "last_seen_at", "source_event_digest", "stale"})) {
        return error;
    }
    if (auto error = read_confidence(value, "confidence", out.confidence)) {
        return error;
    }
    if (auto error = read_timestamp(value, "last_seen_at", out.last_seen_at)) {
        return error;
    }
    if (auto error = read_digest(value, "source_event_digest", out.source_event_digest)) {
        return error;
    }
    if (auto error = read_bool(value, "stale", out.stale)) {
        return error;
    }
    return read_element(*value.find("element"), out.element);
}

} // namespace

namespace ws_detail {

JsonValue element_to_json(const ElementRef &element) {
    JsonValue::Object bounds;
    bounds.emplace_back("bottom", JsonValue(element.bounds.bottom));
    bounds.emplace_back("left", JsonValue(element.bounds.left));
    bounds.emplace_back("right", JsonValue(element.bounds.right));
    bounds.emplace_back("top", JsonValue(element.bounds.top));

    JsonValue::Object object;
    object.emplace_back("bounds", JsonValue(std::move(bounds)));
    object.emplace_back("environment_epoch", json_int(element.environment_epoch));
    object.emplace_back("evidence_digest", json_string(element.evidence_digest.to_string()));
    object.emplace_back("observation_id", json_string(element.observation_id.to_string()));
    object.emplace_back("source",
                        json_string(kElementSourceNames[static_cast<std::size_t>(element.source)]));
    object.emplace_back("space", json_string(element.space.to_string()));
    object.emplace_back("stable_hint", JsonValue(element.stable_hint.hint));
    return JsonValue(std::move(object));
}

JsonValue world_state_to_json(const WorldState &state) {
    JsonValue::Object version;
    version.emplace_back("major", json_int(state.schema_version.major));
    version.emplace_back("minor", json_int(state.schema_version.minor));

    JsonValue::Array entities;
    entities.reserve(state.entities.size());
    for (const auto &entity : state.entities) {
        entities.emplace_back(entity_to_json(entity));
    }

    JsonValue::Array changes;
    changes.reserve(state.recent_changes.size());
    for (const auto &change : state.recent_changes) {
        JsonValue::Object object;
        object.emplace_back("changed_at", JsonValue(wall_ns(change.changed_at)));
        object.emplace_back("change_sequence", json_int(change.change_sequence));
        object.emplace_back("kind",
                            json_string(kChangeKindNames[static_cast<std::size_t>(change.kind)]));
        object.emplace_back("source_event_digest",
                            json_string(change.source_event_digest.to_string()));
        object.emplace_back("summary", JsonValue(change.summary));
        changes.emplace_back(JsonValue(std::move(object)));
    }

    JsonValue::Object foreground;
    foreground.emplace_back("activity_name", JsonValue(state.foreground_app.activity_name));
    foreground.emplace_back("observed_at", JsonValue(wall_ns(state.foreground_app.observed_at)));
    foreground.emplace_back("package_name", JsonValue(state.foreground_app.package_name));
    foreground.emplace_back("sensitive", JsonValue(state.foreground_app.sensitive));
    foreground.emplace_back(
        "status", json_string(kBeliefNames[static_cast<std::size_t>(state.foreground_app.status)]));

    JsonValue::Object page;
    page.emplace_back("app_id", JsonValue(state.current_page.app_id));
    page.emplace_back("confidence", JsonValue(state.current_page.confidence));
    page.emplace_back("recognized_at", JsonValue(wall_ns(state.current_page.recognized_at)));
    page.emplace_back("state_id", JsonValue(state.current_page.state_id));
    page.emplace_back(
        "status", json_string(kBeliefNames[static_cast<std::size_t>(state.current_page.status)]));

    JsonValue::Object root;
    root.emplace_back("current_page", JsonValue(std::move(page)));
    root.emplace_back("entities", JsonValue(std::move(entities)));
    root.emplace_back("foreground_app", JsonValue(std::move(foreground)));
    root.emplace_back("recent_changes", JsonValue(std::move(changes)));
    root.emplace_back("schema_version", JsonValue(std::move(version)));
    return JsonValue(std::move(root));
}

Sha256Digest input_digest(const WorldObservationInput &input) {
    JsonValue::Object app;
    app.emplace_back("activity_name", JsonValue(input.app.activity_name));
    app.emplace_back("package_name", JsonValue(input.app.package_name));
    app.emplace_back("sensitive", JsonValue(input.app.sensitive));

    JsonValue::Array elements;
    elements.reserve(input.elements.size());
    for (const auto &element : input.elements) {
        elements.emplace_back(element_to_json(element));
    }

    JsonValue::Object object;
    object.emplace_back("app", JsonValue(std::move(app)));
    object.emplace_back("elements", JsonValue(std::move(elements)));
    object.emplace_back("environment_epoch", json_int(input.environment_epoch));
    object.emplace_back("observation_id", json_string(input.observation_id.to_string()));
    object.emplace_back("observed_at", JsonValue(wall_ns(input.observed_at)));
    return canonical_json_digest(JsonValue(std::move(object)));
}

Sha256Digest input_digest(const WorldScreenStateInput &input) {
    JsonValue::Object object;
    object.emplace_back("app_id", JsonValue(input.app_id));
    object.emplace_back("confidence", JsonValue(input.confidence));
    object.emplace_back("recognized_at", JsonValue(wall_ns(input.recognized_at)));
    object.emplace_back("state_id", JsonValue(input.state_id));
    return canonical_json_digest(JsonValue(std::move(object)));
}

Sha256Digest input_digest(const WorldNavigationInput &input) {
    JsonValue::Object object;
    object.emplace_back("app_id", JsonValue(input.app_id));
    object.emplace_back("confidence", JsonValue(input.confidence));
    object.emplace_back("from_state", JsonValue(input.from_state));
    object.emplace_back("observed_at", JsonValue(wall_ns(input.observed_at)));
    object.emplace_back("success", JsonValue(input.success));
    object.emplace_back("to_state", JsonValue(input.to_state));
    object.emplace_back("transition_id", JsonValue(input.transition_id));
    return canonical_json_digest(JsonValue(std::move(object)));
}

Sha256Digest input_digest(const WorldToolSettledInput &input) {
    JsonValue::Object object;
    object.emplace_back("disposition", JsonValue(input.disposition));
    object.emplace_back("settled_at", JsonValue(wall_ns(input.settled_at)));
    object.emplace_back("source_event_digest", json_string(input.source_event_digest.to_string()));
    object.emplace_back("tool_ref", JsonValue(input.tool_ref));
    return canonical_json_digest(JsonValue(std::move(object)));
}

Sha256Digest input_digest(const WorldVerificationInput &input) {
    JsonValue::Object object;
    object.emplace_back("confidence", JsonValue(input.confidence));
    object.emplace_back("outcome", JsonValue(input.outcome));
    object.emplace_back("verified_at", JsonValue(wall_ns(input.verified_at)));
    return canonical_json_digest(JsonValue(std::move(object)));
}

Sha256Digest input_digest(const WorldExpiryInput &input) {
    JsonValue::Object object;
    object.emplace_back("now", JsonValue(wall_ns(input.now)));
    return canonical_json_digest(JsonValue(std::move(object)));
}

} // namespace ws_detail

// ---------------------------------------------------------------------------
// Wire member definitions (§4.4; DEC-002 reader discipline).
// ---------------------------------------------------------------------------

std::string WorldState::to_json() const {
    return canonical_json_string(ws_detail::world_state_to_json(*this));
}

Sha256Digest WorldState::digest() const {
    return canonical_json_digest(ws_detail::world_state_to_json(*this));
}

Result<WorldState> WorldState::from_json(std::string_view text) {
    const auto parsed = parse_json(text);
    if (!parsed.has_value()) {
        return malformed_error("world state payload is not valid JSON");
    }
    const JsonValue &document = parsed.value();
    if (auto error = check_member_set(
            document, "world state",
            {"current_page", "entities", "foreground_app", "recent_changes", "schema_version"})) {
        return std::move(error).value();
    }

    const auto *version = document.find("schema_version");
    if (auto error = check_member_set(*version, "schema_version", {"major", "minor"})) {
        return std::move(error).value();
    }
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    if (auto error = read_unsigned(*version, "major", major)) {
        return std::move(error).value();
    }
    if (major != 1U) {
        return make_world_state_error(WorldStateDomainCode::SchemaUnsupported,
                                      "schema major is outside the {1,x} support set");
    }
    if (auto error = read_unsigned(*version, "minor", minor)) {
        return std::move(error).value();
    }
    if (minor > 0xFFFFU) {
        return invalid_value_error("schema minor does not fit SchemaVersion");
    }

    WorldState state;
    state.schema_version =
        SchemaVersion{static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor)};

    const auto *foreground = document.find("foreground_app");
    if (auto error = check_member_set(
            *foreground, "foreground_app",
            {"activity_name", "observed_at", "package_name", "sensitive", "status"})) {
        return std::move(error).value();
    }
    if (auto error =
            read_string(*foreground, "activity_name", state.foreground_app.activity_name)) {
        return std::move(error).value();
    }
    if (auto error = read_string(*foreground, "package_name", state.foreground_app.package_name)) {
        return std::move(error).value();
    }
    if (auto error = read_bool(*foreground, "sensitive", state.foreground_app.sensitive)) {
        return std::move(error).value();
    }
    if (auto error = read_timestamp(*foreground, "observed_at", state.foreground_app.observed_at)) {
        return std::move(error).value();
    }
    if (auto error = read_belief_status(*foreground, "status", state.foreground_app.status)) {
        return std::move(error).value();
    }

    const auto *page = document.find("current_page");
    if (auto error =
            check_member_set(*page, "current_page",
                             {"app_id", "confidence", "recognized_at", "state_id", "status"})) {
        return std::move(error).value();
    }
    if (auto error = read_string(*page, "app_id", state.current_page.app_id)) {
        return std::move(error).value();
    }
    if (auto error = read_string(*page, "state_id", state.current_page.state_id)) {
        return std::move(error).value();
    }
    if (auto error = read_confidence(*page, "confidence", state.current_page.confidence)) {
        return std::move(error).value();
    }
    if (auto error = read_timestamp(*page, "recognized_at", state.current_page.recognized_at)) {
        return std::move(error).value();
    }
    if (auto error = read_belief_status(*page, "status", state.current_page.status)) {
        return std::move(error).value();
    }

    {
        const auto *entities = document.find("entities");
        const auto *array = entities->as_array();
        if (array == nullptr) {
            return malformed_error("entities must be a JSON array");
        }
        state.entities.reserve(array->size());
        for (const auto &entry : *array) {
            WorldEntity entity;
            if (auto error = read_entity(entry, entity)) {
                return std::move(error).value();
            }
            state.entities.push_back(std::move(entity));
        }
    }

    {
        const auto *changes = document.find("recent_changes");
        const auto *array = changes->as_array();
        if (array == nullptr) {
            return malformed_error("recent_changes must be a JSON array");
        }
        state.recent_changes.reserve(array->size());
        for (const auto &entry : *array) {
            if (auto error = check_member_set(
                    entry, "recent_changes entry",
                    {"changed_at", "change_sequence", "kind", "source_event_digest", "summary"})) {
                return std::move(error).value();
            }
            WorldChange change;
            if (auto error = read_change_kind(entry, "kind", change.kind)) {
                return std::move(error).value();
            }
            if (auto error = read_string(entry, "summary", change.summary)) {
                return std::move(error).value();
            }
            if (auto error =
                    read_digest(entry, "source_event_digest", change.source_event_digest)) {
                return std::move(error).value();
            }
            if (auto error = read_timestamp(entry, "changed_at", change.changed_at)) {
                return std::move(error).value();
            }
            if (auto error = read_unsigned(entry, "change_sequence", change.change_sequence)) {
                return std::move(error).value();
            }
            state.recent_changes.push_back(std::move(change));
        }
    }

    if (const Result<void> valid = state.validate(); !valid.has_value()) {
        return valid.error();
    }
    return state;
}

} // namespace mira
