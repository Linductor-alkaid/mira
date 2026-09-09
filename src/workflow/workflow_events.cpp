#include <mira/workflow_events.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace mira {
namespace {

constexpr std::size_t kMaxReasonCodeBytes = 128;

[[nodiscard]] Error event_error(ErrorCode code, std::string_view detail) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = "workflow event: ";
    error.safe_message += detail;
    return error;
}

// Strict member extraction: the object must declare exactly `required` keys
// (unknown or missing members fail closed, DEC-022 §5).
[[nodiscard]] Result<void> check_exact_keys(const JsonValue &payload,
                                            std::span<const std::string_view> required) {
    if (!payload.is_object()) {
        return event_error(ErrorCode::InvalidArgument, "payload must be an object");
    }
    if (payload.as_object()->size() != required.size()) {
        return event_error(ErrorCode::InvalidArgument, "payload key set mismatch");
    }
    for (const auto key : required) {
        if (payload.find(key) == nullptr) {
            return event_error(ErrorCode::InvalidArgument,
                               std::string{"payload is missing '"} + std::string{key} + "'");
        }
    }
    return Result<void>{};
}

constexpr std::string_view kRunStartedKeys[] = {
        "schema",
        "run_id",
        "workflow_id",
        "ir_digest",
        "parameters_digest",
        "policy",
};
constexpr std::string_view kStepStartedKeys[] = {
        "schema",
        "run_id",
        "step_id",
        "kind",
        "attempt",
};
constexpr std::string_view kStepSettledKeys[] = {
        "schema",
        "run_id",
        "step_id",
        "disposition",
        "verification",
        "safe_summary",
};
constexpr std::string_view kRunSettledKeys[] = {
        "schema",
        "run_id",
        "terminal_state",
        "run_epoch",
        "safe_summary",
};
constexpr std::string_view kPatchProposedKeys[] = {
        "schema",
        "patch_id",
        "run_id",
        "patch_digest",
        "target",
        "reason_code",
};
constexpr std::string_view kPatchAppliedKeys[] = {
        "schema",
        "patch_id",
        "run_id",
        "run_patch_epoch",
};
constexpr std::string_view kPatchRejectedKeys[] = {
        "schema",
        "patch_id",
        "run_id",
        "reason_code",
};
constexpr std::string_view kPolicySwitchedKeys[] = {
        "schema",
        "run_id",
        "from",
        "to",
};
constexpr std::string_view kDecisionRaisedKeys[] = {
        "schema",
        "run_id",
        "decision_id",
        "payload_digest",
};
constexpr std::string_view kDecisionResolvedKeys[] = {
        "schema",
        "run_id",
        "decision_id",
        "resolution",
};
constexpr std::string_view kPublishProposedKeys[] = {
        "schema",
        "workflow_id",
        "ir_digest",
        "source_run_id",
};
constexpr std::string_view kPublishAppliedKeys[] = {
        "schema",
        "workflow_id",
        "ir_digest",
        "evidence",
        "dry_run_id",
};
constexpr std::string_view kPublishRejectedKeys[] = {
        "schema",
        "workflow_id",
        "ir_digest",
        "reason_code",
};
constexpr std::string_view kNavigationPlannedKeys[] = {
        "schema",
        "run_id",
        "step_id",
        "from_state",
        "to_state",
        "edge_count",
        "plan_digest",
        "total_cost",
        "guards_blocked",
        "guards_unevaluable",
};
constexpr std::string_view kNavigationObservedKeys[] = {
        "schema",
        "run_id",
        "step_id",
        "transition_id",
        "from_state",
        "to_state",
        "success",
        "confidence",
};
constexpr std::string_view kEpisodeRecordedKeys[] = {
        "schema",
        "run_id",
        "workflow_id",
        "episode_digest",
        "outcome",
        "reason_code",
};
constexpr std::string_view kLessonRecordedKeys[] = {
        "schema",
        "run_id",
        "workflow_id",
        "lesson_digest",
        "outcome",
        "reason_code",
};
constexpr std::string_view kRecoveryAttemptedKeys[] = {
        "schema",
        "run_id",
        "workflow_id",
        "ordinal",
        "task_id",
        "outcome",
        "reason_code",
        "decision_digest",
        "patch_id",
        "model_request_id",
        "lessons_offered",
        "lessons_stale",
        "lessons_unparseable",
        "lessons_kept",
};

// App Model identifiers are host contract strings (DEC-027), bounded by the
// model limits; payloads enforce the same bound.
constexpr std::size_t kMaxNavigationIdBytes = 8 * 1024;

[[nodiscard]] Result<JsonValue> parse_payload_data(const EventPayload &payload) {
    auto json = parse_json(payload.data);
    if (!json.has_value()) {
        return json.error();
    }
    return json;
}

template <typename Id> [[nodiscard]] Result<Id> parse_member_id(const JsonValue &json, const char *key) {
    const auto *text = json.find(key);
    if (text == nullptr || !text->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' must be an id string");
    }
    auto id = Id::parse(*text->as_string());
    if (!id || id->is_nil()) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' is not a valid id");
    }
    return *id;
}

[[nodiscard]] Result<Sha256Digest> parse_member_digest(const JsonValue &json, const char *key) {
    const auto *text = json.find(key);
    if (text == nullptr || !text->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' must be a digest string");
    }
    auto digest = digest_from_hex(*text->as_string());
    if (!digest) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' is not a valid digest");
    }
    return *digest;
}

[[nodiscard]] Result<std::string> parse_member_name(const JsonValue &json, const char *key) {
    const auto *text = json.find(key);
    if (text == nullptr || !text->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' must be a string");
    }
    return *text->as_string();
}

// Named enum members go through their parser; this helper guards the string
// extraction so enum parsers never see a nullptr.
[[nodiscard]] Result<std::string> parse_member_enum_name(const JsonValue &json, const char *key) {
    return parse_member_name(json, key);
}

[[nodiscard]] Result<JsonValue> parse_payload(const EventPayload &payload,
                                              std::string_view type,
                                              std::string_view schema) {
    if (payload.type != type) {
        return event_error(ErrorCode::InvalidArgument, "event type mismatch");
    }
    auto json = parse_payload_data(payload);
    if (!json.has_value()) {
        return json.error();
    }
    const auto *declared = json.value().find("schema");
    if (declared == nullptr || !declared->is_string() || *declared->as_string() != schema) {
        return event_error(ErrorCode::UnsupportedVersion, "payload schema mismatch");
    }
    return json;
}

[[nodiscard]] std::string digest_text(const Sha256Digest &digest) {
    return digest.to_string();
}

} // namespace

bool is_workflow_event_type(std::string_view type) {
    static constexpr std::string_view kTypes[] = {
        "WorkflowRunStarted",   "WorkflowStepStarted",   "WorkflowStepSettled",
        "WorkflowRunSettled",   "WorkflowPatchProposed", "WorkflowPatchApplied",
        "WorkflowPatchRejected", "WorkflowPolicySwitched", "WorkflowDecisionRaised",
        "WorkflowDecisionResolved", "WorkflowPublishProposed", "WorkflowPublishApplied",
        "WorkflowPublishRejected", "WorkflowNavigationPlanned", "WorkflowNavigationObserved",
        "WorkflowEpisodeRecorded", "WorkflowLessonRecorded", "WorkflowRecoveryAttempted",
    };
    return std::any_of(std::begin(kTypes), std::end(kTypes),
                       [&](std::string_view candidate) { return candidate == type; });
}

std::string workflow_step_disposition_name(WorkflowStepDisposition disposition) {
    switch (disposition) {
    case WorkflowStepDisposition::Completed:
        return "completed";
    case WorkflowStepDisposition::Skipped:
        return "skipped";
    case WorkflowStepDisposition::Failed:
        return "failed";
    case WorkflowStepDisposition::Stale:
        return "stale";
    }
    return "unknown";
}

Result<WorkflowStepDisposition> parse_workflow_step_disposition(std::string_view name) {
    for (auto disposition : {WorkflowStepDisposition::Completed, WorkflowStepDisposition::Skipped,
                             WorkflowStepDisposition::Failed, WorkflowStepDisposition::Stale}) {
        if (workflow_step_disposition_name(disposition) == name) {
            return disposition;
        }
    }
    return event_error(ErrorCode::InvalidArgument, "unknown step disposition");
}

std::string workflow_patch_target_name(WorkflowPatchTarget target) {
    switch (target) {
    case WorkflowPatchTarget::RunParameters:
        return "run_parameters";
    case WorkflowPatchTarget::StepArguments:
        return "step_arguments";
    case WorkflowPatchTarget::ExecutionPolicy:
        return "execution_policy";
    }
    return "unknown";
}

Result<WorkflowPatchTarget> parse_workflow_patch_target(std::string_view name) {
    for (auto target : {WorkflowPatchTarget::RunParameters, WorkflowPatchTarget::StepArguments,
                        WorkflowPatchTarget::ExecutionPolicy}) {
        if (workflow_patch_target_name(target) == name) {
            return target;
        }
    }
    return event_error(ErrorCode::InvalidArgument, "unknown patch target");
}

std::string workflow_decision_resolution_name(WorkflowDecisionResolution resolution) {
    switch (resolution) {
    case WorkflowDecisionResolution::Accept:
        return "accept";
    case WorkflowDecisionResolution::Reject:
        return "reject";
    case WorkflowDecisionResolution::CancelRun:
        return "cancel_run";
    }
    return "unknown";
}

Result<WorkflowDecisionResolution>
parse_workflow_decision_resolution(std::string_view name) {
    for (auto resolution : {WorkflowDecisionResolution::Accept, WorkflowDecisionResolution::Reject,
                            WorkflowDecisionResolution::CancelRun}) {
        if (workflow_decision_resolution_name(resolution) == name) {
            return resolution;
        }
    }
    return event_error(ErrorCode::InvalidArgument, "unknown decision resolution");
}

EventPayload to_event_payload(const WorkflowRunStartedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.run-started.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("ir_digest", digest_text(event.ir_digest));
    object.emplace_back("parameters_digest", digest_text(event.parameters_digest));
    object.emplace_back("policy", workflow_policy_name(event.policy));
    EventPayload payload;
    payload.type = "WorkflowRunStarted";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::Critical;
    return payload;
}

EventPayload to_event_payload(const WorkflowStepStartedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.step-started.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("step_id", event.step_id.to_string());
    object.emplace_back("kind", workflow_step_kind_name(event.kind));
    object.emplace_back("attempt", static_cast<std::int64_t>(event.attempt));
    EventPayload payload;
    payload.type = "WorkflowStepStarted";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowStepSettledEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.step-settled.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("step_id", event.step_id.to_string());
    object.emplace_back("disposition", workflow_step_disposition_name(event.disposition));
    object.emplace_back("verification", event.verification);
    object.emplace_back("safe_summary", event.safe_summary);
    EventPayload payload;
    payload.type = "WorkflowStepSettled";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowRunSettledEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.run-settled.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("terminal_state", workflow_run_state_name(event.terminal_state));
    object.emplace_back("run_epoch", static_cast<std::int64_t>(event.run_epoch));
    object.emplace_back("safe_summary", event.safe_summary);
    EventPayload payload;
    payload.type = "WorkflowRunSettled";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::Critical;
    return payload;
}

EventPayload to_event_payload(const WorkflowPatchProposedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.patch-proposed.v1");
    object.emplace_back("patch_id", event.patch_id.to_string());
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("patch_digest", digest_text(event.patch_digest));
    object.emplace_back("target", workflow_patch_target_name(event.target));
    object.emplace_back("reason_code", event.reason_code);
    EventPayload payload;
    payload.type = "WorkflowPatchProposed";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowPatchAppliedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.patch-applied.v1");
    object.emplace_back("patch_id", event.patch_id.to_string());
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("run_patch_epoch", static_cast<std::int64_t>(event.run_patch_epoch));
    EventPayload payload;
    payload.type = "WorkflowPatchApplied";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowPatchRejectedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.patch-rejected.v1");
    object.emplace_back("patch_id", event.patch_id.to_string());
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("reason_code", event.reason_code);
    EventPayload payload;
    payload.type = "WorkflowPatchRejected";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowPolicySwitchedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.policy-switched.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("from", workflow_policy_name(event.from));
    object.emplace_back("to", workflow_policy_name(event.to));
    EventPayload payload;
    payload.type = "WorkflowPolicySwitched";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowDecisionRaisedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.decision-raised.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("decision_id", event.decision_id.to_string());
    object.emplace_back("payload_digest", digest_text(event.payload_digest));
    EventPayload payload;
    payload.type = "WorkflowDecisionRaised";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowDecisionResolvedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.decision-resolved.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("decision_id", event.decision_id.to_string());
    object.emplace_back("resolution", workflow_decision_resolution_name(event.resolution));
    EventPayload payload;
    payload.type = "WorkflowDecisionResolved";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

Result<WorkflowRunStartedEvent> parse_workflow_run_started(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowRunStarted", "mira.workflow.run-started.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kRunStartedKeys); !check.has_value()) {
        return check.error();
    }
    WorkflowRunStartedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto ir_digest = parse_member_digest(json.value(), "ir_digest");
    if (!ir_digest.has_value()) {
        return ir_digest.error();
    }
    event.ir_digest = ir_digest.value();
    auto parameters_digest = parse_member_digest(json.value(), "parameters_digest");
    if (!parameters_digest.has_value()) {
        return parameters_digest.error();
    }
    event.parameters_digest = parameters_digest.value();
    auto policy_name = parse_member_enum_name(json.value(), "policy");
    if (!policy_name.has_value()) {
        return policy_name.error();
    }
    auto parsed_policy = parse_workflow_policy(policy_name.value());
    if (!parsed_policy.has_value()) {
        return parsed_policy.error();
    }
    event.policy = parsed_policy.value();
    return event;
}

Result<WorkflowStepStartedEvent> parse_workflow_step_started(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowStepStarted", "mira.workflow.step-started.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kStepStartedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowStepStartedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto step_id = parse_member_id<StepId>(json.value(), "step_id");
    if (!step_id.has_value()) {
        return step_id.error();
    }
    event.step_id = step_id.value();
    auto kind_name = parse_member_enum_name(json.value(), "kind");
    if (!kind_name.has_value()) {
        return kind_name.error();
    }
    auto parsed_kind = parse_workflow_step_kind(kind_name.value());
    if (!parsed_kind.has_value()) {
        return parsed_kind.error();
    }
    event.kind = parsed_kind.value();
    const auto *attempt = json.value().find("attempt");
    if (!attempt->is_integer() || attempt->as_integer().value() < 1) {
        return event_error(ErrorCode::InvalidArgument, "attempt must be a positive integer");
    }
    event.attempt = static_cast<std::uint32_t>(attempt->as_integer().value());
    return event;
}

Result<WorkflowStepSettledEvent> parse_workflow_step_settled(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowStepSettled", "mira.workflow.step-settled.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kStepSettledKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowStepSettledEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto step_id = parse_member_id<StepId>(json.value(), "step_id");
    if (!step_id.has_value()) {
        return step_id.error();
    }
    event.step_id = step_id.value();
    auto disposition_name = parse_member_enum_name(json.value(), "disposition");
    if (!disposition_name.has_value()) {
        return disposition_name.error();
    }
    auto parsed_disposition = parse_workflow_step_disposition(disposition_name.value());
    if (!parsed_disposition.has_value()) {
        return parsed_disposition.error();
    }
    event.disposition = parsed_disposition.value();
    auto verification = parse_member_name(json.value(), "verification");
    if (!verification.has_value()) {
        return verification.error();
    }
    if (verification.value().size() > kWorkflowEventMaxSummaryBytes) {
        return event_error(ErrorCode::InvalidArgument, "verification summary exceeds limit");
    }
    event.verification = verification.value();
    auto summary = parse_member_name(json.value(), "safe_summary");
    if (!summary.has_value()) {
        return summary.error();
    }
    if (summary.value().size() > kWorkflowEventMaxSummaryBytes) {
        return event_error(ErrorCode::InvalidArgument, "safe summary exceeds limit");
    }
    event.safe_summary = summary.value();
    return event;
}

Result<WorkflowRunSettledEvent> parse_workflow_run_settled(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowRunSettled", "mira.workflow.run-settled.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kRunSettledKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowRunSettledEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto terminal_name = parse_member_enum_name(json.value(), "terminal_state");
    if (!terminal_name.has_value()) {
        return terminal_name.error();
    }
    auto state = parse_workflow_run_state(terminal_name.value());
    if (!state.has_value()) {
        return state.error();
    }
    if (!is_terminal(state.value())) {
        return event_error(ErrorCode::InvalidArgument,
                           "run settled terminal_state must be terminal");
    }
    event.terminal_state = state.value();
    const auto *epoch = json.value().find("run_epoch");
    if (!epoch->is_integer() || epoch->as_integer().value() < 0) {
        return event_error(ErrorCode::InvalidArgument, "run_epoch must be a non-negative integer");
    }
    event.run_epoch = static_cast<std::uint64_t>(epoch->as_integer().value());
    auto summary = parse_member_name(json.value(), "safe_summary");
    if (!summary.has_value()) {
        return summary.error();
    }
    if (summary.value().size() > kWorkflowEventMaxSummaryBytes) {
        return event_error(ErrorCode::InvalidArgument, "safe summary exceeds limit");
    }
    event.safe_summary = summary.value();
    return event;
}

Result<WorkflowPatchProposedEvent> parse_workflow_patch_proposed(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPatchProposed",
                              "mira.workflow.patch-proposed.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kPatchProposedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPatchProposedEvent event;
    auto patch_id = parse_member_id<WorkflowPatchId>(json.value(), "patch_id");
    if (!patch_id.has_value()) {
        return patch_id.error();
    }
    event.patch_id = patch_id.value();
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto digest = parse_member_digest(json.value(), "patch_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.patch_digest = digest.value();
    auto target_name = parse_member_enum_name(json.value(), "target");
    if (!target_name.has_value()) {
        return target_name.error();
    }
    auto parsed_target = parse_workflow_patch_target(target_name.value());
    if (!parsed_target.has_value()) {
        return parsed_target.error();
    }
    event.target = parsed_target.value();
    auto reason = parse_member_name(json.value(), "reason_code");
    if (!reason.has_value()) {
        return reason.error();
    }
    if (reason.value().empty() || reason.value().size() > kMaxReasonCodeBytes) {
        return event_error(ErrorCode::InvalidArgument, "reason_code is not bounded");
    }
    event.reason_code = reason.value();
    return event;
}

Result<WorkflowPatchAppliedEvent> parse_workflow_patch_applied(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPatchApplied",
                              "mira.workflow.patch-applied.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check =
            check_exact_keys(json.value(), kPatchAppliedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPatchAppliedEvent event;
    auto patch_id = parse_member_id<WorkflowPatchId>(json.value(), "patch_id");
    if (!patch_id.has_value()) {
        return patch_id.error();
    }
    event.patch_id = patch_id.value();
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    const auto *epoch = json.value().find("run_patch_epoch");
    if (!epoch->is_integer() || epoch->as_integer().value() < 0) {
        return event_error(ErrorCode::InvalidArgument,
                           "run_patch_epoch must be a non-negative integer");
    }
    event.run_patch_epoch = static_cast<std::uint32_t>(epoch->as_integer().value());
    return event;
}

Result<WorkflowPatchRejectedEvent> parse_workflow_patch_rejected(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPatchRejected",
                              "mira.workflow.patch-rejected.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check =
            check_exact_keys(json.value(), kPatchRejectedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPatchRejectedEvent event;
    auto patch_id = parse_member_id<WorkflowPatchId>(json.value(), "patch_id");
    if (!patch_id.has_value()) {
        return patch_id.error();
    }
    event.patch_id = patch_id.value();
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto reason = parse_member_name(json.value(), "reason_code");
    if (!reason.has_value()) {
        return reason.error();
    }
    if (reason.value().empty() || reason.value().size() > kMaxReasonCodeBytes) {
        return event_error(ErrorCode::InvalidArgument, "reason_code is not bounded");
    }
    event.reason_code = reason.value();
    return event;
}

Result<WorkflowPolicySwitchedEvent> parse_workflow_policy_switched(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPolicySwitched",
                              "mira.workflow.policy-switched.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kPolicySwitchedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPolicySwitchedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto from_name = parse_member_enum_name(json.value(), "from");
    if (!from_name.has_value()) {
        return from_name.error();
    }
    auto from_policy = parse_workflow_policy(from_name.value());
    if (!from_policy.has_value()) {
        return from_policy.error();
    }
    event.from = from_policy.value();
    auto to_name = parse_member_enum_name(json.value(), "to");
    if (!to_name.has_value()) {
        return to_name.error();
    }
    auto to_policy = parse_workflow_policy(to_name.value());
    if (!to_policy.has_value()) {
        return to_policy.error();
    }
    event.to = to_policy.value();
    return event;
}

Result<WorkflowDecisionRaisedEvent> parse_workflow_decision_raised(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowDecisionRaised",
                              "mira.workflow.decision-raised.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kDecisionRaisedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowDecisionRaisedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto decision_id = parse_member_id<WorkflowDecisionId>(json.value(), "decision_id");
    if (!decision_id.has_value()) {
        return decision_id.error();
    }
    event.decision_id = decision_id.value();
    auto digest = parse_member_digest(json.value(), "payload_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.payload_digest = digest.value();
    return event;
}

Result<WorkflowDecisionResolvedEvent>
parse_workflow_decision_resolved(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowDecisionResolved",
                              "mira.workflow.decision-resolved.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kDecisionResolvedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowDecisionResolvedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto decision_id = parse_member_id<WorkflowDecisionId>(json.value(), "decision_id");
    if (!decision_id.has_value()) {
        return decision_id.error();
    }
    event.decision_id = decision_id.value();
    auto resolution_name = parse_member_enum_name(json.value(), "resolution");
    if (!resolution_name.has_value()) {
        return resolution_name.error();
    }
    auto parsed = parse_workflow_decision_resolution(resolution_name.value());
    if (!parsed.has_value()) {
        return parsed.error();
    }
    event.resolution = parsed.value();
    return event;
}

// --- Workflow publish gate (stage D, DEC-025 §3) ----------------------------

EventPayload to_event_payload(const WorkflowPublishProposedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.publish-proposed.v1");
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("ir_digest", digest_text(event.ir_digest));
    object.emplace_back("source_run_id", event.source_run_id.has_value()
                                             ? JsonValue{event.source_run_id->to_string()}
                                             : JsonValue{nullptr});
    EventPayload payload;
    payload.type = "WorkflowPublishProposed";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowPublishAppliedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.publish-applied.v1");
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("ir_digest", digest_text(event.ir_digest));
    object.emplace_back("evidence", digest_text(event.evidence));
    object.emplace_back("dry_run_id", event.dry_run_id.to_string());
    EventPayload payload;
    payload.type = "WorkflowPublishApplied";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowPublishRejectedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.publish-rejected.v1");
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("ir_digest", digest_text(event.ir_digest));
    object.emplace_back("reason_code", event.reason_code);
    EventPayload payload;
    payload.type = "WorkflowPublishRejected";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

Result<WorkflowPublishProposedEvent> parse_workflow_publish_proposed(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPublishProposed",
                              "mira.workflow.publish-proposed.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kPublishProposedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPublishProposedEvent event;
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto digest = parse_member_digest(json.value(), "ir_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.ir_digest = digest.value();
    const auto *source = json.value().find("source_run_id");
    if (source != nullptr && source->is_string()) {
        auto run_id = WorkflowRunId::parse(*source->as_string());
        if (!run_id || run_id->is_nil()) {
            return event_error(ErrorCode::InvalidArgument,
                               "payload member 'source_run_id' is not a valid id");
        }
        event.source_run_id = *run_id;
    }
    return event;
}

Result<WorkflowPublishAppliedEvent> parse_workflow_publish_applied(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPublishApplied",
                              "mira.workflow.publish-applied.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kPublishAppliedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPublishAppliedEvent event;
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto digest = parse_member_digest(json.value(), "ir_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.ir_digest = digest.value();
    auto evidence = parse_member_digest(json.value(), "evidence");
    if (!evidence.has_value()) {
        return evidence.error();
    }
    event.evidence = evidence.value();
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "dry_run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.dry_run_id = run_id.value();
    return event;
}

Result<WorkflowPublishRejectedEvent> parse_workflow_publish_rejected(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowPublishRejected",
                              "mira.workflow.publish-rejected.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kPublishRejectedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowPublishRejectedEvent event;
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto digest = parse_member_digest(json.value(), "ir_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.ir_digest = digest.value();
    auto reason = parse_member_name(json.value(), "reason_code");
    if (!reason.has_value()) {
        return reason.error();
    }
    if (reason.value().empty() || reason.value().size() > kMaxReasonCodeBytes) {
        return event_error(ErrorCode::InvalidArgument, "reason_code is not bounded");
    }
    event.reason_code = reason.value();
    return event;
}

// --- Workflow navigation (stage E, DEC-028 §4) -------------------------------

EventPayload to_event_payload(const WorkflowNavigationPlannedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.navigation-planned.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("step_id", event.step_id.to_string());
    object.emplace_back("from_state", event.from_state);
    object.emplace_back("to_state", event.to_state);
    object.emplace_back("edge_count", static_cast<std::int64_t>(event.edge_count));
    object.emplace_back("plan_digest", digest_text(event.plan_digest));
    object.emplace_back("total_cost", event.total_cost);
    object.emplace_back("guards_blocked", static_cast<std::int64_t>(event.guards_blocked));
    object.emplace_back("guards_unevaluable",
                        static_cast<std::int64_t>(event.guards_unevaluable));
    EventPayload payload;
    payload.type = "WorkflowNavigationPlanned";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

EventPayload to_event_payload(const WorkflowNavigationObservedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.navigation-observed.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("step_id", event.step_id.to_string());
    object.emplace_back("transition_id", event.transition_id);
    object.emplace_back("from_state", event.from_state);
    object.emplace_back("to_state", event.to_state);
    object.emplace_back("success", event.success);
    object.emplace_back("confidence", event.confidence);
    EventPayload payload;
    payload.type = "WorkflowNavigationObserved";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

namespace {

[[nodiscard]] Result<std::string> parse_member_state_id(const JsonValue &json, const char *key) {
    auto name = parse_member_name(json, key);
    if (!name.has_value()) {
        return name;
    }
    if (name.value().empty() || name.value().size() > kMaxNavigationIdBytes) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' is not bounded");
    }
    return name;
}

[[nodiscard]] Result<std::uint64_t> parse_member_count(const JsonValue &json, const char *key) {
    const auto *member = json.find(key);
    if (member == nullptr || !member->is_integer()) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' must be an integer");
    }
    const auto value = member->as_integer().value();
    if (value < 0) {
        return event_error(ErrorCode::InvalidArgument,
                           std::string{"payload member '"} + key + "' must be non-negative");
    }
    return static_cast<std::uint64_t>(value);
}

} // namespace

Result<WorkflowNavigationPlannedEvent>
parse_workflow_navigation_planned(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowNavigationPlanned",
                              "mira.workflow.navigation-planned.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kNavigationPlannedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowNavigationPlannedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto step_id = parse_member_id<StepId>(json.value(), "step_id");
    if (!step_id.has_value()) {
        return step_id.error();
    }
    event.step_id = step_id.value();
    auto from_state = parse_member_state_id(json.value(), "from_state");
    if (!from_state.has_value()) {
        return from_state.error();
    }
    event.from_state = from_state.value();
    auto to_state = parse_member_state_id(json.value(), "to_state");
    if (!to_state.has_value()) {
        return to_state.error();
    }
    event.to_state = to_state.value();
    auto edge_count = parse_member_count(json.value(), "edge_count");
    if (!edge_count.has_value()) {
        return edge_count.error();
    }
    event.edge_count = edge_count.value();
    auto plan_digest = parse_member_digest(json.value(), "plan_digest");
    if (!plan_digest.has_value()) {
        return plan_digest.error();
    }
    event.plan_digest = plan_digest.value();
    const auto *total_cost = json.value().find("total_cost");
    if (total_cost == nullptr || !total_cost->is_number() ||
        !std::isfinite(total_cost->as_number().value()) ||
        total_cost->as_number().value() < 0.0) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'total_cost' must be a non-negative number");
    }
    event.total_cost = total_cost->as_number().value();
    auto guards_blocked = parse_member_count(json.value(), "guards_blocked");
    if (!guards_blocked.has_value()) {
        return guards_blocked.error();
    }
    event.guards_blocked = guards_blocked.value();
    auto guards_unevaluable = parse_member_count(json.value(), "guards_unevaluable");
    if (!guards_unevaluable.has_value()) {
        return guards_unevaluable.error();
    }
    event.guards_unevaluable = guards_unevaluable.value();
    return event;
}

Result<WorkflowNavigationObservedEvent>
parse_workflow_navigation_observed(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowNavigationObserved",
                              "mira.workflow.navigation-observed.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kNavigationObservedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowNavigationObservedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto step_id = parse_member_id<StepId>(json.value(), "step_id");
    if (!step_id.has_value()) {
        return step_id.error();
    }
    event.step_id = step_id.value();
    auto transition_id = parse_member_state_id(json.value(), "transition_id");
    if (!transition_id.has_value()) {
        return transition_id.error();
    }
    event.transition_id = transition_id.value();
    auto from_state = parse_member_state_id(json.value(), "from_state");
    if (!from_state.has_value()) {
        return from_state.error();
    }
    event.from_state = from_state.value();
    auto to_state = parse_member_state_id(json.value(), "to_state");
    if (!to_state.has_value()) {
        return to_state.error();
    }
    event.to_state = to_state.value();
    const auto *success = json.value().find("success");
    if (success == nullptr || !success->is_boolean()) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'success' must be a boolean");
    }
    event.success = *success->as_boolean();
    const auto *confidence = json.value().find("confidence");
    if (confidence == nullptr || !confidence->is_number() ||
        !std::isfinite(confidence->as_number().value()) ||
        confidence->as_number().value() < 0.0 || confidence->as_number().value() > 1.0) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'confidence' must be within [0,1]");
    }
    event.confidence = confidence->as_number().value();
    return event;
}

// --- Workflow learning audit (stage F, DEC-030 §5) ---------------------------

namespace {

[[nodiscard]] Result<std::string> parse_learning_outcome(const JsonValue &json) {
    const auto *outcome = json.find("outcome");
    if (outcome == nullptr || !outcome->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'outcome' must be a string");
    }
    const auto &text = *outcome->as_string();
    if (text != "recorded" && text != "failed") {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'outcome' is outside the closed set");
    }
    return text;
}

[[nodiscard]] Result<std::string> parse_learning_reason_code(const JsonValue &json) {
    const auto *reason = json.find("reason_code");
    if (reason == nullptr || !reason->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'reason_code' must be a string");
    }
    return *reason->as_string();
}

} // namespace

EventPayload to_event_payload(const WorkflowEpisodeRecordedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.episode-recorded.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("episode_digest", digest_text(event.episode_digest));
    object.emplace_back("outcome", event.outcome);
    object.emplace_back("reason_code", event.reason_code);
    EventPayload payload;
    payload.type = "WorkflowEpisodeRecorded";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

Result<WorkflowEpisodeRecordedEvent> parse_workflow_episode_recorded(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowEpisodeRecorded",
                              "mira.workflow.episode-recorded.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kEpisodeRecordedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowEpisodeRecordedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto digest = parse_member_digest(json.value(), "episode_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.episode_digest = digest.value();
    auto outcome = parse_learning_outcome(json.value());
    if (!outcome.has_value()) {
        return outcome.error();
    }
    event.outcome = outcome.value();
    auto reason = parse_learning_reason_code(json.value());
    if (!reason.has_value()) {
        return reason.error();
    }
    event.reason_code = reason.value();
    return event;
}

EventPayload to_event_payload(const WorkflowLessonRecordedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.lesson-recorded.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("lesson_digest", digest_text(event.lesson_digest));
    object.emplace_back("outcome", event.outcome);
    object.emplace_back("reason_code", event.reason_code);
    EventPayload payload;
    payload.type = "WorkflowLessonRecorded";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

Result<WorkflowLessonRecordedEvent> parse_workflow_lesson_recorded(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowLessonRecorded",
                              "mira.workflow.lesson-recorded.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kLessonRecordedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowLessonRecordedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto digest = parse_member_digest(json.value(), "lesson_digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    event.lesson_digest = digest.value();
    auto outcome = parse_learning_outcome(json.value());
    if (!outcome.has_value()) {
        return outcome.error();
    }
    event.outcome = outcome.value();
    auto reason = parse_learning_reason_code(json.value());
    if (!reason.has_value()) {
        return reason.error();
    }
    event.reason_code = reason.value();
    return event;
}

std::string workflow_recovery_outcome_name(WorkflowRecoveryOutcome outcome) {
    switch (outcome) {
    case WorkflowRecoveryOutcome::PatchedAndResumed:
        return "patched-and-resumed";
    case WorkflowRecoveryOutcome::ResumedWithoutPatch:
        return "resumed-without-patch";
    case WorkflowRecoveryOutcome::CancelRequested:
        return "cancel-requested";
    case WorkflowRecoveryOutcome::DeferredToHost:
        return "deferred-to-host";
    case WorkflowRecoveryOutcome::Aborted:
        return "aborted";
    }
    return "aborted";
}

Result<WorkflowRecoveryOutcome> parse_workflow_recovery_outcome(std::string_view name) {
    if (name == "patched-and-resumed") {
        return WorkflowRecoveryOutcome::PatchedAndResumed;
    }
    if (name == "resumed-without-patch") {
        return WorkflowRecoveryOutcome::ResumedWithoutPatch;
    }
    if (name == "cancel-requested") {
        return WorkflowRecoveryOutcome::CancelRequested;
    }
    if (name == "deferred-to-host") {
        return WorkflowRecoveryOutcome::DeferredToHost;
    }
    if (name == "aborted") {
        return WorkflowRecoveryOutcome::Aborted;
    }
    return event_error(ErrorCode::InvalidArgument, "unknown workflow recovery outcome");
}

EventPayload to_event_payload(const WorkflowRecoveryAttemptedEvent &event) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.workflow.recovery-attempted.v1");
    object.emplace_back("run_id", event.run_id.to_string());
    object.emplace_back("workflow_id", event.workflow_id.to_string());
    object.emplace_back("ordinal", static_cast<std::int64_t>(event.ordinal));
    object.emplace_back("task_id", event.task_id.to_string());
    object.emplace_back("outcome", workflow_recovery_outcome_name(event.outcome));
    object.emplace_back("reason_code", event.reason_code);
    object.emplace_back("decision_digest", event.decision_digest.has_value()
                                                ? JsonValue{digest_text(*event.decision_digest)}
                                                : JsonValue{nullptr});
    object.emplace_back("patch_id", event.patch_id.has_value()
                                        ? JsonValue{event.patch_id->to_string()}
                                        : JsonValue{nullptr});
    object.emplace_back("model_request_id", event.model_request_id.has_value()
                                                    ? JsonValue{event.model_request_id->to_string()}
                                                    : JsonValue{nullptr});
    object.emplace_back("lessons_offered", static_cast<std::int64_t>(event.lessons_offered));
    object.emplace_back("lessons_stale", static_cast<std::int64_t>(event.lessons_stale));
    object.emplace_back("lessons_unparseable",
                        static_cast<std::int64_t>(event.lessons_unparseable));
    object.emplace_back("lessons_kept", static_cast<std::int64_t>(event.lessons_kept));
    EventPayload payload;
    payload.type = "WorkflowRecoveryAttempted";
    payload.data = to_json_string(JsonValue{std::move(object)});
    payload.classification = EventClass::State;
    return payload;
}

Result<WorkflowRecoveryAttemptedEvent>
parse_workflow_recovery_attempted(const EventPayload &payload) {
    auto json = parse_payload(payload, "WorkflowRecoveryAttempted",
                              "mira.workflow.recovery-attempted.v1");
    if (!json.has_value()) {
        return json.error();
    }
    if (auto check = check_exact_keys(json.value(), kRecoveryAttemptedKeys);
        !check.has_value()) {
        return check.error();
    }
    WorkflowRecoveryAttemptedEvent event;
    auto run_id = parse_member_id<WorkflowRunId>(json.value(), "run_id");
    if (!run_id.has_value()) {
        return run_id.error();
    }
    event.run_id = run_id.value();
    auto workflow_id = parse_member_id<WorkflowId>(json.value(), "workflow_id");
    if (!workflow_id.has_value()) {
        return workflow_id.error();
    }
    event.workflow_id = workflow_id.value();
    auto ordinal = parse_member_count(json.value(), "ordinal");
    if (!ordinal.has_value()) {
        return ordinal.error();
    }
    if (ordinal.value() > std::numeric_limits<std::uint32_t>::max()) {
        return event_error(ErrorCode::InvalidArgument, "payload member 'ordinal' overflows");
    }
    event.ordinal = static_cast<std::uint32_t>(ordinal.value());
    auto task_id = parse_member_id<TaskId>(json.value(), "task_id");
    if (!task_id.has_value()) {
        return task_id.error();
    }
    event.task_id = task_id.value();
    const auto *outcome_name = json.value().find("outcome");
    if (outcome_name == nullptr || !outcome_name->is_string()) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'outcome' must be a string");
    }
    auto outcome = parse_workflow_recovery_outcome(*outcome_name->as_string());
    if (!outcome.has_value()) {
        return outcome.error();
    }
    event.outcome = outcome.value();
    auto reason = parse_learning_reason_code(json.value());
    if (!reason.has_value()) {
        return reason.error();
    }
    event.reason_code = reason.value();
    const auto *decision_digest = json.value().find("decision_digest");
    if (decision_digest == nullptr ||
        !(decision_digest->is_null() || decision_digest->is_string())) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'decision_digest' must be a digest or null");
    }
    if (decision_digest->is_string()) {
        auto digest = digest_from_hex(*decision_digest->as_string());
        if (!digest) {
            return event_error(ErrorCode::InvalidArgument,
                               "payload member 'decision_digest' is not a valid digest");
        }
        event.decision_digest = *digest;
    }
    const auto *patch_id = json.value().find("patch_id");
    if (patch_id == nullptr || !(patch_id->is_null() || patch_id->is_string())) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'patch_id' must be an id or null");
    }
    if (patch_id->is_string()) {
        auto parsed = WorkflowPatchId::parse(*patch_id->as_string());
        if (!parsed.has_value()) {
            return event_error(ErrorCode::InvalidArgument,
                               "payload member 'patch_id' is not a valid id");
        }
        event.patch_id = *parsed;
    }
    const auto *model_request_id = json.value().find("model_request_id");
    if (model_request_id == nullptr ||
        !(model_request_id->is_null() || model_request_id->is_string())) {
        return event_error(ErrorCode::InvalidArgument,
                           "payload member 'model_request_id' must be an id or null");
    }
    if (model_request_id->is_string()) {
        auto parsed = ModelRequestId::parse(*model_request_id->as_string());
        if (!parsed.has_value()) {
            return event_error(ErrorCode::InvalidArgument,
                               "payload member 'model_request_id' is not a valid id");
        }
        event.model_request_id = *parsed;
    }
    for (const auto *counter : {"lessons_offered", "lessons_stale", "lessons_unparseable",
                                "lessons_kept"}) {
        auto value = parse_member_count(json.value(), counter);
        if (!value.has_value()) {
            return value.error();
        }
        if (value.value() > std::numeric_limits<std::uint32_t>::max()) {
            return event_error(ErrorCode::InvalidArgument,
                               std::string{"payload member '"} + counter + "' overflows");
        }
        const auto bounded = static_cast<std::uint32_t>(value.value());
        if (std::string_view{counter} == "lessons_offered") {
            event.lessons_offered = bounded;
        } else if (std::string_view{counter} == "lessons_stale") {
            event.lessons_stale = bounded;
        } else if (std::string_view{counter} == "lessons_unparseable") {
            event.lessons_unparseable = bounded;
        } else {
            event.lessons_kept = bounded;
        }
    }
    return event;
}

} // namespace mira
