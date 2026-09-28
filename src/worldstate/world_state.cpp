// M27 (DEC-041 first stage) session World State projection core: error
// domain, value-type validation and the frozen update operators. Contract
// source of truth: docs/plans/m27-world-state-projection-core.md §4 (frozen
// 2026-09-28, including the §9 third-revision B1/B2 corrections). Every
// operation here is a bounded synchronous pure function driven by the caller:
// no thread, timer, Executor registration or system-clock read exists in this
// translation unit (§4.6). A failed operator leaves the projected input value
// untouched (strong-consistency failure semantics, §4.3). Wire serialization
// and the wire reader live in world_state_wire.cpp.

#include <mira/world_state.hpp>

#include "world_state_wire.hpp"

#include <mira/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

namespace mira {
namespace {

constexpr std::string_view kDomain = "mira.world_state";

constexpr std::string_view kDispositions[] = {"completed", "skipped", "failed", "stale"};
constexpr std::string_view kOutcomes[] = {"confirmed", "refuted", "inconclusive"};

// Vocabulary §2.3 frozen registry-level bound for toolref references.
constexpr std::size_t kToolRefMaxBytes = 256;

template <std::size_t N>
[[nodiscard]] bool closed_set_contains(std::string_view text, const std::string_view (&set)[N]) {
    return std::find(std::begin(set), std::end(set), text) != std::end(set);
}

[[nodiscard]] bool confidence_in_range(double confidence) {
    return std::isfinite(confidence) && confidence >= 0.0 && confidence <= 1.0;
}

// ---------------------------------------------------------------------------
// Ring append with the operator invariants: summary bound (PayloadTooLarge,
// never truncated), strictly advancing change_sequence, FIFO eviction at
// max_recent_changes (§4.3).
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void> append_change(WorldState &state, WorldChangeKind kind,
                                         std::string summary,
                                         const Sha256Digest &source_event_digest,
                                         Timestamp changed_at, const WorldStateOptions &options) {
    if (summary.size() > options.max_change_summary_bytes) {
        return make_world_state_error(WorldStateDomainCode::PayloadTooLarge,
                                      "change summary exceeds max_change_summary_bytes");
    }
    WorldChange change;
    change.kind = kind;
    change.summary = std::move(summary);
    change.source_event_digest = source_event_digest;
    change.changed_at = changed_at;
    change.change_sequence =
        state.recent_changes.empty() ? 1U : state.recent_changes.back().change_sequence + 1U;
    state.recent_changes.push_back(std::move(change));
    if (state.recent_changes.size() > options.max_recent_changes) {
        state.recent_changes.erase(state.recent_changes.begin());
    }
    return Result<void>{};
}

// Prune is a total function (§4.3: returns a value), so its fixed-width
// eviction summaries are appended without the bound check; the ring FIFO and
// the strictly advancing sequence still apply.
void append_change_unchecked(WorldState &state, WorldChangeKind kind, std::string summary,
                             const Sha256Digest &source_event_digest, Timestamp changed_at,
                             const WorldStateOptions &options) {
    WorldChange change;
    change.kind = kind;
    change.summary = std::move(summary);
    change.source_event_digest = source_event_digest;
    change.changed_at = changed_at;
    change.change_sequence =
        state.recent_changes.empty() ? 1U : state.recent_changes.back().change_sequence + 1U;
    state.recent_changes.push_back(std::move(change));
    if (state.recent_changes.size() > options.max_recent_changes) {
        state.recent_changes.erase(state.recent_changes.begin());
    }
}

// ---------------------------------------------------------------------------
// Frozen eviction order (§4.3/§4.4): stale first -> earliest last_seen_at ->
// lowest confidence -> ElementRef canonical JSON lexicographic ascending
// (total deterministic order; entities are element-unique in well-formed
// projections, so the element key settles every remaining tie).
// ---------------------------------------------------------------------------

[[nodiscard]] bool same_element(const ElementRef &left, const ElementRef &right) {
    return left.observation_id == right.observation_id &&
           left.environment_epoch == right.environment_epoch && left.source == right.source &&
           left.stable_hint.hint == right.stable_hint.hint && left.bounds == right.bounds &&
           left.space == right.space && left.evidence_digest == right.evidence_digest;
}

[[nodiscard]] bool evicts_before(const WorldEntity &left, const WorldEntity &right) {
    if (left.stale != right.stale) {
        return left.stale;
    }
    if (left.last_seen_at.wall != right.last_seen_at.wall) {
        return left.last_seen_at.wall < right.last_seen_at.wall;
    }
    if (left.confidence != right.confidence) {
        return left.confidence < right.confidence;
    }
    return canonical_json_string(ws_detail::element_to_json(left.element)) <
           canonical_json_string(ws_detail::element_to_json(right.element));
}

using EntityIterator = std::vector<WorldEntity>::iterator;

[[nodiscard]] EntityIterator eviction_candidate(WorldState &state) {
    return std::min_element(state.entities.begin(), state.entities.end(), evicts_before);
}

[[nodiscard]] Result<void> evict_one(WorldState &state, const Sha256Digest &cause_digest,
                                     Timestamp changed_at, const WorldStateOptions &options) {
    const auto victim = eviction_candidate(state);
    std::string summary =
        "entity evicted observation_id=" + victim->element.observation_id.to_string();
    if (const Result<void> appended =
            append_change(state, WorldChangeKind::EntityEvicted, std::move(summary), cause_digest,
                          changed_at, options);
        !appended.has_value()) {
        return appended;
    }
    state.entities.erase(victim);
    return Result<void>{};
}

[[nodiscard]] bool same_foreground_content(const WorldForegroundApp &left,
                                           const WorldForegroundApp &right) {
    return left.status == right.status && left.package_name == right.package_name &&
           left.activity_name == right.activity_name && left.sensitive == right.sensitive;
}

} // namespace

// ---------------------------------------------------------------------------
// Error domain (§4.5 frozen triple mapping; DEC-002 stable public values).
// ---------------------------------------------------------------------------

std::string_view world_state_domain_code_name(WorldStateDomainCode code) {
    switch (code) {
    case WorldStateDomainCode::OptionsInvalid:
        return "OptionsInvalid";
    case WorldStateDomainCode::StateInvalid:
        return "StateInvalid";
    case WorldStateDomainCode::RecordInvalid:
        return "RecordInvalid";
    case WorldStateDomainCode::VocabularyViolation:
        return "VocabularyViolation";
    case WorldStateDomainCode::PayloadTooLarge:
        return "PayloadTooLarge";
    case WorldStateDomainCode::SchemaUnsupported:
        return "SchemaUnsupported";
    case WorldStateDomainCode::PayloadMalformed:
        return "PayloadMalformed";
    case WorldStateDomainCode::StaleTargetMissing:
        return "StaleTargetMissing";
    }
    return "Unknown";
}

Error make_world_state_error(WorldStateDomainCode code, std::string detail) {
    Error error;
    error.code = ErrorCode::InvalidState;
    switch (code) {
    case WorldStateDomainCode::OptionsInvalid:
    case WorldStateDomainCode::StateInvalid:
    case WorldStateDomainCode::RecordInvalid:
    case WorldStateDomainCode::VocabularyViolation:
    case WorldStateDomainCode::PayloadMalformed:
        error.code = ErrorCode::InvalidArgument;
        break;
    case WorldStateDomainCode::PayloadTooLarge:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case WorldStateDomainCode::SchemaUnsupported:
        error.code = ErrorCode::UnsupportedVersion;
        break;
    case WorldStateDomainCode::StaleTargetMissing:
        error.code = ErrorCode::InvalidState;
        break;
    }
    error.domain = std::string(kDomain);
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = false;
    error.safe_message = std::move(detail);
    return error;
}

// ---------------------------------------------------------------------------
// Value-type validation (§4.1). Every failure carries StateInvalid: nested
// and vocabulary-class value failures fold to StateInvalid and validate()
// never distinguishes vocabulary semantics (§4.5 folding rule, B2 revision).
// ---------------------------------------------------------------------------

Result<void> WorldStateOptions::validate() const {
    if (max_entities == 0 || max_recent_changes == 0 || max_change_summary_bytes == 0) {
        return make_world_state_error(WorldStateDomainCode::OptionsInvalid,
                                      "world state option capacities must be positive");
    }
    return Result<void>{};
}

Result<void> WorldForegroundApp::validate() const {
    if (static_cast<std::uint8_t>(status) > static_cast<std::uint8_t>(WorldBeliefStatus::Unknown)) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "foreground status is outside the belief closed set");
    }
    if ((status == WorldBeliefStatus::Unknown) != package_name.empty()) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "foreground Unknown <=> empty package_name violated");
    }
    return Result<void>{};
}

Result<void> WorldPageAssumption::validate() const {
    if (static_cast<std::uint8_t>(status) > static_cast<std::uint8_t>(WorldBeliefStatus::Unknown)) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "page status is outside the belief closed set");
    }
    if (status == WorldBeliefStatus::Unknown) {
        if (!app_id.empty() || !state_id.empty() || confidence != 0.0) {
            return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                          "page Unknown requires empty pair and zero confidence");
        }
        return Result<void>{};
    }
    if (app_id.empty() || state_id.empty()) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "non-Unknown page requires a non-empty pair");
    }
    if (!confidence_in_range(confidence)) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "page confidence outside [0,1]");
    }
    return Result<void>{};
}

Result<void> WorldEntity::validate() const {
    if (element.observation_id.is_nil()) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "entity element observation_id is nil");
    }
    if (element.space.is_nil()) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "entity element space is invalid");
    }
    if (static_cast<std::uint8_t>(element.source) >
        static_cast<std::uint8_t>(ElementSource::Unknown)) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "entity element source is outside the closed set");
    }
    if (!confidence_in_range(confidence)) {
        return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                      "entity confidence outside [0,1]");
    }
    return Result<void>{};
}

Result<void> WorldState::validate() const {
    if (const Result<void> foreground = foreground_app.validate(); !foreground.has_value()) {
        return foreground.error();
    }
    if (const Result<void> page = current_page.validate(); !page.has_value()) {
        return page.error();
    }
    for (const auto &entity : entities) {
        if (const Result<void> entity_ok = entity.validate(); !entity_ok.has_value()) {
            return entity_ok.error();
        }
    }
    for (std::size_t index = 0; index < recent_changes.size(); ++index) {
        const auto &change = recent_changes[index];
        if (static_cast<std::uint8_t>(change.kind) >
            static_cast<std::uint8_t>(WorldChangeKind::EntityEvicted)) {
            return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                          "change kind is outside the closed set");
        }
        if (index > 0 && change.change_sequence <= recent_changes[index - 1].change_sequence) {
            return make_world_state_error(WorldStateDomainCode::StateInvalid,
                                          "change_sequence regression");
        }
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Update operators (§4.3; ordering of checks: options, vocabulary, record).
// ---------------------------------------------------------------------------

Result<WorldState> apply_world_observation(WorldState state, const WorldObservationInput &input,
                                           const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    if (input.observation_id.is_nil()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "observation input observation_id is nil");
    }
    for (const auto &element : input.elements) {
        if (element.observation_id.is_nil()) {
            return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                          "observation element observation_id is nil");
        }
        if (element.space.is_nil()) {
            return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                          "observation element space is invalid");
        }
    }
    if (input.elements.size() > options.max_entities) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "observation elements exceed options.max_entities");
    }

    WorldState next = std::move(state);
    const Sha256Digest record_digest = ws_detail::input_digest(input);

    WorldForegroundApp desired;
    if (!input.app.package_name.empty()) {
        desired.status = WorldBeliefStatus::Believed;
        desired.package_name = input.app.package_name;
        desired.activity_name = input.app.activity_name;
        desired.sensitive = input.app.sensitive;
        desired.observed_at = input.observed_at;
    }
    // §4.3: the projected foreground is overwritten field-by-field on every
    // observation, so a same-content later observation still refreshes
    // observed_at; the content comparison only gates the PageAssumed entry.
    const bool foreground_changed = !same_foreground_content(next.foreground_app, desired);
    next.foreground_app = std::move(desired);
    if (foreground_changed) {
        const std::string summary = next.foreground_app.status == WorldBeliefStatus::Unknown
                                        ? std::string("foreground unknown")
                                        : "foreground " + next.foreground_app.package_name + "/" +
                                              next.foreground_app.activity_name +
                                              (next.foreground_app.sensitive ? " sensitive" : "");
        if (const Result<void> appended = append_change(next, WorldChangeKind::PageAssumed, summary,
                                                        record_digest, input.observed_at, options);
            !appended.has_value()) {
            return appended.error();
        }
    }

    std::size_t added = 0;
    std::size_t refreshed = 0;
    for (const auto &element : input.elements) {
        const auto existing = std::find_if(next.entities.begin(), next.entities.end(),
                                           [&element](const WorldEntity &entity) {
                                               return same_element(entity.element, element);
                                           });
        if (existing != next.entities.end()) {
            existing->confidence = 1.0;
            existing->stale = false;
            existing->last_seen_at = input.observed_at;
            existing->source_event_digest = record_digest;
            ++refreshed;
            continue;
        }
        while (next.entities.size() >= options.max_entities) {
            if (const Result<void> evicted =
                    evict_one(next, record_digest, input.observed_at, options);
                !evicted.has_value()) {
                return evicted.error();
            }
        }
        WorldEntity entity;
        entity.element = element;
        entity.confidence = 1.0;
        entity.stale = false;
        entity.last_seen_at = input.observed_at;
        entity.source_event_digest = record_digest;
        next.entities.push_back(std::move(entity));
        ++added;
    }
    if (added + refreshed > 0) {
        const std::string summary =
            "entities added=" + std::to_string(added) + " refreshed=" + std::to_string(refreshed);
        if (const Result<void> appended =
                append_change(next, WorldChangeKind::EntityObserved, summary, record_digest,
                              input.observed_at, options);
            !appended.has_value()) {
            return appended.error();
        }
    }
    return next;
}

Result<WorldState> apply_screen_state(WorldState state, const WorldScreenStateInput &input,
                                      const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    if (input.app_id.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "screen state input app_id is empty");
    }
    if (input.state_id.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "screen state input state_id is empty");
    }
    if (!confidence_in_range(input.confidence)) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "screen state input confidence outside [0,1]");
    }

    WorldState next = std::move(state);
    const Sha256Digest record_digest = ws_detail::input_digest(input);

    WorldPageAssumption believed;
    believed.status = WorldBeliefStatus::Believed;
    believed.app_id = input.app_id;
    believed.state_id = input.state_id;
    believed.confidence = input.confidence;
    believed.recognized_at = input.recognized_at;
    next.current_page = std::move(believed);

    const std::string summary = "page " + input.app_id + "/" + input.state_id;
    if (const Result<void> appended = append_change(next, WorldChangeKind::PageAssumed, summary,
                                                    record_digest, input.recognized_at, options);
        !appended.has_value()) {
        return appended.error();
    }
    return next;
}

Result<WorldState> apply_navigation(WorldState state, const WorldNavigationInput &input,
                                    const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    if (input.app_id.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "navigation input app_id is empty");
    }
    if (input.from_state.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "navigation input from_state is empty");
    }
    if (input.to_state.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "navigation input to_state is empty");
    }
    if (!confidence_in_range(input.confidence)) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "navigation input confidence outside [0,1]");
    }

    WorldState next = std::move(state);
    const Sha256Digest record_digest = ws_detail::input_digest(input);

    // from_state reconciliation: the only ConflictMarked trigger (B1 revision).
    // It records the broken assumption chain and never changes the transition
    // semantics below.
    if (next.current_page.status != WorldBeliefStatus::Unknown &&
        (next.current_page.app_id != input.app_id ||
         next.current_page.state_id != input.from_state)) {
        const std::string summary = "conflict projected=" + next.current_page.app_id + "/" +
                                    next.current_page.state_id + " claimed=" + input.app_id + "/" +
                                    input.from_state;
        if (const Result<void> appended =
                append_change(next, WorldChangeKind::ConflictMarked, summary, record_digest,
                              input.observed_at, options);
            !appended.has_value()) {
            return appended.error();
        }
    }

    if (input.success) {
        WorldPageAssumption believed;
        believed.status = WorldBeliefStatus::Believed;
        believed.app_id = input.app_id;
        believed.state_id = input.to_state;
        believed.confidence = input.confidence;
        believed.recognized_at = input.observed_at;
        next.current_page = std::move(believed);
    } else if (next.current_page.status != WorldBeliefStatus::Unknown) {
        // Stale keeps confidence and recognized_at: staleness keeps counting
        // from the original recognition instant.
        next.current_page.status = WorldBeliefStatus::Stale;
    }

    const std::string summary = "navigation " + input.app_id + " " + input.from_state + "->" +
                                input.to_state + (input.success ? " success" : " failed");
    if (const Result<void> appended =
            append_change(next, WorldChangeKind::NavigationObserved, summary, record_digest,
                          input.observed_at, options);
        !appended.has_value()) {
        return appended.error();
    }
    return next;
}

Result<WorldState> apply_tool_settled(WorldState state, const WorldToolSettledInput &input,
                                      const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    if (input.tool_ref.empty()) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "tool settle input tool_ref is empty");
    }
    if (input.tool_ref.size() > kToolRefMaxBytes) {
        return make_world_state_error(WorldStateDomainCode::VocabularyViolation,
                                      "tool settle input tool_ref exceeds 256 bytes");
    }
    if (!closed_set_contains(input.disposition, kDispositions)) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "tool settle disposition is outside the closed set");
    }

    WorldState next = std::move(state);
    const Sha256Digest record_digest = ws_detail::input_digest(input);
    const std::string summary = "tool settled " + input.disposition + " " + input.tool_ref;
    if (const Result<void> appended = append_change(next, WorldChangeKind::ToolSettled, summary,
                                                    record_digest, input.settled_at, options);
        !appended.has_value()) {
        return appended.error();
    }
    return next;
}

Result<WorldState> apply_verification(WorldState state, const WorldVerificationInput &input,
                                      const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    if (!closed_set_contains(input.outcome, kOutcomes)) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "verification outcome is outside the closed set");
    }
    if (!confidence_in_range(input.confidence)) {
        return make_world_state_error(WorldStateDomainCode::RecordInvalid,
                                      "verification confidence outside [0,1]");
    }

    WorldState next = std::move(state);
    if (next.current_page.status == WorldBeliefStatus::Unknown) {
        return make_world_state_error(WorldStateDomainCode::StaleTargetMissing,
                                      "verification target page is Unknown");
    }
    const Sha256Digest record_digest = ws_detail::input_digest(input);

    if (input.outcome == "confirmed" || input.outcome == "refuted") {
        next.current_page.status =
            input.outcome == "confirmed" ? WorldBeliefStatus::Believed : WorldBeliefStatus::Stale;
        next.current_page.confidence = input.confidence;
        next.current_page.recognized_at = input.verified_at;
    }

    const std::string summary = "verification " + input.outcome;
    if (const Result<void> appended =
            append_change(next, WorldChangeKind::VerificationSettled, summary, record_digest,
                          input.verified_at, options);
        !appended.has_value()) {
        return appended.error();
    }
    return next;
}

Result<WorldState> apply_expiry(WorldState state, const WorldExpiryInput &input,
                                const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }

    WorldState next = std::move(state);
    const Sha256Digest record_digest = ws_detail::input_digest(input);
    const auto staleness_bound =
        std::chrono::duration_cast<std::chrono::nanoseconds>(options.stale_after);

    for (std::size_t index = 0; index < next.entities.size(); ++index) {
        WorldEntity &entity = next.entities[index];
        const auto age = input.now.wall - entity.last_seen_at.wall;
        if (entity.stale || age <= staleness_bound) {
            continue;
        }
        entity.stale = true;
        entity.confidence *= 0.5;
        const std::string summary =
            "entity staled observation_id=" + entity.element.observation_id.to_string();
        if (const Result<void> appended = append_change(next, WorldChangeKind::EntityStaled,
                                                        summary, record_digest, input.now, options);
            !appended.has_value()) {
            return appended.error();
        }
    }
    if (next.current_page.status == WorldBeliefStatus::Believed) {
        const auto age = input.now.wall - next.current_page.recognized_at.wall;
        if (age > staleness_bound) {
            next.current_page.status = WorldBeliefStatus::Stale;
            next.current_page.confidence *= 0.5;
            const std::string summary =
                "page staled " + next.current_page.app_id + "/" + next.current_page.state_id;
            if (const Result<void> appended =
                    append_change(next, WorldChangeKind::EntityStaled, summary, record_digest,
                                  input.now, options);
                !appended.has_value()) {
                return appended.error();
            }
        }
    }
    return next;
}

WorldState prune_entities(WorldState state, const WorldStateOptions &options) {
    WorldState next = std::move(state);
    const Sha256Digest no_cause{};
    while (next.entities.size() > options.max_entities) {
        const auto victim = eviction_candidate(next);
        const std::string summary =
            "entity evicted observation_id=" + victim->element.observation_id.to_string();
        append_change_unchecked(next, WorldChangeKind::EntityEvicted, summary, no_cause,
                                Timestamp{}, options);
        next.entities.erase(victim);
    }
    return next;
}

Result<WorldState> rebuild_world_state(std::span<const WorldStateInput> inputs,
                                       const WorldStateOptions &options) {
    if (const Result<void> options_ok = options.validate(); !options_ok.has_value()) {
        return options_ok.error();
    }
    WorldState state;
    for (const auto &record : inputs) {
        Result<WorldState> applied = std::visit(
            [&state, &options](const auto &input) -> Result<WorldState> {
                using InputType = std::decay_t<decltype(input)>;
                if constexpr (std::is_same_v<InputType, WorldObservationInput>) {
                    return apply_world_observation(std::move(state), input, options);
                } else if constexpr (std::is_same_v<InputType, WorldScreenStateInput>) {
                    return apply_screen_state(std::move(state), input, options);
                } else if constexpr (std::is_same_v<InputType, WorldNavigationInput>) {
                    return apply_navigation(std::move(state), input, options);
                } else if constexpr (std::is_same_v<InputType, WorldToolSettledInput>) {
                    return apply_tool_settled(std::move(state), input, options);
                } else if constexpr (std::is_same_v<InputType, WorldVerificationInput>) {
                    return apply_verification(std::move(state), input, options);
                } else {
                    return apply_expiry(std::move(state), input, options);
                }
            },
            record);
        if (!applied.has_value()) {
            return applied;
        }
        state = std::move(applied).value();
    }
    return state;
}

} // namespace mira
