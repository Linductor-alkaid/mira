#include <mira/workflow_navigation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <utility>

namespace mira {
namespace {

constexpr SchemaVersion kAppModelReaderVersion{1, 0};

// Hard ceilings behind AppModelLimits; callers may tighten, never relax.
constexpr std::size_t kCeilingDocumentBytes = 512 * 1024;
constexpr std::size_t kCeilingStates = 1024;
constexpr std::size_t kCeilingTransitions = 4096;
constexpr std::size_t kCeilingStringBytes = 8 * 1024;
constexpr std::size_t kCeilingSummaryBytes = 2048;
constexpr std::size_t kCeilingArgumentsBytes = 64 * 1024;
constexpr std::size_t kCeilingActionDepth = 16;

const AppModelLimits &clamp_limits(const AppModelLimits &limits) {
    static thread_local AppModelLimits clamped;
    clamped = limits;
    clamped.max_document_bytes = std::min(clamped.max_document_bytes, kCeilingDocumentBytes);
    clamped.max_states = std::min(clamped.max_states, kCeilingStates);
    clamped.max_transitions = std::min(clamped.max_transitions, kCeilingTransitions);
    clamped.max_string_bytes = std::min(clamped.max_string_bytes, kCeilingStringBytes);
    clamped.max_summary_bytes = std::min(clamped.max_summary_bytes, kCeilingSummaryBytes);
    clamped.max_arguments_bytes = std::min(clamped.max_arguments_bytes, kCeilingArgumentsBytes);
    return clamped;
}

[[nodiscard]] Error model_error(AppModelError code, std::string detail) {
    return make_app_model_error(code, std::move(detail));
}

// Exact key-set check with the same fail-closed semantics as the IR decoder.
[[nodiscard]] Result<void> check_keys(const JsonValue &object,
                                      std::span<const std::string_view> required,
                                      std::span<const std::string_view> optional,
                                      std::string_view where) {
    if (!object.is_object()) {
        return model_error(AppModelError::InvalidShape, std::string{where} + " must be an object");
    }
    for (const auto &member : *object.as_object()) {
        const bool known = std::any_of(required.begin(), required.end(),
                                       [&](std::string_view key) { return key == member.first; }) ||
                           std::any_of(optional.begin(), optional.end(),
                                       [&](std::string_view key) { return key == member.first; });
        if (!known) {
            return model_error(AppModelError::UnknownField,
                               std::string{where} + ": unknown field '" + member.first + "'");
        }
    }
    for (const auto key : required) {
        if (object.find(key) == nullptr) {
            return model_error(AppModelError::InvalidShape,
                               std::string{where} + ": missing field '" + std::string{key} + "'");
        }
    }
    return Result<void>{};
}

[[nodiscard]] std::optional<std::string> bounded_string(const JsonValue &value,
                                                        std::size_t max_bytes) {
    const auto *text = value.as_string();
    if (text == nullptr || text->empty() || text->size() > max_bytes) {
        return std::nullopt;
    }
    return *text;
}

[[nodiscard]] Result<std::uint64_t> parse_time_member(const JsonValue &json, const char *key,
                                                      std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_integer()) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be an integer");
    }
    const auto value = member->as_integer().value();
    if (value < 0) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be non-negative");
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] Result<std::uint32_t> parse_count_member(const JsonValue &json, const char *key,
                                                       std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_integer()) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be an integer");
    }
    const auto value = member->as_integer().value();
    if (value < 0 || value > static_cast<std::int64_t>(UINT32_MAX)) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] Result<double> parse_unit_member(const JsonValue &json, const char *key,
                                               std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_number()) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be a number");
    }
    const double value = member->as_number().value();
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be within [0,1]");
    }
    return value;
}

[[nodiscard]] Result<double> parse_non_negative_member(const JsonValue &json, const char *key,
                                                       std::string_view where) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_number()) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be a number");
    }
    const double value = member->as_number().value();
    if (!std::isfinite(value) || value < 0.0) {
        return model_error(AppModelError::InvalidShape,
                           std::string{where} + ": '" + key + "' must be non-negative");
    }
    return value;
}

[[nodiscard]] JsonValue confidence_to_json(const ConfidenceRecord &record) {
    JsonValue::Object object;
    object.emplace_back("confidence", record.confidence);
    object.emplace_back("observed_at_ms", static_cast<std::int64_t>(record.observed_at_ms));
    object.emplace_back("last_verified_ms", static_cast<std::int64_t>(record.last_verified_ms));
    object.emplace_back("verified_count", static_cast<std::int64_t>(record.verified_count));
    object.emplace_back("failure_count", static_cast<std::int64_t>(record.failure_count));
    object.emplace_back("source", app_model_source_name(record.source));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<ConfidenceRecord> parse_confidence(const JsonValue &json,
                                                        const AppModelLimits &limits) {
    static constexpr std::string_view kRequired[] = {
        "confidence",     "observed_at_ms", "last_verified_ms",
        "verified_count", "failure_count",  "source",
    };
    if (auto check = check_keys(json, kRequired, {}, "confidence"); !check.has_value()) {
        return check.error();
    }
    ConfidenceRecord record;
    const auto confidence = parse_unit_member(json, "confidence", "confidence");
    if (!confidence.has_value()) {
        return confidence.error();
    }
    record.confidence = confidence.value();
    auto observed = parse_time_member(json, "observed_at_ms", "confidence");
    if (!observed.has_value()) {
        return observed.error();
    }
    record.observed_at_ms = observed.value();
    auto verified_at = parse_time_member(json, "last_verified_ms", "confidence");
    if (!verified_at.has_value()) {
        return verified_at.error();
    }
    record.last_verified_ms = verified_at.value();
    auto verified = parse_count_member(json, "verified_count", "confidence");
    if (!verified.has_value()) {
        return verified.error();
    }
    record.verified_count = verified.value();
    auto failures = parse_count_member(json, "failure_count", "confidence");
    if (!failures.has_value()) {
        return failures.error();
    }
    record.failure_count = failures.value();
    const auto source = bounded_string(*json.find("source"), limits.max_string_bytes);
    if (!source) {
        return model_error(AppModelError::InvalidShape, "confidence source must be bounded");
    }
    auto parsed_source = parse_app_model_source(*source);
    if (!parsed_source.has_value()) {
        return model_error(AppModelError::InvalidShape, "confidence source is unknown");
    }
    record.source = parsed_source.value();
    return record;
}

[[nodiscard]] JsonValue costs_to_json(const NavigationCosts &costs) {
    JsonValue::Object object;
    object.emplace_back("latency_ms", costs.latency_ms);
    object.emplace_back("failure_probability", costs.failure_probability);
    object.emplace_back("risk", costs.risk);
    object.emplace_back("energy", costs.energy);
    object.emplace_back("model_cost", costs.model_cost);
    object.emplace_back("agent_required", costs.agent_required);
    object.emplace_back("vision_required", costs.vision_required);
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<NavigationCosts> parse_costs(const JsonValue &json) {
    static constexpr std::string_view kRequired[] = {
        "latency_ms",     "failure_probability", "risk", "energy", "model_cost",
        "agent_required", "vision_required",
    };
    if (auto check = check_keys(json, kRequired, {}, "costs"); !check.has_value()) {
        return check.error();
    }
    NavigationCosts costs;
    auto latency = parse_non_negative_member(json, "latency_ms", "costs");
    if (!latency.has_value()) {
        return latency.error();
    }
    costs.latency_ms = latency.value();
    auto failure = parse_unit_member(json, "failure_probability", "costs");
    if (!failure.has_value()) {
        return failure.error();
    }
    costs.failure_probability = failure.value();
    auto risk = parse_unit_member(json, "risk", "costs");
    if (!risk.has_value()) {
        return risk.error();
    }
    costs.risk = risk.value();
    auto energy = parse_unit_member(json, "energy", "costs");
    if (!energy.has_value()) {
        return energy.error();
    }
    costs.energy = energy.value();
    auto model_cost = parse_non_negative_member(json, "model_cost", "costs");
    if (!model_cost.has_value()) {
        return model_cost.error();
    }
    costs.model_cost = model_cost.value();
    const auto *agent = json.find("agent_required");
    if (agent == nullptr || !agent->is_boolean()) {
        return model_error(AppModelError::InvalidShape,
                           "costs: 'agent_required' must be a boolean");
    }
    costs.agent_required = *agent->as_boolean();
    const auto *vision = json.find("vision_required");
    if (vision == nullptr || !vision->is_boolean()) {
        return model_error(AppModelError::InvalidShape,
                           "costs: 'vision_required' must be a boolean");
    }
    costs.vision_required = *vision->as_boolean();
    return costs;
}

[[nodiscard]] Result<std::optional<WorkflowPredicate>> parse_guard(const JsonValue &json,
                                                                   const AppModelLimits &limits) {
    if (json.is_null()) {
        return std::optional<WorkflowPredicate>{};
    }
    static constexpr std::string_view kRequired[] = {"signal", "op"};
    static constexpr std::string_view kOptional[] = {"value"};
    if (auto check = check_keys(json, kRequired, kOptional, "guard"); !check.has_value()) {
        return check.error();
    }
    WorkflowPredicate predicate;
    const auto signal = bounded_string(*json.find("signal"), limits.max_string_bytes);
    if (!signal) {
        return model_error(AppModelError::InvalidShape, "guard signal must be a bounded string");
    }
    predicate.signal = *signal;
    const auto *op_text = json.find("op");
    if (op_text == nullptr || !op_text->is_string()) {
        return model_error(AppModelError::InvalidShape, "guard op must be a string");
    }
    auto op = parse_workflow_predicate_op(*op_text->as_string());
    if (!op.has_value()) {
        return model_error(AppModelError::InvalidShape, "guard op is unknown");
    }
    predicate.op = op.value();
    const auto *value = json.find("value");
    if (predicate.op == WorkflowPredicateOp::Exists) {
        if (value != nullptr && !value->is_null()) {
            return model_error(AppModelError::InvalidShape,
                               "guard value must be absent for op 'exists'");
        }
        predicate.value = JsonValue{};
    } else {
        if (value == nullptr) {
            return model_error(AppModelError::InvalidShape, "guard value is required");
        }
        if (value->is_array() || value->is_object()) {
            return model_error(AppModelError::InvalidShape, "guard value must be a scalar");
        }
        predicate.value = *value;
    }
    WorkflowLimits guard_limits = kDefaultWorkflowLimits;
    guard_limits.max_string_bytes = limits.max_string_bytes;
    if (auto valid = validate_workflow_predicate(predicate, guard_limits); !valid.has_value()) {
        return model_error(AppModelError::InvalidShape, "guard predicate shape is invalid");
    }
    return std::optional<WorkflowPredicate>{predicate};
}

[[nodiscard]] JsonValue guard_to_json(const std::optional<WorkflowPredicate> &guard) {
    if (!guard.has_value()) {
        return JsonValue{};
    }
    JsonValue::Object object;
    object.emplace_back("signal", guard->signal);
    object.emplace_back("op", workflow_predicate_op_name(guard->op));
    if (guard->op != WorkflowPredicateOp::Exists) {
        object.emplace_back("value", guard->value);
    }
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<void> check_action_depth(const JsonValue &value, std::size_t depth) {
    if (depth > kCeilingActionDepth) {
        return model_error(AppModelError::LimitExceeded, "action nesting exceeds limit");
    }
    if (value.is_object()) {
        for (const auto &member : *value.as_object()) {
            if (auto check = check_action_depth(member.second, depth + 1); !check.has_value()) {
                return check;
            }
        }
    } else if (value.is_array()) {
        for (const auto &item : *value.as_array()) {
            if (auto check = check_action_depth(item, depth + 1); !check.has_value()) {
                return check;
            }
        }
    }
    return Result<void>{};
}

[[nodiscard]] JsonValue state_to_json(const AppModelState &state) {
    JsonValue::Object object;
    object.emplace_back("id", state.id);
    object.emplace_back("page", state.page);
    if (state.modal.has_value()) {
        object.emplace_back("modal", *state.modal);
    }
    if (state.context.has_value()) {
        object.emplace_back("context", *state.context);
    }
    if (!state.summary.empty()) {
        object.emplace_back("summary", state.summary);
    }
    object.emplace_back("confidence", confidence_to_json(state.confidence));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<AppModelState> parse_state(const JsonValue &json,
                                                const AppModelLimits &limits) {
    static constexpr std::string_view kRequired[] = {"id", "page", "confidence"};
    static constexpr std::string_view kOptional[] = {"modal", "context", "summary"};
    if (auto check = check_keys(json, kRequired, kOptional, "state"); !check.has_value()) {
        return check.error();
    }
    AppModelState state;
    const auto id = bounded_string(*json.find("id"), limits.max_string_bytes);
    if (!id) {
        return model_error(AppModelError::InvalidShape, "state id must be a bounded string");
    }
    state.id = *id;
    const auto page = bounded_string(*json.find("page"), limits.max_string_bytes);
    if (!page) {
        return model_error(AppModelError::InvalidShape, "state page must be a bounded string");
    }
    state.page = *page;
    for (const auto key : {"modal", "context"}) {
        if (const auto *member = json.find(key); member != nullptr && !member->is_null()) {
            const auto text = bounded_string(*member, limits.max_string_bytes);
            if (!text) {
                return model_error(AppModelError::InvalidShape,
                                   std::string{"state "} + key + " must be a bounded string");
            }
            if (std::string_view{key} == "modal") {
                state.modal = *text;
            } else {
                state.context = *text;
            }
        }
    }
    if (const auto *summary = json.find("summary"); summary != nullptr && !summary->is_null()) {
        const auto text = bounded_string(*summary, limits.max_summary_bytes);
        if (!text) {
            return model_error(AppModelError::InvalidShape, "state summary is not bounded");
        }
        state.summary = *text;
    }
    auto confidence = parse_confidence(*json.find("confidence"), limits);
    if (!confidence.has_value()) {
        return confidence.error();
    }
    state.confidence = confidence.value();
    return state;
}

[[nodiscard]] JsonValue transition_to_json(const AppModelTransition &transition) {
    JsonValue::Object object;
    object.emplace_back("id", transition.id);
    object.emplace_back("from_state", transition.from_state);
    object.emplace_back("to_state", transition.to_state);
    object.emplace_back("action", transition.action);
    object.emplace_back("guard", guard_to_json(transition.guard));
    object.emplace_back("costs", costs_to_json(transition.costs));
    object.emplace_back("confidence", confidence_to_json(transition.confidence));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<AppModelTransition> parse_transition(const JsonValue &json,
                                                          const AppModelLimits &limits) {
    static constexpr std::string_view kRequired[] = {"id",     "from_state", "to_state",
                                                     "action", "costs",      "confidence"};
    static constexpr std::string_view kOptional[] = {"guard"};
    if (auto check = check_keys(json, kRequired, kOptional, "transition"); !check.has_value()) {
        return check.error();
    }
    AppModelTransition transition;
    for (const auto &[key, target] : std::array<std::pair<std::string_view, std::string *>, 3>{
             std::pair{"id", &transition.id},
             std::pair{"from_state", &transition.from_state},
             std::pair{"to_state", &transition.to_state},
         }) {
        const auto text = bounded_string(*json.find(std::string{key}), limits.max_string_bytes);
        if (!text) {
            return model_error(AppModelError::InvalidShape, std::string{"transition "} +
                                                                std::string{key} +
                                                                " must be a bounded string");
        }
        *target = *text;
    }
    const auto *action = json.find("action");
    if (action == nullptr || !action->is_object()) {
        return model_error(AppModelError::InvalidShape, "transition action must be an object");
    }
    const auto *tool = action->find("tool");
    if (tool == nullptr || !tool->is_string() || tool->as_string()->empty()) {
        return model_error(AppModelError::InvalidShape,
                           "transition action must carry a string \"tool\" member");
    }
    if (to_json_string(*action).size() > limits.max_arguments_bytes) {
        return model_error(AppModelError::LimitExceeded, "transition action exceeds limit");
    }
    if (auto depth = check_action_depth(*action, 1); !depth.has_value()) {
        return depth.error();
    }
    transition.action = *action;
    if (const auto *guard = json.find("guard"); guard != nullptr && !guard->is_null()) {
        auto parsed = parse_guard(*guard, limits);
        if (!parsed.has_value()) {
            return parsed.error();
        }
        transition.guard = parsed.value();
    }
    auto costs = parse_costs(*json.find("costs"));
    if (!costs.has_value()) {
        return costs.error();
    }
    transition.costs = costs.value();
    auto confidence = parse_confidence(*json.find("confidence"), limits);
    if (!confidence.has_value()) {
        return confidence.error();
    }
    transition.confidence = confidence.value();
    return transition;
}

// --- Planner internals -------------------------------------------------------

// One edge on a candidate path, compared element-wise for deterministic
// tie-breaking (DEC-028 §1): cost first, then edge id, then target state.
struct PathStep final {
    double edge_cost = 0.0;
    const AppModelTransition *transition = nullptr;
};

[[nodiscard]] bool step_less(const PathStep &left, const PathStep &right) {
    if (left.edge_cost != right.edge_cost) {
        return left.edge_cost < right.edge_cost;
    }
    if (left.transition->id != right.transition->id) {
        return left.transition->id < right.transition->id;
    }
    return left.transition->to_state < right.transition->to_state;
}

[[nodiscard]] bool path_less(const std::vector<PathStep> &left,
                             const std::vector<PathStep> &right) {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        const bool left_first = step_less(left[index], right[index]);
        const bool right_first = step_less(right[index], left[index]);
        if (left_first != right_first) {
            return left_first;
        }
    }
    return left.size() < right.size();
}

struct FrontierNode final {
    double cost = 0.0;
    std::vector<PathStep> path;
    std::string state;
};

// Min-heap ordering: cost, then the lexicographic edge sequence.
struct FrontierGreater final {
    [[nodiscard]] bool operator()(const FrontierNode &left, const FrontierNode &right) const {
        if (left.cost != right.cost) {
            return left.cost > right.cost;
        }
        return path_less(right.path, left.path);
    }
};

} // namespace

Error make_app_model_error(AppModelError code, std::string detail) {
    Error error;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = "app model: " + std::move(detail);
    switch (code) {
    case AppModelError::UnknownField:
        error.code = ErrorCode::UnsupportedVersion;
        break;
    case AppModelError::VersionMismatch:
        error.code = ErrorCode::UnsupportedVersion;
        break;
    case AppModelError::LimitExceeded:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case AppModelError::InvalidReference:
    case AppModelError::DuplicateId:
    case AppModelError::InvalidShape:
        error.code = ErrorCode::InvalidArgument;
        break;
    }
    return error;
}

Error make_navigation_error(NavigationError code, std::string detail) {
    Error error;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = "navigation: " + std::move(detail);
    switch (code) {
    case NavigationError::UnknownFromState:
    case NavigationError::UnknownToState:
        error.code = ErrorCode::InvalidArgument;
        break;
    case NavigationError::NoPath:
    case NavigationError::BudgetExceeded:
        error.code = ErrorCode::ResourceExhausted;
        break;
    }
    return error;
}

const AppModelLimits kDefaultAppModelLimits{};

std::string app_model_source_name(AppModelSource source) {
    switch (source) {
    case AppModelSource::Host:
        return "host";
    case AppModelSource::Agent:
        return "agent";
    case AppModelSource::Trajectory:
        return "trajectory";
    }
    return "host";
}

Result<AppModelSource> parse_app_model_source(std::string_view name) {
    for (const auto source :
         {AppModelSource::Host, AppModelSource::Agent, AppModelSource::Trajectory}) {
        if (app_model_source_name(source) == name) {
            return source;
        }
    }
    return make_app_model_error(AppModelError::InvalidShape, "unknown app model source");
}

ConfidenceRecord note_transition_outcome(ConfidenceRecord record, bool success,
                                         std::uint64_t now_ms) {
    if (success) {
        ++record.verified_count;
        record.last_verified_ms = now_ms;
    } else {
        ++record.failure_count;
    }
    record.observed_at_ms = now_ms;
    record.confidence = static_cast<double>(record.verified_count + 1) /
                        static_cast<double>(record.verified_count + record.failure_count + 2);
    return record;
}

ConfidenceRecord apply_confidence_decay(ConfidenceRecord record, std::uint64_t now_ms,
                                        std::uint64_t half_life_ms) {
    if (half_life_ms == 0 || now_ms <= record.last_verified_ms) {
        return record;
    }
    const double elapsed = static_cast<double>(now_ms - record.last_verified_ms);
    const double factor = std::pow(0.5, elapsed / static_cast<double>(half_life_ms));
    record.confidence = std::clamp(record.confidence * factor, 0.0, 1.0);
    return record;
}

bool needs_exploration(const ConfidenceRecord &record, double threshold) {
    return record.confidence < threshold;
}

JsonValue app_model_to_json(const AppModel &model) {
    JsonValue::Object version;
    version.emplace_back("major", static_cast<std::int64_t>(model.schema_version.major));
    version.emplace_back("minor", static_cast<std::int64_t>(model.schema_version.minor));

    JsonValue::Object root;
    root.emplace_back("schema_version", JsonValue{std::move(version)});
    root.emplace_back("app_id", model.app_id);
    root.emplace_back("name", model.name);
    if (!model.summary.empty()) {
        root.emplace_back("summary", model.summary);
    }
    JsonValue::Array states;
    for (const auto &state : model.states) {
        states.push_back(state_to_json(state));
    }
    root.emplace_back("states", JsonValue{std::move(states)});
    JsonValue::Array transitions;
    for (const auto &transition : model.transitions) {
        transitions.push_back(transition_to_json(transition));
    }
    root.emplace_back("transitions", JsonValue{std::move(transitions)});
    return JsonValue{std::move(root)};
}

Result<AppModel> app_model_from_json(const JsonValue &json, const AppModelLimits &limits) {
    const AppModelLimits &bounded = clamp_limits(limits);
    static constexpr std::string_view kRequired[] = {"schema_version", "app_id", "name", "states",
                                                     "transitions"};
    static constexpr std::string_view kOptional[] = {"summary"};
    if (auto check = check_keys(json, kRequired, kOptional, "app model"); !check.has_value()) {
        return check.error();
    }
    if (to_json_string(json).size() > bounded.max_document_bytes) {
        return model_error(AppModelError::LimitExceeded, "app model document exceeds limit");
    }
    AppModel model;
    const auto *version = json.find("schema_version");
    if (!version->is_object()) {
        return model_error(AppModelError::InvalidShape, "schema_version must be an object");
    }
    static constexpr std::string_view kVersionKeys[] = {"major", "minor"};
    if (auto check = check_keys(*version, kVersionKeys, {}, "schema_version"); !check.has_value()) {
        return check.error();
    }
    const auto *major = version->find("major");
    const auto *minor = version->find("minor");
    if (!major->is_integer() || !minor->is_integer()) {
        return model_error(AppModelError::InvalidShape, "schema_version members must be integers");
    }
    const auto incoming_major = static_cast<std::uint16_t>(major->as_integer().value());
    const auto incoming_minor = static_cast<std::uint16_t>(minor->as_integer().value());
    if (incoming_major != kAppModelReaderVersion.major ||
        incoming_minor > kAppModelReaderVersion.minor) {
        return model_error(AppModelError::VersionMismatch,
                           "app model schema version is not supported");
    }
    model.schema_version = SchemaVersion{incoming_major, incoming_minor};
    const auto app_id = bounded_string(*json.find("app_id"), bounded.max_string_bytes);
    if (!app_id) {
        return model_error(AppModelError::InvalidShape, "app_id must be a bounded string");
    }
    model.app_id = *app_id;
    const auto name = bounded_string(*json.find("name"), bounded.max_string_bytes);
    if (!name) {
        return model_error(AppModelError::InvalidShape, "app model name must be bounded");
    }
    model.name = *name;
    if (const auto *summary = json.find("summary"); summary != nullptr && !summary->is_null()) {
        const auto text = bounded_string(*summary, bounded.max_summary_bytes);
        if (!text) {
            return model_error(AppModelError::InvalidShape, "app model summary is not bounded");
        }
        model.summary = *text;
    }
    const auto *states = json.find("states");
    if (!states->is_array()) {
        return model_error(AppModelError::InvalidShape, "states must be an array");
    }
    if (states->as_array()->size() > bounded.max_states) {
        return model_error(AppModelError::LimitExceeded, "state count exceeds limit");
    }
    std::set<std::string> state_ids;
    for (const auto &item : *states->as_array()) {
        auto state = parse_state(item, bounded);
        if (!state.has_value()) {
            return state.error();
        }
        if (!state_ids.insert(state.value().id).second) {
            return model_error(AppModelError::DuplicateId,
                               "duplicate state id '" + state.value().id + "'");
        }
        model.states.push_back(state.value());
    }
    const auto *transitions = json.find("transitions");
    if (!transitions->is_array()) {
        return model_error(AppModelError::InvalidShape, "transitions must be an array");
    }
    if (transitions->as_array()->size() > bounded.max_transitions) {
        return model_error(AppModelError::LimitExceeded, "transition count exceeds limit");
    }
    std::set<std::string> transition_ids;
    for (const auto &item : *transitions->as_array()) {
        auto transition = parse_transition(item, bounded);
        if (!transition.has_value()) {
            return transition.error();
        }
        if (!transition_ids.insert(transition.value().id).second) {
            return model_error(AppModelError::DuplicateId,
                               "duplicate transition id '" + transition.value().id + "'");
        }
        if (state_ids.count(transition.value().from_state) == 0 ||
            state_ids.count(transition.value().to_state) == 0) {
            return model_error(AppModelError::InvalidReference,
                               "transition '" + transition.value().id +
                                   "' references an undeclared state");
        }
        model.transitions.push_back(transition.value());
    }
    return model;
}

Result<AppModel> parse_app_model(std::string_view text, const AppModelLimits &limits) {
    const auto json = parse_json(text);
    if (!json.has_value()) {
        return make_app_model_error(AppModelError::InvalidShape, "app model document is not JSON");
    }
    return app_model_from_json(json.value(), limits);
}

Result<void> validate_app_model(const AppModel &model, const AppModelLimits &limits) {
    // Single source of validation semantics: canonicalize then re-decode. A
    // struct-built model that cannot survive its own round trip (dangling
    // references, duplicate ids, malformed actions, limits) is rejected with
    // the decoder's error.
    auto decoded = app_model_from_json(app_model_to_json(model), limits);
    if (!decoded.has_value()) {
        return decoded.error();
    }
    return Result<void>{};
}

Sha256Digest app_model_digest(const AppModel &model) {
    return canonical_json_digest(app_model_to_json(model));
}

bool NavigationCostProfile::valid() const noexcept {
    const auto finite_non_negative = [](double weight) {
        return std::isfinite(weight) && weight >= 0.0;
    };
    return finite_non_negative(latency) && finite_non_negative(failure) &&
           finite_non_negative(model_cost) && finite_non_negative(risk) &&
           finite_non_negative(energy);
}

double navigation_edge_cost(const NavigationCosts &costs, const NavigationCostProfile &profile) {
    return profile.latency * costs.latency_ms + profile.failure * costs.failure_probability +
           profile.model_cost * costs.model_cost + profile.risk * costs.risk +
           profile.energy * costs.energy;
}

Result<NavigationPlan> plan_navigation(const AppModel &model, const std::string &from_state,
                                       const std::string &to_state,
                                       const NavigationCostProfile &profile,
                                       const JsonValue &predicate_context,
                                       const NavigationPlanOptions &options) {
    if (!profile.valid()) {
        return make_navigation_error(NavigationError::BudgetExceeded,
                                     "cost profile weights must be finite and non-negative");
    }
    std::set<std::string> state_ids;
    for (const auto &state : model.states) {
        state_ids.insert(state.id);
    }
    if (state_ids.count(from_state) == 0) {
        return make_navigation_error(NavigationError::UnknownFromState,
                                     "from state '" + from_state + "' is not declared");
    }
    if (state_ids.count(to_state) == 0) {
        return make_navigation_error(NavigationError::UnknownToState,
                                     "to state '" + to_state + "' is not declared");
    }

    NavigationPlan plan;
    plan.from_state = from_state;
    plan.to_state = to_state;
    if (from_state == to_state) {
        plan.state_sequence = {from_state};
        JsonValue::Array empty;
        plan.plan_digest = canonical_json_digest(JsonValue{std::move(empty)});
        return plan;
    }

    // Outgoing edges per state, in declaration order.
    std::map<std::string, std::vector<const AppModelTransition *>> outgoing;
    for (const auto &transition : model.transitions) {
        outgoing[transition.from_state].push_back(&transition);
    }

    std::priority_queue<FrontierNode, std::vector<FrontierNode>, FrontierGreater> frontier;
    frontier.push(FrontierNode{0.0, {}, from_state});
    std::set<std::string> settled;
    std::size_t evaluations = 0;
    bool path_capped = false;
    std::optional<FrontierNode> found;

    while (!frontier.empty()) {
        FrontierNode current = frontier.top();
        frontier.pop();
        if (settled.count(current.state) != 0) {
            continue;
        }
        if (current.state == to_state) {
            found = std::move(current);
            break;
        }
        settled.insert(current.state);
        if (current.path.size() >= options.max_path_edges) {
            // Extending this path would exceed the length cap (RULE-08).
            path_capped = true;
            continue;
        }
        for (const AppModelTransition *transition : outgoing[current.state]) {
            if (evaluations >= options.max_edge_evaluations) {
                return make_navigation_error(NavigationError::BudgetExceeded,
                                             "edge evaluation budget exhausted");
            }
            ++evaluations;
            if (transition->costs.agent_required && !options.allow_agent_edges) {
                continue;
            }
            if (transition->guard.has_value()) {
                const auto verdict =
                    evaluate_workflow_predicate(*transition->guard, predicate_context);
                if (verdict == WorkflowPredicateResult::NotSatisfied) {
                    ++plan.guards_blocked;
                    continue;
                }
                if (verdict == WorkflowPredicateResult::NotEvaluable) {
                    ++plan.guards_unevaluable;
                    continue;
                }
            }
            FrontierNode next;
            next.cost = current.cost + navigation_edge_cost(transition->costs, profile);
            next.path = current.path;
            next.path.push_back(
                PathStep{navigation_edge_cost(transition->costs, profile), transition});
            next.state = transition->to_state;
            frontier.push(std::move(next));
        }
    }

    if (!found.has_value()) {
        if (path_capped) {
            return make_navigation_error(NavigationError::BudgetExceeded,
                                         "path length budget exhausted");
        }
        // Guard counters ride on the deterministic message so callers can
        // tell a disconnected graph from guard-excluded edges (DEC-028 §1).
        return make_navigation_error(
            NavigationError::NoPath,
            "no path from '" + from_state + "' to '" + to_state +
                "'; guards blocked=" + std::to_string(plan.guards_blocked) +
                ", unevaluable=" + std::to_string(plan.guards_unevaluable));
    }

    double total = 0.0;
    JsonValue::Array edge_ids;
    plan.state_sequence.push_back(from_state);
    for (const auto &step : found->path) {
        plan.transition_ids.push_back(step.transition->id);
        edge_ids.push_back(JsonValue{step.transition->id});
        plan.state_sequence.push_back(step.transition->to_state);
        total += step.edge_cost;
    }
    plan.total_cost = total;
    plan.plan_digest = canonical_json_digest(JsonValue{std::move(edge_ids)});
    return plan;
}

} // namespace mira
