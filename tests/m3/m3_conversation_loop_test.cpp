// ConversationLoop tests (DEC-047, issue #73 / MIRA-20261004-001): the
// no-observation conversational loop over a scripted gateway — plain text
// answers, canonical tool round trips riding ToolCallPart/ToolResultPart,
// fail-closed refusal/registry/budget paths, cancellation, recovery budgets
// and turn limits. The same file pins the parse_decision Text-mode semantics
// and the contract/wire-encoding shape of the new canonical tool parts.

#include "support/harness_support.hpp"

#include "support/test.hpp"

#include <mira/conversation_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class ConversationFixture final {
  public:
    ConversationFixture() {
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 2;
        executor_config.max_threads = 2;
        executor_config.queue_capacity = 32;
        executor_.initialize(executor_config);
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
        // The profile keeps its default output limit (16384): the loop's
        // whole-run envelope (per-turn tokens x allowed requests) is compared
        // against it by the router, and the shipped defaults (1024 x 15) must
        // route. default_config_routes_against_default_profile_limits pins
        // that contract explicitly.
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                  ModelGatewayConfig{});
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);

        BuiltinToolSpec echo;
        echo.wire_name = "echo";
        echo.description = "Returns the message argument verbatim for round-trip tests.";
        echo.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"message": {"type": "string"}},
            "required": ["message"],
            "additionalProperties": false
        })json")
                                                .value()};
        echo_spec_ = echo;
        (void)registry_->register_tool(
            std::move(echo), [this](const JsonValue &arguments, const OperationContext &) {
                const std::lock_guard lock(arguments_mutex_);
                seen_arguments_.push_back(arguments);
                return JsonValue(JsonValue::Object{{"echo", arguments}});
            });

        spec_.task_id = TaskId::generate();
        spec_.session_id = SessionId::generate();
        spec_.task_epoch = 1;
        spec_.profile_id = profile_->id;
        spec_.goal = "what is the answer";
    }

    ~ConversationFixture() { (void)executor_.shutdown(true); }

    void use_provider(std::vector<ModelResponse> script) {
        provider_ = std::make_shared<RecordingProvider>(profile_, std::move(script));
        gateway_->register_provider(provider_);
    }

    // A provider whose first calls fail with scripted errors (for gateway
    // give-up paths) before serving the scripted responses.
    void use_flaky_provider(std::vector<Error> failures, std::vector<ModelResponse> script) {
        provider_ = nullptr;
        flaky_ = std::make_shared<FlakyProvider>(profile_, std::move(failures), std::move(script));
        gateway_->register_provider(flaky_);
    }

    [[nodiscard]] ConversationLoop make_loop(const ConversationLoopConfig &config,
                                             bool with_tools = true) {
        ConversationLoop loop(*gateway_, config);
        loop.set_event_store(events_, runtime_, spec_.session_id);
        if (with_tools) {
            loop.set_tool_registry(registry_);
        }
        admission_->activate(spec_.task_id, spec_.task_epoch);
        return loop;
    }

    [[nodiscard]] const std::vector<JsonValue> &seen_arguments() const {
        const std::lock_guard lock(arguments_mutex_);
        return seen_arguments_;
    }

    executor::Executor executor_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<RecordingProvider> provider_;
    std::shared_ptr<BuiltinToolRegistry> registry_ = std::make_shared<BuiltinToolRegistry>();
    BuiltinToolSpec echo_spec_;
    std::shared_ptr<MemoryEventStore> events_ = std::make_shared<MemoryEventStore>();
    RuntimeId runtime_ = RuntimeId::generate();
    AgentLoopSpec spec_;

    [[nodiscard]] const BuiltinToolSpec &echo_spec() const { return echo_spec_; }

  private:
    class FlakyProvider final : public IModelProvider {
      public:
        FlakyProvider(std::shared_ptr<const ModelProfile> profile, std::vector<Error> failures,
                      std::vector<ModelResponse> script)
            : profile_(std::move(profile)), failures_(std::move(failures)),
              script_(std::move(script)) {}

        [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
        [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                                  const OperationContext &context,
                                                  const ProviderInferOptions &) override {
            if (context.cancelled()) {
                return make_model_error(ModelDomainCode::ModelCancelled, "flaky provider cancelled",
                                        false, request.operation_id);
            }
            const std::lock_guard lock(mutex_);
            if (failure_cursor_ < failures_.size()) {
                return failures_[failure_cursor_++];
            }
            if (script_cursor_ >= script_.size()) {
                return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                        "flaky provider is exhausted", false, request.operation_id);
            }
            ModelResponse response = script_[script_cursor_++];
            response.request_id = request.request_id;
            response.operation_id = request.operation_id;
            response.profile_id = request.profile_id;
            return response;
        }

      private:
        std::shared_ptr<const ModelProfile> profile_;
        mutable std::mutex mutex_;
        std::vector<Error> failures_;
        std::vector<ModelResponse> script_;
        std::size_t failure_cursor_ = 0;
        std::size_t script_cursor_ = 0;
    };

    std::shared_ptr<FlakyProvider> flaky_;

    mutable std::mutex arguments_mutex_;
    std::vector<JsonValue> seen_arguments_;
};

[[nodiscard]] OperationContext conversation_context(std::function<bool()> cancelled = nullptr) {
    OperationContext context;
    context.session = SessionId::generate();
    context.task = TaskId::generate();
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.cancellation_requested = std::move(cancelled);
    return context;
}

[[nodiscard]] std::vector<std::string> session_event_types(const MemoryEventStore &store,
                                                           const SessionId &session) {
    EventQuery query;
    query.session_id = session;
    auto page = store.read(query);
    std::vector<std::string> types;
    if (!page.has_value()) {
        return types;
    }
    for (const auto &event : page.value().events) {
        types.push_back(event.payload.type);
    }
    return types;
}

// ---------------------------------------------------------------------------
// ConversationLoop scenarios
// ---------------------------------------------------------------------------

int conversation_stream_options_are_forwarded() {
    ConversationFixture fixture;
    fixture.profile_->capabilities.generation.reasoning_effort = ParamMapping::OmitIfUnset;
    auto answer = text_response("canonical answer");
    answer.usage.input_tokens = 123;
    answer.usage.quality = UsageQuality::ProviderReported;
    fixture.use_provider({answer});
    ConversationLoopConfig config;
    config.reasoning_effort = ReasoningEffort::High;
    config.inference.stream = true;
    std::string preview;
    config.inference.preview_sink = [&](const auto &, const auto &snapshot) {
        preview = snapshot.text;
    };
    auto loop = fixture.make_loop(config);
    auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().answer == "canonical answer");
    MIRA_CHECK(preview == "fixture preview");
    MIRA_CHECK(result.value().last_usage.input_tokens == 123);
    MIRA_CHECK(fixture.provider_->requests()[0].generation.reasoning_effort ==
               ReasoningEffort::High);
    return 0;
}

int plain_text_answer_settles_immediately() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("the answer is 42")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "the answer is 42");
    MIRA_CHECK(result.value().turns.size() == 1);
    MIRA_CHECK(result.value().turns.front().summary == "answer");
    MIRA_CHECK(result.value().recoveries == 0);

    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(!types.empty());
    MIRA_CHECK(types.back() == "ConversationSettled");
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolExecuted") == types.end());
    return 0;
}

int signed_thinking_replayed_before_tool_results() {
    ConversationFixture fixture;
    auto first = tool_call_response(fixture.echo_spec(), R"({"message":"ping"})");
    first.output.insert(first.output.begin(), ThinkingPart{"plan", "signed", false});
    first.output.insert(first.output.begin() + 1,
                        MessageOutput{ModelRole::Assistant, {OutputTextPart{"comment", {}}}});
    fixture.use_provider({first, text_response("done")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    auto found =
        std::find_if(requests[1].input.begin(), requests[1].input.end(), [](const auto &item) {
            return !item.content.empty() &&
                   std::holds_alternative<ThinkingPart>(item.content.front());
        });
    MIRA_CHECK(found != requests[1].input.end() && found->role == ModelRole::Assistant);
    MIRA_CHECK(found->content.size() == 3);
    MIRA_CHECK(std::get<ThinkingPart>(found->content[0]).signature == "signed");
    MIRA_CHECK(std::get<TextPart>(found->content[1]).text == "comment");
    MIRA_CHECK(std::holds_alternative<ToolCallPart>(found->content[2]));
    MIRA_CHECK(std::next(found) != requests[1].input.end() &&
               std::holds_alternative<ToolResultPart>(std::next(found)->content[0]));
    return 0;
}

int tool_round_trip_replays_canonical_parts() {
    ConversationFixture fixture;
    const auto arguments = parse_json(R"json({"message": "ping"})json");
    MIRA_CHECK(arguments.has_value());
    fixture.use_provider({
        tool_call_response(fixture.echo_spec(), R"json({"message": "ping"})json"),
        text_response("final answer after the tool"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "final answer after the tool");
    MIRA_CHECK(result.value().turns.size() == 2);
    MIRA_CHECK(result.value().tool_executions == 1);
    MIRA_CHECK(result.value().turns.front().summary == "tool:echo");
    MIRA_CHECK(result.value().turns.back().summary == "answer");

    // The handler saw exactly the resolved arguments.
    MIRA_CHECK(fixture.seen_arguments().size() == 1);
    MIRA_CHECK(to_json_string(fixture.seen_arguments().front()) ==
               to_json_string(arguments.value()));

    // The first request exposed the tool; the second carried the canonical
    // round trip (assistant call echo + user result) as native parts.
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    MIRA_CHECK(requests.front().tools.size() == 1);
    MIRA_CHECK(requests.front().tools.front().wire_name == "echo");

    const ToolCallPart *call_part = nullptr;
    const ToolResultPart *result_part = nullptr;
    std::optional<ModelRole> call_role;
    std::optional<ModelRole> result_role;
    for (const auto &item : requests[1].input) {
        for (const auto &part : item.content) {
            if (const auto *call = std::get_if<ToolCallPart>(&part)) {
                call_part = call;
                call_role = item.role;
            } else if (const auto *record = std::get_if<ToolResultPart>(&part)) {
                result_part = record;
                result_role = item.role;
            }
        }
    }
    MIRA_CHECK(call_part != nullptr);
    MIRA_CHECK(call_role.has_value() && *call_role == ModelRole::Assistant);
    MIRA_CHECK(call_part->provider_call_id.value == "call-1");
    MIRA_CHECK(call_part->wire_name == "echo");
    MIRA_CHECK(to_json_string(call_part->arguments) == to_json_string(arguments.value()));
    MIRA_CHECK(call_part->arguments_digest == digest_string(to_json_string(arguments.value())));
    MIRA_CHECK(result_part != nullptr);
    MIRA_CHECK(result_role.has_value() && *result_role == ModelRole::User);
    MIRA_CHECK(result_part->provider_call_id.value == "call-1");
    MIRA_CHECK(!result_part->failed);
    const auto *echoed = result_part->result.find("echo");
    MIRA_CHECK(echoed != nullptr);
    MIRA_CHECK(to_json_string(*echoed) == to_json_string(arguments.value()));

    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolExecuted") != types.end());
    MIRA_CHECK(!types.empty() && types.back() == "ConversationSettled");
    return 0;
}

int default_config_routes_against_default_profile_limits() {
    // F2 regression pin: the shipped ConversationLoopConfig defaults must
    // produce requests whose whole-run output envelope (per-turn tokens x
    // turns+recoveries+1) stays under the profile's default output limit, so
    // a plain default-config run routes and answers instead of failing the
    // route query.
    ConversationFixture fixture;
    fixture.use_provider({text_response("routed with defaults")});
    MIRA_CHECK(fixture.profile_->capabilities.limits.max_output_tokens == 16'384);
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "routed with defaults");
    MIRA_CHECK(fixture.provider_->requests().size() == 1);
    return 0;
}

int tool_proposals_without_registry_fail_closed() {
    ConversationFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.echo_spec(), R"json({"message": "ping"})json"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{}, /*with_tools=*/false);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    // Acceptance (issue #73): the failure names the missing registry, fails
    // on the first model call and spends no recovery budget.
    MIRA_CHECK(result.value().safe_summary.find("tool registry") != std::string::npos);
    MIRA_CHECK(fixture.provider_->requests().size() == 1);
    MIRA_CHECK(result.value().turns.size() == 1);
    MIRA_CHECK(result.value().recoveries == 0);
    // Nothing may execute without a registry.
    MIRA_CHECK(fixture.seen_arguments().empty());
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(std::find(types.begin(), types.end(), "ToolExecuted") == types.end());
    MIRA_CHECK(!types.empty() && types.back() == "ConversationSettled");
    return 0;
}

int tool_execution_budget_is_enforced() {
    ConversationFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.echo_spec(), R"json({"message": "one"})json", "call-1"),
        tool_call_response(fixture.echo_spec(), R"json({"message": "two"})json", "call-2"),
    });
    ConversationLoopConfig config;
    config.max_tool_executions = 1;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary.find("tool execution budget") != std::string::npos);
    MIRA_CHECK(fixture.seen_arguments().size() == 1);
    return 0;
}

int refusal_fails_closed() {
    ConversationFixture fixture;
    fixture.use_provider({refused_response()});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary.find("no policy bypass") != std::string::npos);
    MIRA_CHECK(result.value().answer.empty());
    return 0;
}

int pre_cancelled_context_settles_cancelled() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("never sent")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context([] { return true; }));
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Cancelled);
    // Cancellation trips before any model call leaves the loop.
    MIRA_CHECK(fixture.provider_->requests().empty());
    const auto types = session_event_types(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(!types.empty() && types.back() == "ConversationSettled");
    return 0;
}

int admission_rejection_settles_cancelled() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("never admitted")});
    ConversationLoop loop(*fixture.gateway_, ConversationLoopConfig{});
    // No admission activation: the gateway rejects the request as cancelled
    // (unadmitted epoch == takeover/cancel semantics) before dispatch.
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Cancelled);
    MIRA_CHECK(fixture.provider_->requests().empty());
    return 0;
}

int empty_answer_recovers_then_fails_closed() {
    ConversationFixture fixture;
    fixture.use_provider({text_response(""), text_response("")});
    ConversationLoopConfig config;
    config.max_recoveries = 1;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().recoveries == 1);
    MIRA_CHECK(result.value().turns.size() == 2);
    MIRA_CHECK(result.value().safe_summary.find("no answer text") != std::string::npos);
    // The recovery retry carried explicit feedback asking for text.
    const auto second_text = fixture.provider_->request_text(1);
    MIRA_CHECK(second_text.find("The previous reply carried no answer text") != std::string::npos);
    return 0;
}

int turn_budget_exhaustion_settles_max_turns() {
    ConversationFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.echo_spec(), R"json({"message": "one"})json", "call-1"),
        tool_call_response(fixture.echo_spec(), R"json({"message": "two"})json", "call-2"),
    });
    ConversationLoopConfig config;
    config.max_turns = 2;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::MaxTurns);
    MIRA_CHECK(result.value().turns.size() == 2);
    MIRA_CHECK(result.value().safe_summary.find("turn budget exhausted") != std::string::npos);
    MIRA_CHECK(fixture.seen_arguments().size() == 2);
    return 0;
}

int text_only_turn_budget_exhaustion_settles_max_turns() {
    ConversationFixture fixture;
    // Two empty answers burn both recoveries inside two turns; the turn
    // budget then expires before a terminal answer (text-only variant that
    // does not depend on the tool round-trip path).
    fixture.use_provider({text_response(""), text_response("")});
    ConversationLoopConfig config;
    config.max_turns = 2;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::MaxTurns);
    MIRA_CHECK(result.value().turns.size() == 2);
    MIRA_CHECK(result.value().recoveries == 2);
    MIRA_CHECK(result.value().safe_summary.find("turn budget exhausted") != std::string::npos);
    return 0;
}

int empty_goal_is_rejected() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("unused")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    auto spec = fixture.spec_;
    spec.goal.clear();
    const auto result = loop.run(spec, conversation_context());
    MIRA_CHECK(!result.has_value());
    MIRA_CHECK(result.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(fixture.provider_->requests().empty());
    return 0;
}

int recoverable_model_failures_settle_after_retry() {
    // Recovery that succeeds: the gateway exhausts its own retry budget
    // (two consecutive rate limits) and the loop spends one recovery before
    // the text answer settles.
    {
        ConversationFixture fixture;
        fixture.use_flaky_provider(
            {make_model_error(ModelDomainCode::RateLimited, "rate limited", true, std::nullopt),
             make_model_error(ModelDomainCode::RateLimited, "rate limited", true, std::nullopt)},
            {text_response("recovered answer")});
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(result.value().answer == "recovered answer");
        MIRA_CHECK(result.value().recoveries == 1);
    }
    // Recovery budget exhausted: repeated give-ups settle Failed.
    {
        ConversationFixture fixture;
        std::vector<Error> failures;
        for (int index = 0; index < 8; ++index) {
            failures.push_back(
                make_model_error(ModelDomainCode::RateLimited, "rate limited", true, std::nullopt));
        }
        fixture.use_flaky_provider(std::move(failures), {text_response("never reached")});
        ConversationLoopConfig config;
        config.max_recoveries = 1;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().recoveries == 1);
        MIRA_CHECK(result.value().safe_summary.find("model call failed") != std::string::npos);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// parse_decision Text-mode semantics (DEC-047)
// ---------------------------------------------------------------------------

[[nodiscard]] ModelRequest parse_probe_request(OutputMode mode) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart system_text;
    system_text.text = "answer briefly";
    system_item.content.emplace_back(std::move(system_text));
    request.input.push_back(std::move(system_item));
    request.output_contract.mode = mode;
    request.data_policy.store = false;
    return request;
}

[[nodiscard]] ModelResponse tool_call_output_response(bool with_commentary) {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.status = ModelCompletionStatus::Completed;
    ToolCallOutput call;
    call.provider_call_id = ProviderToolCallId{"call-1"};
    call.tool_id = ToolId::generate();
    call.provider_name = "echo";
    call.arguments = parse_json(R"json({"message": "hi"})json").value();
    call.arguments_digest = digest_string(to_json_string(call.arguments));
    response.output.emplace_back(std::move(call));
    if (with_commentary) {
        MessageOutput note;
        note.role = ModelRole::Assistant;
        OutputTextPart text;
        text.text = "let me check with a tool first";
        note.content.emplace_back(std::move(text));
        response.output.emplace_back(std::move(note));
    }
    return response;
}

int text_mode_tool_calls_parse_as_proposals() {
    // Text mode: a completed tool call is a proposal batch, not an ambiguity.
    const auto request = parse_probe_request(OutputMode::Text);
    auto plain = parse_decision(request, tool_call_output_response(false));
    MIRA_CHECK(plain.outcome == DecisionParseOutcome::ToolProposals);

    // Commentary text alongside the call does not make it terminal; the call
    // still wins (DEC-047).
    auto commented = parse_decision(request, tool_call_output_response(true));
    MIRA_CHECK(commented.outcome == DecisionParseOutcome::ToolProposals);

    // Regression guard: structured modes keep mixing ambiguous.
    auto strict = parse_probe_request(OutputMode::StrictJsonSchema);
    strict.output_contract.schema_id = SchemaId::generate();
    strict.output_contract.schema = JsonSchema{parse_json(R"json({
        "type": "object",
        "properties": {"action": {"type": "string"}},
        "required": ["action"],
        "additionalProperties": false
    })json")
                                                   .value()};
    strict.output_contract.canonical_schema_digest =
        canonical_json_digest(strict.output_contract.schema.root);
    auto mixed = parse_decision(strict, tool_call_output_response(true));
    MIRA_CHECK(mixed.outcome == DecisionParseOutcome::Ambiguous);
    return 0;
}

// ---------------------------------------------------------------------------
// Canonical tool part contracts
// ---------------------------------------------------------------------------

struct SampleToolRound final {
    ToolProposal proposal;
    ToolExecutionRecord record;
    ModelInputItem call_item;
    ModelInputItem result_item;
};

[[nodiscard]] SampleToolRound make_sample_round() {
    SampleToolRound round;
    round.proposal.provider_call_id = ProviderToolCallId{"call-9"};
    round.proposal.tool_id = ToolId::generate();
    round.proposal.wire_name = "echo";
    round.proposal.arguments = parse_json(R"json({"message": "ping"})json").value();
    round.proposal.arguments_digest = digest_string(to_json_string(round.proposal.arguments));

    round.record.provider_call_id = round.proposal.provider_call_id;
    round.record.tool_id = round.proposal.tool_id;
    round.record.result = JsonValue(JsonValue::Object{{"echo", round.proposal.arguments}});

    round.call_item = make_tool_call_item(round.proposal);
    round.result_item = make_tool_result_item(round.record);
    return round;
}

[[nodiscard]] ModelRequest request_with_tool_round(const SampleToolRound &round) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart system_text;
    system_text.text = "answer briefly";
    system_item.content.emplace_back(std::move(system_text));
    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    TextPart user_text;
    user_text.text = "hello";
    user_item.content.emplace_back(std::move(user_text));
    request.input = {std::move(system_item), std::move(user_item), round.call_item,
                     round.result_item};
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    return request;
}

int canonical_tool_item_builders_and_validation() {
    const auto round = make_sample_round();

    MIRA_CHECK(round.call_item.role == ModelRole::Assistant);
    MIRA_CHECK(round.call_item.provenance.source == "mira.tool-call-echo.v1");
    MIRA_CHECK(round.call_item.content.size() == 1);
    const auto *call = std::get_if<ToolCallPart>(&round.call_item.content.front());
    MIRA_CHECK(call != nullptr);
    MIRA_CHECK(call->provider_call_id.value == "call-9");
    MIRA_CHECK(call->wire_name == "echo");
    MIRA_CHECK(to_json_string(call->arguments) == to_json_string(round.proposal.arguments));
    MIRA_CHECK(call->arguments_digest == round.proposal.arguments_digest);

    MIRA_CHECK(round.result_item.role == ModelRole::User);
    MIRA_CHECK(round.result_item.provenance.source == "mira.tool-result.v1");
    MIRA_CHECK(round.result_item.content.size() == 1);
    const auto *record = std::get_if<ToolResultPart>(&round.result_item.content.front());
    MIRA_CHECK(record != nullptr);
    MIRA_CHECK(record->provider_call_id.value == "call-9");
    MIRA_CHECK(record->tool_id == round.proposal.tool_id);
    MIRA_CHECK(to_json_string(record->result) == to_json_string(round.record.result));
    MIRA_CHECK(!record->failed);

    // The well-formed round trip passes request validation.
    auto request = request_with_tool_round(round);
    MIRA_CHECK(validate_model_request(request).has_value());

    // Arguments digest mismatch fails closed.
    auto mismatched = request;
    {
        auto &item = mismatched.input[2];
        auto *part = std::get_if<ToolCallPart>(&item.content.front());
        MIRA_CHECK(part != nullptr);
        part->arguments = parse_json(R"json({"message": "tampered"})json").value();
    }
    MIRA_CHECK(!validate_model_request(mismatched).has_value());

    // Empty call id / wire name fail closed.
    auto empty_id = request;
    {
        auto *part = std::get_if<ToolCallPart>(&empty_id.input[2].content.front());
        part->provider_call_id = ProviderToolCallId{};
    }
    MIRA_CHECK(!validate_model_request(empty_id).has_value());

    // Failed results must carry a bounded safe summary.
    auto failed_without_summary = request;
    {
        auto *part = std::get_if<ToolResultPart>(&failed_without_summary.input[3].content.front());
        part->failed = true;
    }
    MIRA_CHECK(!validate_model_request(failed_without_summary).has_value());

    auto oversize_summary = request;
    {
        auto *part = std::get_if<ToolResultPart>(&oversize_summary.input[3].content.front());
        part->failed = true;
        part->safe_error_summary = std::string(2049, 'x');
    }
    MIRA_CHECK(!validate_model_request(oversize_summary).has_value());

    // Results without an artifact reference stay under the inline limit.
    auto oversize_result = request;
    {
        auto *part = std::get_if<ToolResultPart>(&oversize_result.input[3].content.front());
        JsonValue::Array bulky;
        for (int index = 0; index < 20000; ++index) {
            bulky.emplace_back(std::string(64, 'x'));
        }
        part->result = JsonValue(std::move(bulky));
    }
    MIRA_CHECK(!validate_model_request(oversize_result).has_value());
    return 0;
}

int tool_part_json_round_trip() {
    const auto round = make_sample_round();
    const auto request = request_with_tool_round(round);

    const auto json = model_request_to_json(request);
    auto parsed = parse_json(to_json_string(json));
    MIRA_CHECK(parsed.has_value());
    auto restored = model_request_from_json(parsed.value());
    MIRA_CHECK(restored.has_value());
    MIRA_CHECK(restored.value().input.size() == request.input.size());

    const auto &call_out = restored.value().input[2];
    MIRA_CHECK(call_out.role == ModelRole::Assistant);
    MIRA_CHECK(call_out.provenance.source == "mira.tool-call-echo.v1");
    const auto *call = std::get_if<ToolCallPart>(&call_out.content.front());
    MIRA_CHECK(call != nullptr);
    MIRA_CHECK(call->provider_call_id.value == "call-9");
    MIRA_CHECK(call->wire_name == "echo");
    MIRA_CHECK(to_json_string(call->arguments) == to_json_string(round.proposal.arguments));
    MIRA_CHECK(call->arguments_digest == round.proposal.arguments_digest);

    const auto &result_out = restored.value().input[3];
    MIRA_CHECK(result_out.role == ModelRole::User);
    MIRA_CHECK(result_out.provenance.source == "mira.tool-result.v1");
    const auto *record = std::get_if<ToolResultPart>(&result_out.content.front());
    MIRA_CHECK(record != nullptr);
    MIRA_CHECK(record->provider_call_id.value == "call-9");
    MIRA_CHECK(record->tool_id == round.proposal.tool_id);
    MIRA_CHECK(to_json_string(record->result) == to_json_string(round.record.result));
    MIRA_CHECK(record->failed == round.record.failed);
    MIRA_CHECK(record->safe_error_summary == round.record.safe_error_summary);

    // The canonical prompt digest covers the new parts and is stable.
    std::vector<ModelInputItem> without_round(request.input.begin(), request.input.begin() + 2);
    MIRA_CHECK(prompt_digest(request.input) != prompt_digest(without_round));
    MIRA_CHECK(prompt_digest(request.input) == prompt_digest(request.input));
    return 0;
}

int dialects_encode_tool_round_trip_and_reject_mixing() {
    const auto round = make_sample_round();
    auto request = request_with_tool_round(round);

    NullArtifactSource artifacts;
    ResponsesV1Mapper responses;
    ChatCompletionsV1Mapper chat;

    const auto responses_profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    const auto chat_profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test"));

    // Responses dialect: native function_call / function_call_output items.
    auto wire = responses.encode_request(request, *responses_profile, false, artifacts);
    MIRA_CHECK(wire.has_value());
    const auto *input_array = wire.value().find("input");
    MIRA_CHECK(input_array != nullptr && input_array->is_array() &&
               input_array->as_array() != nullptr);
    const JsonValue *call_wire = nullptr;
    const JsonValue *output_wire = nullptr;
    for (const auto &element : *input_array->as_array()) {
        const auto *type = element.find("type");
        if (type == nullptr || !type->is_string()) {
            continue;
        }
        if (*type->as_string() == "function_call") {
            call_wire = &element;
        } else if (*type->as_string() == "function_call_output") {
            output_wire = &element;
        }
    }
    MIRA_CHECK(call_wire != nullptr);
    MIRA_CHECK(call_wire->find("call_id") != nullptr &&
               call_wire->find("call_id")->as_string() != nullptr &&
               *call_wire->find("call_id")->as_string() == "call-9");
    MIRA_CHECK(call_wire->find("name") != nullptr &&
               *call_wire->find("name")->as_string() == "echo");
    MIRA_CHECK(call_wire->find("arguments") != nullptr &&
               *call_wire->find("arguments")->as_string() ==
                   to_json_string(round.proposal.arguments));
    MIRA_CHECK(output_wire != nullptr);
    MIRA_CHECK(output_wire->find("call_id") != nullptr &&
               *output_wire->find("call_id")->as_string() == "call-9");
    MIRA_CHECK(output_wire->find("output") != nullptr &&
               output_wire->find("output")->as_string() != nullptr);
    const auto envelope = parse_json(*output_wire->find("output")->as_string());
    MIRA_CHECK(envelope.has_value());
    const auto *status = envelope.value().find("status");
    MIRA_CHECK(status != nullptr && status->is_string() && *status->as_string() == "ok");
    const auto *result_value = envelope.value().find("result");
    MIRA_CHECK(result_value != nullptr);
    MIRA_CHECK(to_json_string(*result_value) == to_json_string(round.record.result));

    // Chat Completions dialect: assistant tool_calls + tool role messages.
    auto chat_wire = chat.encode_request(request, *chat_profile, false, artifacts);
    MIRA_CHECK(chat_wire.has_value());
    const auto *messages = chat_wire.value().find("messages");
    MIRA_CHECK(messages != nullptr && messages->is_array() && messages->as_array() != nullptr);
    const JsonValue *assistant_wire = nullptr;
    const JsonValue *tool_wire = nullptr;
    for (const auto &element : *messages->as_array()) {
        const auto *role = element.find("role");
        if (role == nullptr || !role->is_string()) {
            continue;
        }
        if (*role->as_string() == "assistant" && element.find("tool_calls") != nullptr) {
            assistant_wire = &element;
        } else if (*role->as_string() == "tool") {
            tool_wire = &element;
        }
    }
    MIRA_CHECK(assistant_wire != nullptr);
    MIRA_CHECK(assistant_wire->find("content") != nullptr &&
               !assistant_wire->find("content")->is_string());
    const auto *tool_calls = assistant_wire->find("tool_calls");
    MIRA_CHECK(tool_calls != nullptr && tool_calls->is_array() &&
               tool_calls->as_array() != nullptr && tool_calls->as_array()->size() == 1);
    const auto &entry = tool_calls->as_array()->front();
    MIRA_CHECK(entry.find("id") != nullptr && *entry.find("id")->as_string() == "call-9");
    MIRA_CHECK(entry.find("type") != nullptr && *entry.find("type")->as_string() == "function");
    const auto *function = entry.find("function");
    MIRA_CHECK(function != nullptr);
    MIRA_CHECK(function->find("name") != nullptr && *function->find("name")->as_string() == "echo");
    MIRA_CHECK(function->find("arguments") != nullptr &&
               *function->find("arguments")->as_string() ==
                   to_json_string(round.proposal.arguments));
    MIRA_CHECK(tool_wire != nullptr);
    MIRA_CHECK(tool_wire->find("tool_call_id") != nullptr &&
               *tool_wire->find("tool_call_id")->as_string() == "call-9");
    MIRA_CHECK(tool_wire->find("content") != nullptr &&
               tool_wire->find("content")->as_string() != nullptr);
    const auto chat_envelope = parse_json(*tool_wire->find("content")->as_string());
    MIRA_CHECK(chat_envelope.has_value());
    const auto *chat_status = chat_envelope.value().find("status");
    MIRA_CHECK(chat_status != nullptr && *chat_status->as_string() == "ok");
    const auto *chat_result = chat_envelope.value().find("result");
    MIRA_CHECK(chat_result != nullptr);
    MIRA_CHECK(to_json_string(*chat_result) == to_json_string(round.record.result));

    // Mixed items (tool part next to text) fail closed in both dialects.
    ModelInputItem mixed_item;
    mixed_item.role = ModelRole::Assistant;
    mixed_item.content.emplace_back(round.call_item.content.front());
    TextPart stray;
    stray.text = "mixed in";
    mixed_item.content.emplace_back(std::move(stray));
    auto mixed_request = request;
    mixed_request.input.push_back(mixed_item);
    auto mixed_responses =
        responses.encode_request(mixed_request, *responses_profile, false, artifacts);
    MIRA_CHECK(!mixed_responses.has_value());
    MIRA_CHECK(mixed_responses.error().domain == "mira.model");
    MIRA_CHECK(mixed_responses.error().code == ErrorCode::InvalidArgument);
    auto mixed_chat = chat.encode_request(mixed_request, *chat_profile, false, artifacts);
    MIRA_CHECK(!mixed_chat.has_value());

    // Profiles without the function-tools capability fail closed too.
    auto no_tools_profile = std::make_shared<ModelProfile>(*responses_profile);
    no_tools_profile->capabilities.function_tools =
        CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    auto denied = responses.encode_request(request, *no_tools_profile, false, artifacts);
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(denied.error().code == ErrorCode::UnsupportedCapability);
    return 0;
}

} // namespace

int main() {
    if (signed_thinking_replayed_before_tool_results())
        return 1;
    if (auto status = conversation_stream_options_are_forwarded())
        return status;
    if (const int code = plain_text_answer_settles_immediately(); code != 0) {
        return code;
    }
    if (const int code = default_config_routes_against_default_profile_limits(); code != 0) {
        return code;
    }
    if (const int code = tool_round_trip_replays_canonical_parts(); code != 0) {
        return code;
    }
    if (const int code = tool_proposals_without_registry_fail_closed(); code != 0) {
        return code;
    }
    if (const int code = tool_execution_budget_is_enforced(); code != 0) {
        return code;
    }
    if (const int code = refusal_fails_closed(); code != 0) {
        return code;
    }
    if (const int code = pre_cancelled_context_settles_cancelled(); code != 0) {
        return code;
    }
    if (const int code = admission_rejection_settles_cancelled(); code != 0) {
        return code;
    }
    if (const int code = empty_answer_recovers_then_fails_closed(); code != 0) {
        return code;
    }
    if (const int code = text_only_turn_budget_exhaustion_settles_max_turns(); code != 0) {
        return code;
    }
    if (const int code = turn_budget_exhaustion_settles_max_turns(); code != 0) {
        return code;
    }
    if (const int code = empty_goal_is_rejected(); code != 0) {
        return code;
    }
    if (const int code = recoverable_model_failures_settle_after_retry(); code != 0) {
        return code;
    }
    if (const int code = text_mode_tool_calls_parse_as_proposals(); code != 0) {
        return code;
    }
    if (const int code = canonical_tool_item_builders_and_validation(); code != 0) {
        return code;
    }
    if (const int code = tool_part_json_round_trip(); code != 0) {
        return code;
    }
    return dialects_encode_tool_round_trip_and_reject_mixing();
}
