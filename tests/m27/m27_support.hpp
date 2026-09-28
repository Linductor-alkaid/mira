#pragma once

// Shared fixtures for the M27 world-state IVA test matrix (DEC-041 first
// stage; milestone plan docs/plans/m27-world-state-projection-core.md §4
// frozen contract semantics, §6 gates WS-G1..G6). Consumers:
// tests/m27/m27_entity_vocabulary_test.cpp, tests/m27/m27_world_state_test.cpp
// and tests/m27/m27_wire_rebuild.cpp.
//
// Everything here pins the §4 frozen surface: the mira.world_state error
// domain (8 explicit int32 codes + stable names + the deterministic ErrorCode
// assignment observed at freeze), the frozen dataset (all six input record
// kinds over the conflict/expiry/eviction paths), and deterministic builders.
// All timestamps come from the dataset clock, never from the system clock
// (§4.6) - the only Timestamp::now() use is inside the purity case, which
// asserts the host clock is NOT observable through the projection digest.

#include "../support/test.hpp"

#include <mira/world_state.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mira::m27 {

// ---------------------------------------------------------------------------
// Frozen error domain table (milestone §4.5; DEC-002 stable public values:
// explicit int32 values 1..8 and stable member names). The ErrorCode column
// pins the deterministic assignment of make_world_state_error (make_context_error
// precedent): same code -> same ErrorCode, forever.
// ---------------------------------------------------------------------------

struct DomainCodeEntry final {
    WorldStateDomainCode code;
    std::string_view name;
    std::int32_t value;
    ErrorCode mapped;
};

inline const DomainCodeEntry kDomainCodes[] = {
    {WorldStateDomainCode::OptionsInvalid, "OptionsInvalid", 1, ErrorCode::InvalidArgument},
    {WorldStateDomainCode::StateInvalid, "StateInvalid", 2, ErrorCode::InvalidArgument},
    {WorldStateDomainCode::RecordInvalid, "RecordInvalid", 3, ErrorCode::InvalidArgument},
    {WorldStateDomainCode::VocabularyViolation, "VocabularyViolation", 4,
     ErrorCode::InvalidArgument},
    {WorldStateDomainCode::PayloadTooLarge, "PayloadTooLarge", 5, ErrorCode::ResourceExhausted},
    {WorldStateDomainCode::SchemaUnsupported, "SchemaUnsupported", 6,
     ErrorCode::UnsupportedVersion},
    {WorldStateDomainCode::PayloadMalformed, "PayloadMalformed", 7, ErrorCode::InvalidArgument},
    {WorldStateDomainCode::StaleTargetMissing, "StaleTargetMissing", 8, ErrorCode::InvalidState},
};

inline constexpr std::string_view kDomainName = "mira.world_state";

// ---------------------------------------------------------------------------
// Deterministic dataset clock (§4.6: no system-clock read in the projection).
// ---------------------------------------------------------------------------

inline constexpr std::int64_t kBaseNs = 1'724'000'000'000'000'000;
inline constexpr std::int64_t kSecondNs = 1'000'000'000;

[[nodiscard]] inline Timestamp at_ns(std::int64_t offset_seconds, std::int64_t offset_ns = 0) {
    Timestamp time;
    time.wall = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(kBaseNs + offset_seconds * kSecondNs + offset_ns));
    return time;
}

// ---------------------------------------------------------------------------
// Deterministic id / element builders. Id128 is the 32-lowercase-hex form; the
// dataset tag lands in the last 8 hex digits so eviction-order assertions can
// identify entries by their canonical observation_id string.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string tagged_hex(char prefix, std::uint64_t tag) {
    char text[40]; // 1 prefix + 31 hex + NUL, with headroom for the compiler
    std::snprintf(text, sizeof(text), "%c%031llx", prefix, static_cast<unsigned long long>(tag));
    return std::string(text, text + 32);
}

[[nodiscard]] inline ObservationId tagged_oid(std::uint64_t tag) {
    return ObservationId::parse(tagged_hex('1', tag)).value();
}

[[nodiscard]] inline CoordinateSpaceId tagged_space(std::uint64_t tag) {
    return CoordinateSpaceId::parse(tagged_hex('2', tag)).value();
}

[[nodiscard]] inline ElementRef make_element(std::uint64_t tag, std::string hint = {}) {
    ElementRef element;
    element.observation_id = tagged_oid(tag);
    element.environment_epoch = 1;
    element.source = ElementSource::UiTree;
    element.stable_hint.hint =
        hint.empty() ? std::string("node_") + std::to_string(tag) : std::move(hint);
    element.bounds = RectF{0.0, 0.0, 10.0, 20.0};
    element.space = tagged_space(7);
    element.evidence_digest = digest_string("mira://m27/evidence/" + std::to_string(tag));
    return element;
}

[[nodiscard]] inline AppContext make_app(std::string package, std::string activity,
                                         bool sensitive = false) {
    AppContext app;
    app.package_name = std::move(package);
    app.activity_name = std::move(activity);
    app.sensitive = sensitive;
    return app;
}

// Handcrafted entity for the capacity/eviction grids (WS-G4); the optional
// hint override enables the full-key-tie eviction cells.
[[nodiscard]] inline WorldEntity make_entity(std::uint64_t tag, bool stale, double confidence,
                                             std::int64_t seen_second, std::string hint = {}) {
    WorldEntity entity;
    entity.element = make_element(tag, std::move(hint));
    entity.confidence = confidence;
    entity.stale = stale;
    entity.last_seen_at = at_ns(seen_second);
    entity.source_event_digest = digest_string("mira://m27/entity/" + std::to_string(tag));
    return entity;
}

[[nodiscard]] inline std::string tag_of(const WorldEntity &entity) {
    return entity.element.observation_id.to_string();
}

// ---------------------------------------------------------------------------
// Input record builders (§4.2 closed set).
// ---------------------------------------------------------------------------

[[nodiscard]] inline WorldObservationInput make_observation(std::uint64_t oid_tag,
                                                            const AppContext &app,
                                                            std::vector<ElementRef> elements,
                                                            std::int64_t second) {
    WorldObservationInput input;
    input.observation_id = tagged_oid(oid_tag);
    input.environment_epoch = 1;
    input.app = app;
    input.elements = std::move(elements);
    input.observed_at = at_ns(second);
    return input;
}

[[nodiscard]] inline WorldScreenStateInput make_screen(std::string app_id, std::string state_id,
                                                       double confidence, std::int64_t second) {
    WorldScreenStateInput input;
    input.app_id = std::move(app_id);
    input.state_id = std::move(state_id);
    input.confidence = confidence;
    input.recognized_at = at_ns(second);
    return input;
}

[[nodiscard]] inline WorldNavigationInput make_navigation(std::string app_id, std::string from,
                                                          std::string to, std::string transition,
                                                          bool success, double confidence,
                                                          std::int64_t second) {
    WorldNavigationInput input;
    input.app_id = std::move(app_id);
    input.from_state = std::move(from);
    input.to_state = std::move(to);
    input.transition_id = std::move(transition);
    input.success = success;
    input.confidence = confidence;
    input.observed_at = at_ns(second);
    return input;
}

[[nodiscard]] inline WorldToolSettledInput make_tool(std::string tool_ref, std::string disposition,
                                                     std::int64_t second) {
    WorldToolSettledInput input;
    input.tool_ref = std::move(tool_ref);
    input.disposition = std::move(disposition);
    input.source_event_digest = digest_string("mira://m27/event/tool/" + tool_ref);
    input.settled_at = at_ns(second);
    return input;
}

[[nodiscard]] inline WorldVerificationInput
make_verification(std::string outcome, double confidence, std::int64_t second) {
    WorldVerificationInput input;
    input.outcome = std::move(outcome);
    input.confidence = confidence;
    input.verified_at = at_ns(second);
    return input;
}

[[nodiscard]] inline WorldExpiryInput make_expiry(std::int64_t second, std::int64_t extra_ns = 0) {
    WorldExpiryInput input;
    input.now = at_ns(second, extra_ns);
    return input;
}

// ---------------------------------------------------------------------------
// Frozen dataset (WS-G3): covers all six input kinds (five updates + the
// explicit WorldExpiryInput) over the conflict, expiry and insert-overflow
// eviction paths, under dataset_options() below. Timeline:
//   obs@1s   foreground Believed(com.example.launcher/.MainActivity), e1+e2
//   obs@2s   same foreground, e2 refreshed + e3 inserted -> e1 evicted
//   screen@3s page Believed(com.example.launcher/state_home, 0.9)
//   nav@4s   from_state mismatch -> ConflictMarked, then success ->
//            Believed(.../state_detail, 0.8, recognized_at = 4s)
//   tool@5s  ToolSettled entry only
//   verify@6s confirmed -> confidence 0.95, recognized_at = 6s
//   expiry@37s e2/e3 (last seen 2s, age 35s > 30s) stale + halved, page
//            (recognized 6s, age 31s > 30s) stale + halved
// ---------------------------------------------------------------------------

[[nodiscard]] inline WorldStateOptions dataset_options() {
    WorldStateOptions options;
    options.max_entities = 2;
    options.max_recent_changes = 64;
    options.max_change_summary_bytes = 2048;
    options.stale_after = std::chrono::milliseconds{30000};
    return options;
}

[[nodiscard]] inline std::vector<WorldStateInput> frozen_dataset() {
    std::vector<WorldStateInput> dataset;
    const AppContext launcher = make_app("com.example.launcher", "MainActivity");
    dataset.push_back(make_observation(0x11, launcher, {make_element(1), make_element(2)}, 1));
    dataset.push_back(make_observation(0x12, launcher, {make_element(2), make_element(3)}, 2));
    dataset.push_back(make_screen("com.example.launcher", "state_home", 0.9, 3));
    dataset.push_back(make_navigation("com.example.launcher", "state_other", "state_detail",
                                      "transition-1", true, 0.8, 4));
    dataset.push_back(make_tool("toolref:delta.render", "completed", 5));
    dataset.push_back(make_verification("confirmed", 0.95, 6));
    dataset.push_back(make_expiry(37));
    return dataset;
}

// Byte anchor for the frozen dataset rebuild (WS-G3). Pinned from the first
// verified run; any semantic drift in the operators changes this digest.
inline constexpr std::string_view kFrozenDatasetDigestHex =
    "4ce804a3a1d5ed73855ff6cc6d0cce038f822912afca57bd9d7b4fbb977e6bc3";

// ---------------------------------------------------------------------------
// Independent incremental fold (the test-side counterpart of
// rebuild_world_state; WS-G3 asserts both agree byte-for-byte).
// ---------------------------------------------------------------------------

[[nodiscard]] inline Result<WorldState> advance(WorldState state, const WorldStateInput &input,
                                                const WorldStateOptions &options) {
    if (const auto *observation = std::get_if<WorldObservationInput>(&input)) {
        return apply_world_observation(std::move(state), *observation, options);
    }
    if (const auto *screen = std::get_if<WorldScreenStateInput>(&input)) {
        return apply_screen_state(std::move(state), *screen, options);
    }
    if (const auto *navigation = std::get_if<WorldNavigationInput>(&input)) {
        return apply_navigation(std::move(state), *navigation, options);
    }
    if (const auto *tool = std::get_if<WorldToolSettledInput>(&input)) {
        return apply_tool_settled(std::move(state), *tool, options);
    }
    if (const auto *verification = std::get_if<WorldVerificationInput>(&input)) {
        return apply_verification(std::move(state), *verification, options);
    }
    return apply_expiry(std::move(state), std::get<WorldExpiryInput>(input), options);
}

[[nodiscard]] inline Result<WorldState> fold_all(const std::vector<WorldStateInput> &dataset,
                                                 const WorldStateOptions &options) {
    WorldState state;
    for (const auto &record : dataset) {
        auto applied = advance(std::move(state), record, options);
        if (!applied.has_value()) {
            return applied;
        }
        state = std::move(applied).value();
    }
    return state;
}

// ---------------------------------------------------------------------------
// Assertion helpers.
// ---------------------------------------------------------------------------

// Timestamp carries no comparison operators; projections only ever compare
// the wall instant (§4.3 staleness rule).
[[nodiscard]] inline bool same_instant(const Timestamp &left, const Timestamp &right) {
    return left.wall == right.wall;
}

[[nodiscard]] inline bool is_epoch(const Timestamp &time) {
    return time.wall == std::chrono::system_clock::time_point{};
}

inline int expect_domain_error(const Error &error, WorldStateDomainCode code, const char *what) {
    if (error.domain != kDomainName) {
        std::cerr << "m27: " << what << ": domain '" << error.domain << "' != '" << kDomainName
                  << "'\n";
        return 1;
    }
    if (error.domain_code != static_cast<std::int32_t>(code)) {
        std::cerr << "m27: " << what << ": domain_code " << error.domain_code
                  << " != " << static_cast<std::int32_t>(code) << '\n';
        return 1;
    }
    if (error.retryable) {
        std::cerr << "m27: " << what << ": error must not be retryable\n";
        return 1;
    }
    return 0;
}

template <typename T>
int expect_error(const Result<T> &result, WorldStateDomainCode code, const char *what) {
    if (result.has_value()) {
        std::cerr << "m27: " << what << ": expected error, got success\n";
        return 1;
    }
    return expect_domain_error(result.error(), code, what);
}

template <typename T> int expect_ok(const Result<T> &result, const char *what) {
    if (!result.has_value()) {
        std::cerr << "m27: " << what << ": unexpected error (" << result.error().domain << '#'
                  << result.error().domain_code << " " << result.error().safe_message << ")\n";
        return 1;
    }
    return 0;
}

// Header face scan (WS-G1 reverse assertions / WS-G5 read-only face): the
// public header must not carry the given tokens at all.
inline int assert_header_absent(const std::initializer_list<std::string_view> &tokens,
                                const char *what) {
    std::ifstream in(std::string(M27_SOURCE_DIR) + "/include/mira/world_state.hpp");
    if (!in) {
        std::cerr << "m27: " << what << ": cannot open world_state.hpp\n";
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const std::string_view token : tokens) {
        if (text.find(token) != std::string::npos) {
            std::cerr << "m27: " << what << ": forbidden token '" << token
                      << "' present in world_state.hpp\n";
            return 1;
        }
    }
    return 0;
}

} // namespace mira::m27
