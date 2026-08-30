#include <mira/agent_loop.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

[[nodiscard]] std::string format_coordinate(double value) {
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return length > 0 ? std::string(buffer, static_cast<std::size_t>(length)) : std::string("0");
}

[[nodiscard]] std::optional<double> decision_number(const JsonValue &decision, const char *key) {
    const auto *field = decision.find(key);
    if (field == nullptr || !field->is_number()) {
        return std::nullopt;
    }
    return field->as_number();
}

[[nodiscard]] std::optional<std::string> decision_string(const JsonValue &decision,
                                                         const char *key) {
    const auto *field = decision.find(key);
    if (field == nullptr || !field->is_string()) {
        return std::nullopt;
    }
    return *field->as_string();
}

[[nodiscard]] bool canonical_range(double value) {
    return value >= 0.0 && value <= 1.0;
}

[[nodiscard]] Error loop_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.agent_loop";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] bool recoverable_model_failure(const Error &error) {
    if (!error.retryable) {
        return false;
    }
    if (error.domain != "mira.model") {
        return true;
    }
    // Rate limits and overload are recoverable within the step budget;
    // permission, policy and request-shape failures are not.
    return error.domain_code == static_cast<std::int32_t>(ModelDomainCode::RateLimited) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::TransportFailed);
}

[[nodiscard]] std::string summarize_action(const InputSequence &sequence) {
    if (sequence.events.empty()) {
        return "none";
    }
    std::string summary;
    for (std::size_t index = 0; index < sequence.events.size(); ++index) {
        if (index > 0) {
            summary += ";";
        }
        summary += sequence.events[index].kind;
    }
    return summary;
}

} // namespace

JsonSchema agent_decision_schema() {
    auto parsed = parse_json(R"json({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["tap", "long_press", "swipe", "type", "back", "home", "done", "fail"]
            },
            "x": {"type": "number", "minimum": 0, "maximum": 1},
            "y": {"type": "number", "minimum": 0, "maximum": 1},
            "end_x": {"type": "number", "minimum": 0, "maximum": 1},
            "end_y": {"type": "number", "minimum": 0, "maximum": 1},
            "text": {"type": "string", "maxLength": 4096},
            "reason": {"type": "string", "maxLength": 2048}
        },
        "required": ["action", "reason"],
        "additionalProperties": false
    })json");
    return JsonSchema{std::move(parsed).value()};
}

std::string loop_outcome_name(LoopOutcome outcome) {
    switch (outcome) {
    case LoopOutcome::Completed:
        return "Completed";
    case LoopOutcome::Failed:
        return "Failed";
    case LoopOutcome::Cancelled:
        return "Cancelled";
    case LoopOutcome::MaxSteps:
        return "MaxSteps";
    }
    return "Failed";
}

ModelDoneVerifier::Verdict ModelDoneVerifier::verify(const Observation &,
                                                     const DecisionCandidate &decision) {
    const auto action = decision_string(decision.value, "action");
    if (!action.has_value()) {
        return Verdict::Invalid;
    }
    return *action == "done" ? Verdict::Satisfied : Verdict::NotSatisfied;
}

Result<InputSequence> compile_discrete_action(const JsonValue &decision) {
    const auto action = decision_string(decision, "action");
    if (!action.has_value()) {
        return loop_error(ErrorCode::InvalidModelOutput, "decision carries no action");
    }
    InputSequence sequence;
    InputEvent event;
    event.kind = *action;
    if (*action == "tap" || *action == "long_press") {
        const auto x = decision_number(decision, "x");
        const auto y = decision_number(decision, "y");
        if (!x.has_value() || !y.has_value() || !canonical_range(*x) || !canonical_range(*y)) {
            return loop_error(ErrorCode::InvalidModelOutput,
                              "point action requires canonical x and y coordinates");
        }
        event.payload = format_coordinate(*x) + "," + format_coordinate(*y);
    } else if (*action == "swipe") {
        const auto x = decision_number(decision, "x");
        const auto y = decision_number(decision, "y");
        const auto end_x = decision_number(decision, "end_x");
        const auto end_y = decision_number(decision, "end_y");
        if (!x.has_value() || !y.has_value() || !end_x.has_value() || !end_y.has_value() ||
            !canonical_range(*x) || !canonical_range(*y) || !canonical_range(*end_x) ||
            !canonical_range(*end_y)) {
            return loop_error(ErrorCode::InvalidModelOutput,
                              "swipe requires four canonical coordinates");
        }
        event.payload = format_coordinate(*x) + "," + format_coordinate(*y) + "," +
                        format_coordinate(*end_x) + "," + format_coordinate(*end_y);
    } else if (*action == "type") {
        const auto text = decision_string(decision, "text");
        if (!text.has_value() || text->empty()) {
            return loop_error(ErrorCode::InvalidModelOutput, "type action requires text");
        }
        event.payload = *text;
    } else if (*action == "back" || *action == "home") {
        event.payload.clear();
    } else if (*action == "done" || *action == "fail") {
        sequence.events.clear();
        return sequence; // Terminal intents compile to no input.
    } else {
        return loop_error(ErrorCode::InvalidModelOutput, "decision action is unknown");
    }
    sequence.events.push_back(std::move(event));
    return sequence;
}

AgentLoop::AgentLoop(std::shared_ptr<IEnvironment> environment, ModelGateway &gateway,
                     AgentLoopConfig config)
    : environment_(std::move(environment)), gateway_(gateway), config_(config) {}

void AgentLoop::set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                                SessionId session) {
    events_ = std::move(events);
    runtime_ = runtime;
    session_ = session;
}

void AgentLoop::emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
                     EventClass classification) const {
    if (events_ == nullptr) {
        return;
    }
    JsonValue::Object envelope;
    envelope.emplace_back("task_id", spec.task_id.to_string());
    envelope.emplace_back("task_epoch", static_cast<std::int64_t>(spec.task_epoch));
    envelope.emplace_back("detail", std::move(summary));
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_;
    append.session_id = session_;
    append.task_id = spec.task_id;
    append.payload = EventPayload{std::move(type), to_json_string(JsonValue(std::move(envelope))),
                                  classification};
    (void)events_->append(append);
}

Result<Observation> AgentLoop::observe_once(const AgentLoopSpec & /*spec*/,
                                            const OperationContext &context,
                                            ObservationMode mode) {
    ObservationRequest request;
    request.mode = mode;
    if (mode == ObservationMode::Full) {
        request.required.screen = true;
    }
    request.max_age = config_.observation_max_age;
    OperationContext observe_context = context;
    observe_context.operation = OperationId::generate();
    return environment_->observe(request, observe_context);
}

Result<ModelRequest> AgentLoop::build_request(const AgentLoopSpec &spec,
                                              const Observation &observation,
                                              const std::string &extra_instruction) {
    const auto schema = agent_decision_schema();
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = spec.task_id;
    request.task_epoch = spec.task_epoch;
    request.profile_id = spec.profile_id;

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = "mira.agent-loop.system.v1";
    system_item.authority = Sensitivity::Internal;
    TextPart system_text;
    system_text.text =
        "You are Mira, a device agent. Decide exactly one discrete action per "
        "turn as JSON matching the decision schema. Use canonical coordinates in "
        "[0,1]. Return action \"done\" only when the goal is achieved or \"fail\" "
        "when it cannot be achieved.";
    system_text.sensitivity = Sensitivity::Internal;
    system_item.content.emplace_back(std::move(system_text));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = "mira.agent-loop.context.v1";
    user_item.authority = Sensitivity::Internal;
    TextPart goal_text;
    goal_text.text = "Goal: " + spec.goal;
    goal_text.sensitivity = Sensitivity::Internal;
    user_item.content.emplace_back(std::move(goal_text));

    if (observation.screen.has_value()) {
        const auto &screen = observation.screen->value;
        ArtifactRef reference;
        reference.id = screen.payload_artifact;
        reference.media_type = "application/octet-stream";
        reference.sensitivity = Sensitivity::Internal;
        reference.byte_size = static_cast<std::uint64_t>(screen.width_pixels) *
                              static_cast<std::uint64_t>(screen.height_pixels) * 4ULL;
        ImagePart image;
        image.source = reference;
        image.detail = ImageDetail::Low;
        image.media_type = reference.media_type;
        user_item.content.emplace_back(std::move(image));
    }

    if (observation.structure.has_value()) {
        // UI text is untrusted observation data, never authority.
        std::string summary;
        for (const auto &node : observation.structure->value.nodes) {
            if (node.text.empty()) {
                continue;
            }
            if (summary.size() > 2048) {
                summary += " [truncated]";
                break;
            }
            if (!summary.empty()) {
                summary += " | ";
            }
            summary += node.text;
        }
        if (!summary.empty()) {
            TextPart ui_text;
            ui_text.text = "Observed UI text: " + summary;
            ui_text.sensitivity = Sensitivity::Internal;
            user_item.content.emplace_back(std::move(ui_text));
        }
    }

    if (!extra_instruction.empty()) {
        TextPart extra;
        extra.text = extra_instruction;
        extra.sensitivity = Sensitivity::Internal;
        user_item.content.emplace_back(std::move(extra));
    }

    request.input = {std::move(system_item), std::move(user_item)};
    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id =
        SchemaId::parse("6d6972612d6465636973696f6e2d7631").value_or(SchemaId{});
    request.output_contract.schema_version = SemanticVersion{1, 0, 0};
    request.output_contract.schema = schema;
    request.output_contract.canonical_schema_digest = canonical_json_digest(schema.root);

    // Per-step generation bound plus the whole-loop envelope: the gateway
    // ledger accumulates per task, so the request budget must describe the
    // remaining loop capacity, not a single step.
    request.generation.max_output_tokens = 512;
    request.budget.max_output_tokens =
        512ULL * (static_cast<std::uint64_t>(config_.max_steps) +
                  config_.max_recoveries_per_step + 1);
    request.budget.max_requests = config_.max_steps + config_.max_recoveries_per_step + 1;
    request.data_policy.store = false;
    request.prompt_provenance.system_template_digest =
        digest_string("mira.agent-loop.system.v1");
    request.prompt_provenance.decision_schema_digest =
        request.output_contract.canonical_schema_digest;
    return request;
}

Result<AgentLoopResult> AgentLoop::run(const AgentLoopSpec &spec, const OperationContext &context,
                                       ILoopVerifier &verifier) {
    AgentLoopResult result;
    if (spec.goal.empty()) {
        return loop_error(ErrorCode::InvalidArgument, "loop goal must not be empty");
    }
    // Exhausting the step budget is the default terminal; every early exit
    // assigns a specific outcome before leaving the loop.
    result.outcome = LoopOutcome::MaxSteps;

    RepairBudget repair_budget{};
    std::string feedback;

    for (std::uint32_t step = 1; step <= config_.max_steps; ++step) {
        if (context.cancelled()) {
            result.outcome = LoopOutcome::Cancelled;
            result.safe_summary = "cancellation requested";
            emit(spec, "LoopSettled",
                 JsonValue::Object{{"outcome", loop_outcome_name(result.outcome)}},
                 EventClass::State);
            return result;
        }

        LoopStepRecord record;
        record.step = step;

        // --- Observe -----------------------------------------------------
        auto observation = observe_once(spec, context, ObservationMode::Full);
        if (!observation) {
            if (observation.error().retryable &&
                result.recoveries < config_.max_recoveries_per_step) {
                ++result.recoveries;
                record.phase = StepPhase::Recovering;
                record.note = "observation failed; recovering";
                result.steps.push_back(std::move(record));
                continue;
            }
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "observation failed: " + observation.error().safe_message;
            break;
        }
        record.observation = observation.value().id;

        // --- Reason / Plan -----------------------------------------------
        auto request = build_request(spec, observation.value(), feedback);
        if (!request) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "request assembly failed";
            break;
        }
        feedback.clear();
        record.model_request = request.value().request_id;

        OperationContext model_context = context;
        model_context.operation = request.value().operation_id;
        model_context.deadline =
            context.deadline.has_value()
                ? std::min(context.deadline.value(),
                           std::chrono::steady_clock::now() + config_.model_call_deadline)
                : std::chrono::steady_clock::now() + config_.model_call_deadline;

        auto call = gateway_.infer(request.value(), model_context);
        if (!call) {
            if (call.error().code == ErrorCode::Cancelled) {
                result.outcome = LoopOutcome::Cancelled;
                result.safe_summary = "model call was cancelled";
                break;
            }
            if (recoverable_model_failure(call.error()) &&
                result.recoveries < config_.max_recoveries_per_step) {
                ++result.recoveries;
                record.phase = StepPhase::Recovering;
                record.note = "model failure; recovering: " + call.error().safe_message;
                result.steps.push_back(std::move(record));
                continue;
            }
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "model call failed: " + call.error().safe_message;
            break;
        }
        auto outcome = std::move(call).value();
        if (!outcome.admitted) {
            result.outcome = LoopOutcome::Cancelled;
            result.safe_summary = outcome.rejection_reason;
            break;
        }
        record.phase = StepPhase::Reasoned;

        // --- Bounded schema repair ---------------------------------------
        const RepairPolicy repair_policy{1, 2048};
        if (outcome.parse.outcome == DecisionParseOutcome::Malformed &&
            !repair_budget.exhausted(repair_policy)) {
            auto repair =
                build_schema_repair_request(request.value(), outcome.parse, repair_policy, repair_budget);
            if (repair) {
                repair_budget.attempts_used += 1;
                result.repairs += 1;
                OperationContext repair_context = model_context;
                repair_context.operation = repair.value().operation_id;
                auto repair_call = gateway_.infer(repair.value(), repair_context);
                if (repair_call && repair_call.value().admitted &&
                    repair_call.value().parse.outcome == DecisionParseOutcome::Decision) {
                    outcome = std::move(repair_call).value();
                }
            }
        }

        const auto parse_outcome = outcome.parse.outcome;
        if (parse_outcome == DecisionParseOutcome::Refused ||
            parse_outcome == DecisionParseOutcome::ContentFiltered) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "model refused or was filtered; no policy bypass";
            result.steps.push_back(std::move(record));
            break;
        }
        if (parse_outcome == DecisionParseOutcome::Ambiguous ||
            parse_outcome == DecisionParseOutcome::NoExecutableOutput ||
            parse_outcome == DecisionParseOutcome::ToolProposals) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary =
                parse_outcome == DecisionParseOutcome::ToolProposals
                    ? "tool proposals are not executable in the M3 loop"
                    : "model output carried no usable decision";
            result.steps.push_back(std::move(record));
            break;
        }
        if (parse_outcome != DecisionParseOutcome::Decision) {
            // Incomplete or still-malformed output: recover with feedback.
            if (result.recoveries < config_.max_recoveries_per_step) {
                ++result.recoveries;
                record.phase = StepPhase::Recovering;
                record.note = "incomplete or malformed decision; recovering";
                result.steps.push_back(std::move(record));
                feedback = "The previous decision was incomplete or invalid; produce a complete "
                           "decision object.";
                continue;
            }
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "decision remained malformed after repair budget";
            result.steps.push_back(std::move(record));
            break;
        }

        const auto decision = outcome.parse.decision.value();
        record.decision_digest = decision.decision_digest_field;
        const auto action = decision_string(decision.value, "action").value_or("fail");

        // --- Act ---------------------------------------------------------
        if (action == "fail") {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary =
                "model declared failure: " +
                decision_string(decision.value, "reason").value_or("no reason given");
            record.action_summary = "fail";
            result.steps.push_back(std::move(record));
            break;
        }
        if (action == "done") {
            // Verify: the claim alone never settles the loop.
            auto verify_observation = observe_once(spec, context, ObservationMode::Verification);
            if (verify_observation) {
                const auto verdict = verifier.verify(verify_observation.value(), decision);
                if (verdict == ILoopVerifier::Verdict::Satisfied) {
                    record.phase = StepPhase::Verified;
                    record.action_summary = "done";
                    record.verified = true;
                    result.steps.push_back(std::move(record));
                    result.outcome = LoopOutcome::Completed;
                    result.safe_summary = "goal verified against a fresh observation";
                    break;
                }
                if (verdict == ILoopVerifier::Verdict::Invalid) {
                    result.outcome = LoopOutcome::Failed;
                    result.safe_summary = "verifier rejected the decision as invalid";
                    result.steps.push_back(std::move(record));
                    break;
                }
                record.note = "model claimed done but verification disagreed";
                result.steps.push_back(std::move(record));
                feedback = "Verification disagreed with the done claim; continue the task.";
                continue;
            }
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "verification observation failed";
            result.steps.push_back(std::move(record));
            break;
        }

        auto sequence = compile_discrete_action(decision.value);
        if (!sequence) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "decision did not compile to a discrete action";
            result.steps.push_back(std::move(record));
            break;
        }
        record.action_summary = summarize_action(sequence.value());

        OperationContext action_context = context;
        action_context.operation = OperationId::generate();
        emit(spec, "ActionDispatched",
             JsonValue::Object{{"step", static_cast<std::int64_t>(step)},
                               {"action", record.action_summary},
                               {"decision_digest",
                                decision.decision_digest_field.to_string()}},
             EventClass::State);
        auto receipt = environment_->execute(sequence.value(), action_context);
        if (!receipt) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "action dispatch failed: " + receipt.error().safe_message;
            result.steps.push_back(std::move(record));
            break;
        }
        record.phase = StepPhase::Acted;
        if (receipt.value().status == ExecutionStatus::Rejected) {
            if (result.recoveries < config_.max_recoveries_per_step) {
                ++result.recoveries;
                record.note = "action rejected; recovering";
                result.steps.push_back(std::move(record));
                feedback = "The previous action was rejected by the platform.";
                continue;
            }
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "action rejected and recovery budget exhausted";
            result.steps.push_back(std::move(record));
            break;
        }
        if (receipt.value().side_effect_may_have_occurred) {
            record.note = "uncertain side effect; verifying instead of resending";
        }

        // --- Verify ------------------------------------------------------
        auto verify_observation = observe_once(spec, context, ObservationMode::Verification);
        if (!verify_observation) {
            result.outcome = LoopOutcome::Failed;
            result.safe_summary = "verification observation failed";
            result.steps.push_back(std::move(record));
            break;
        }
        const auto verdict = verifier.verify(verify_observation.value(), decision);
        record.phase = StepPhase::Verified;
        emit(spec, "VerificationResult",
             JsonValue::Object{{"step", static_cast<std::int64_t>(step)},
                               {"verdict", verdict == ILoopVerifier::Verdict::Satisfied
                                               ? "satisfied"
                                               : "not-satisfied"}},
             EventClass::State);
        if (verdict == ILoopVerifier::Verdict::Satisfied) {
            record.verified = true;
            result.steps.push_back(std::move(record));
            result.outcome = LoopOutcome::Completed;
            result.safe_summary = "goal verified after action";
            break;
        }
        result.steps.push_back(std::move(record));
    }

    if (result.outcome == LoopOutcome::MaxSteps && result.safe_summary.empty()) {
        result.safe_summary = "step budget exhausted before verification";
    }
    emit(spec, "LoopSettled",
         JsonValue::Object{{"outcome", loop_outcome_name(result.outcome)},
                           {"steps", static_cast<std::int64_t>(result.steps.size())},
                           {"recoveries", static_cast<std::int64_t>(result.recoveries)}},
         EventClass::State);
    return result;
}

} // namespace mira
