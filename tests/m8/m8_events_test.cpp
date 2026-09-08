#include "../support/test.hpp"

#include <mira/event_store.hpp>
#include <mira/replay.hpp>
#include <mira/workflow_events.hpp>

#include <string>

namespace {

using namespace mira;

int run_and_step_payloads_round_trip() {
    WorkflowRunStartedEvent started;
    started.run_id = WorkflowRunId::generate();
    started.workflow_id = WorkflowId::generate();
    started.ir_digest = digest_string("ir");
    started.parameters_digest = digest_string("parameters");
    started.policy = WorkflowPolicy::Recoverable;
    const EventPayload started_payload = to_event_payload(started);
    MIRA_CHECK(started_payload.type == "WorkflowRunStarted");
    MIRA_CHECK(started_payload.classification == EventClass::Critical);
    auto parsed_started = parse_workflow_run_started(started_payload);
    MIRA_CHECK(parsed_started.has_value());
    MIRA_CHECK(parsed_started.value().run_id == started.run_id);
    MIRA_CHECK(parsed_started.value().ir_digest == started.ir_digest);
    MIRA_CHECK(parsed_started.value().policy == WorkflowPolicy::Recoverable);

    WorkflowStepStartedEvent step_started;
    step_started.run_id = started.run_id;
    step_started.step_id = StepId::generate();
    step_started.kind = WorkflowStepKind::ToolCall;
    step_started.attempt = 2;
    auto parsed_step = parse_workflow_step_started(to_event_payload(step_started));
    MIRA_CHECK(parsed_step.has_value());
    MIRA_CHECK(parsed_step.value().step_id == step_started.step_id);
    MIRA_CHECK(parsed_step.value().attempt == 2);

    WorkflowStepSettledEvent settled;
    settled.run_id = started.run_id;
    settled.step_id = step_started.step_id;
    settled.disposition = WorkflowStepDisposition::Skipped;
    settled.verification = "none";
    settled.safe_summary = "precondition unsatisfied";
    auto parsed_settled = parse_workflow_step_settled(to_event_payload(settled));
    MIRA_CHECK(parsed_settled.has_value());
    MIRA_CHECK(parsed_settled.value().disposition == WorkflowStepDisposition::Skipped);

    WorkflowRunSettledEvent run_settled;
    run_settled.run_id = started.run_id;
    run_settled.terminal_state = WorkflowRunState::Completed;
    run_settled.run_epoch = 5;
    run_settled.safe_summary = "all steps verified";
    auto parsed_run = parse_workflow_run_settled(to_event_payload(run_settled));
    MIRA_CHECK(parsed_run.has_value());
    MIRA_CHECK(parsed_run.value().terminal_state == WorkflowRunState::Completed);
    MIRA_CHECK(parsed_run.value().run_epoch == 5);
    return 0;
}

int patch_policy_and_decision_payloads_round_trip() {
    WorkflowPatchProposedEvent proposed;
    proposed.patch_id = WorkflowPatchId::generate();
    proposed.run_id = WorkflowRunId::generate();
    proposed.patch_digest = digest_string("patch");
    proposed.target = WorkflowPatchTarget::RunParameters;
    proposed.reason_code = "user-conversation";
    auto parsed_proposed = parse_workflow_patch_proposed(to_event_payload(proposed));
    MIRA_CHECK(parsed_proposed.has_value());
    MIRA_CHECK(parsed_proposed.value().target == WorkflowPatchTarget::RunParameters);

    WorkflowPatchAppliedEvent applied;
    applied.patch_id = proposed.patch_id;
    applied.run_id = proposed.run_id;
    applied.run_patch_epoch = 3;
    auto parsed_applied = parse_workflow_patch_applied(to_event_payload(applied));
    MIRA_CHECK(parsed_applied.has_value());
    MIRA_CHECK(parsed_applied.value().run_patch_epoch == 3);

    WorkflowPatchRejectedEvent rejected;
    rejected.patch_id = proposed.patch_id;
    rejected.run_id = proposed.run_id;
    rejected.reason_code = "id-conflict";
    auto parsed_rejected = parse_workflow_patch_rejected(to_event_payload(rejected));
    MIRA_CHECK(parsed_rejected.has_value());
    MIRA_CHECK(parsed_rejected.value().reason_code == "id-conflict");

    WorkflowPolicySwitchedEvent switched;
    switched.run_id = proposed.run_id;
    switched.from = WorkflowPolicy::Strict;
    switched.to = WorkflowPolicy::DryRun;
    auto parsed_switched = parse_workflow_policy_switched(to_event_payload(switched));
    MIRA_CHECK(parsed_switched.has_value());
    MIRA_CHECK(parsed_switched.value().to == WorkflowPolicy::DryRun);

    WorkflowDecisionRaisedEvent raised;
    raised.run_id = proposed.run_id;
    raised.decision_id = WorkflowDecisionId::generate();
    raised.payload_digest = digest_string("decision");
    auto parsed_raised = parse_workflow_decision_raised(to_event_payload(raised));
    MIRA_CHECK(parsed_raised.has_value());
    MIRA_CHECK(parsed_raised.value().decision_id == raised.decision_id);

    WorkflowDecisionResolvedEvent resolved;
    resolved.run_id = proposed.run_id;
    resolved.decision_id = raised.decision_id;
    resolved.resolution = WorkflowDecisionResolution::Accept;
    auto parsed_resolved = parse_workflow_decision_resolved(to_event_payload(resolved));
    MIRA_CHECK(parsed_resolved.has_value());
    MIRA_CHECK(parsed_resolved.value().resolution == WorkflowDecisionResolution::Accept);
    return 0;
}

int payloads_fail_closed() {
    WorkflowRunStartedEvent started;
    started.run_id = WorkflowRunId::generate();
    started.workflow_id = WorkflowId::generate();
    started.ir_digest = digest_string("ir");
    started.parameters_digest = digest_string("parameters");
    started.policy = WorkflowPolicy::Strict;

    // Unknown field inside the payload data.
    EventPayload with_unknown = to_event_payload(started);
    auto json = parse_json(with_unknown.data);
    MIRA_CHECK(json.has_value());
    json.value().set("future", JsonValue{static_cast<std::int64_t>(1)});
    with_unknown.data = to_json_string(json.value());
    MIRA_CHECK(!parse_workflow_run_started(with_unknown).has_value());

    // Wrong schema string.
    EventPayload wrong_schema = to_event_payload(started);
    auto schema_json = parse_json(wrong_schema.data);
    MIRA_CHECK(schema_json.has_value());
    schema_json.value().set("schema", JsonValue{std::string("mira.workflow.run-started.v2")});
    wrong_schema.data = to_json_string(schema_json.value());
    auto mismatched = parse_workflow_run_started(wrong_schema);
    MIRA_CHECK(!mismatched.has_value());
    MIRA_CHECK(mismatched.error().code == ErrorCode::UnsupportedVersion);

    // Wrong event type.
    EventPayload wrong_type = to_event_payload(started);
    wrong_type.type = "WorkflowStepStarted";
    MIRA_CHECK(!parse_workflow_run_started(wrong_type).has_value());

    // Malformed id.
    EventPayload bad_id = to_event_payload(started);
    auto id_json = parse_json(bad_id.data);
    MIRA_CHECK(id_json.has_value());
    id_json.value().set("run_id", JsonValue{std::string("not-hex")});
    bad_id.data = to_json_string(id_json.value());
    MIRA_CHECK(!parse_workflow_run_started(bad_id).has_value());

    // Unparsable data.
    EventPayload garbage;
    garbage.type = "WorkflowRunStarted";
    garbage.data = "{not json";
    MIRA_CHECK(!parse_workflow_run_started(garbage).has_value());

    // Non-terminal run settled states are rejected.
    WorkflowRunSettledEvent not_terminal;
    not_terminal.run_id = started.run_id;
    not_terminal.terminal_state = WorkflowRunState::Running;
    MIRA_CHECK(!parse_workflow_run_settled(to_event_payload(not_terminal)).has_value());

    // Reason codes are bounded and required.
    WorkflowPatchRejectedEvent no_reason;
    no_reason.patch_id = WorkflowPatchId::generate();
    no_reason.run_id = started.run_id;
    no_reason.reason_code = std::string(512, 'x');
    MIRA_CHECK(!parse_workflow_patch_rejected(to_event_payload(no_reason)).has_value());

    MIRA_CHECK(is_workflow_event_type("WorkflowPatchProposed"));
    MIRA_CHECK(is_workflow_event_type("WorkflowDecisionResolved"));
    MIRA_CHECK(!is_workflow_event_type("LoopSettled"));
    MIRA_CHECK(!is_workflow_event_type(""));
    return 0;
}

int payloads_never_inline_user_content() {
    // DEC-022 §5: payload builders carry ids, digests, enums and codes only.
    // The serialized run-started payload must not contain any parameter text
    // even though the caller holds it; parameters travel as a digest.
    WorkflowRunStartedEvent started;
    started.run_id = WorkflowRunId::generate();
    started.workflow_id = WorkflowId::generate();
    started.ir_digest = digest_string("ir");
    started.parameters_digest = digest_string("parameters");
    started.policy = WorkflowPolicy::Strict;
    const std::string data = to_event_payload(started).data;
    MIRA_CHECK(data.find("zhang san") == std::string::npos);
    MIRA_CHECK(data.find("contact") == std::string::npos);
    MIRA_CHECK(data.find(started.parameters_digest.to_string()) != std::string::npos);
    return 0;
}

int offline_replay_of_workflow_events_has_no_side_effects() {
    // W-08 contract assertion: workflow events rebuild projections from the
    // event store and the replay environment never dispatches real input.
    MemoryEventStore store;

    const WorkflowRunId run_id = WorkflowRunId::generate();
    const WorkflowId workflow_id = WorkflowId::generate();
    const Sha256Digest ir_digest = digest_string("ir");

    WorkflowRunStartedEvent started;
    started.run_id = run_id;
    started.workflow_id = workflow_id;
    started.ir_digest = ir_digest;
    started.parameters_digest = digest_string("parameters");
    started.policy = WorkflowPolicy::DryRun;

    WorkflowStepStartedEvent step_started;
    step_started.run_id = run_id;
    step_started.step_id = StepId::generate();
    step_started.kind = WorkflowStepKind::ToolCall;
    step_started.attempt = 1;

    WorkflowStepSettledEvent step_settled;
    step_settled.run_id = run_id;
    step_settled.step_id = step_started.step_id;
    step_settled.disposition = WorkflowStepDisposition::Completed;
    step_settled.verification = "satisfied";
    step_settled.safe_summary = "ok";

    WorkflowRunSettledEvent run_settled;
    run_settled.run_id = run_id;
    run_settled.terminal_state = WorkflowRunState::Completed;
    run_settled.run_epoch = 2;
    run_settled.safe_summary = "dry run complete";

    const SessionId session = SessionId::generate();
    std::size_t sequence = 0;
    for (const auto &payload :
         {to_event_payload(started), to_event_payload(step_started),
          to_event_payload(step_settled), to_event_payload(run_settled)}) {
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = RuntimeId::generate();
        request.session_id = session;
        request.payload = payload;
        request.cause = sequence++;
        auto receipt = store.append(request);
        MIRA_CHECK(receipt.has_value());
    }

    // Rebuild the run projection purely from recorded events.
    EventQuery query;
    query.session_id = session;
    auto page = store.read(query);
    MIRA_CHECK(page.has_value());
    MIRA_CHECK(page.value().events.size() == 4);
    int started_count = 0;
    int settled_count = 0;
    for (const auto &envelope : page.value().events) {
        MIRA_CHECK(is_workflow_event_type(envelope.payload.type));
        if (envelope.payload.type == "WorkflowRunStarted") {
            auto event = parse_workflow_run_started(envelope.payload);
            MIRA_CHECK(event.has_value());
            MIRA_CHECK(event.value().policy == WorkflowPolicy::DryRun);
            MIRA_CHECK(event.value().ir_digest == ir_digest);
            started_count += 1;
        }
        if (envelope.payload.type == "WorkflowRunSettled") {
            auto event = parse_workflow_run_settled(envelope.payload);
            MIRA_CHECK(event.has_value());
            settled_count += 1;
        }
    }
    MIRA_CHECK(started_count == 1);
    MIRA_CHECK(settled_count == 1);

    // The replay environment holds no receipts for workflow input: replaying
    // a dispatch attempt settles as uncertain instead of executing (W-08).
    OfflineReplayEnvironment replay{{}, {}, EnvironmentCapabilities{}};
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    InputSequence sequence_input;
    sequence_input.events.push_back(InputEvent{.kind = "tap", .payload = "0.5,0.5"});
    const auto dispatch = replay.execute(sequence_input, context);
    MIRA_CHECK(!dispatch.has_value());
    MIRA_CHECK(dispatch.error().code == ErrorCode::ExecutionUncertain);
    MIRA_CHECK(!replay.interrupted());
    return 0;
}

} // namespace

int main() {
    if (run_and_step_payloads_round_trip() != 0) {
        return 1;
    }
    if (patch_policy_and_decision_payloads_round_trip() != 0) {
        return 1;
    }
    if (payloads_fail_closed() != 0) {
        return 1;
    }
    if (payloads_never_inline_user_content() != 0) {
        return 1;
    }
    if (offline_replay_of_workflow_events_has_no_side_effects() != 0) {
        return 1;
    }
    return 0;
}
