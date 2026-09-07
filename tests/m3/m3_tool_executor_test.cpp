// BuiltinToolRegistry and conversation projection contract tests (DEC-015,
// DEC-016). Every fail-closed path of the execution boundary has a negative
// case here; the loop-level closure lives in m3_tool_loop_test.cpp.

#include "support/m3_support.hpp"

#include "support/test.hpp"

#include <mira/conversation_log.hpp>
#include <mira/event_store.hpp>
#include <mira/model_digest.hpp>
#include <mira/tool_executor.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] BuiltinToolSpec number_tool_spec(ToolId id,
                                               std::string wire_name = "double_number") {
    BuiltinToolSpec spec;
    spec.tool_id = id;
    spec.wire_name = std::move(wire_name);
    spec.description = "Doubles the input number.";
    const auto schema = parse_json(R"json({
        "type": "object",
        "properties": {"value": {"type": "number"}},
        "required": ["value"],
        "additionalProperties": false
    })json");
    spec.parameters_schema = JsonSchema{schema.value()};
    return spec;
}

[[nodiscard]] BuiltinToolHandler double_handler() {
    return [](const JsonValue &arguments, const OperationContext &) -> Result<JsonValue> {
        JsonValue::Object result;
        result.emplace_back("doubled", arguments.find("value")->as_number().value_or(0.0) * 2.0);
        return JsonValue(std::move(result));
    };
}

[[nodiscard]] ToolProposal proposal_for(const BuiltinToolSpec &spec, JsonValue arguments,
                                        OperationId operation = OperationId::generate()) {
    ToolProposal proposal;
    proposal.provider_call_id = ProviderToolCallId{"call-1"};
    proposal.tool_id = spec.tool_id;
    proposal.wire_name = spec.wire_name;
    proposal.tool_version = spec.version;
    proposal.arguments = std::move(arguments);
    proposal.arguments_digest = digest_string(to_json_string(proposal.arguments));
    proposal.operation_id = operation;
    proposal.has_side_effects = spec.has_side_effects;
    return proposal;
}

[[nodiscard]] OperationContext plain_context() {
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

int register_rejects_bad_specs() {
    BuiltinToolRegistry registry;
    const auto id = ToolId::generate();

    auto no_name = number_tool_spec(id);
    no_name.wire_name.clear();
    MIRA_CHECK(!registry.register_tool(no_name, double_handler()));

    auto hosted = number_tool_spec(id);
    hosted.wire_name = "bash";
    MIRA_CHECK(!registry.register_tool(hosted, double_handler()));

    auto bad_schema = number_tool_spec(id);
    const auto unsupported = parse_json(
        R"json({"type":"object","oneOf":[{"properties":{"value":{"type":"number"}}}]})json");
    bad_schema.parameters_schema = JsonSchema{unsupported.value()};
    MIRA_CHECK(!registry.register_tool(bad_schema, double_handler()));

    MIRA_CHECK(registry.register_tool(number_tool_spec(id), double_handler()));
    MIRA_CHECK(!registry.register_tool(number_tool_spec(id), double_handler()));

    auto clash = number_tool_spec(ToolId::generate());
    MIRA_CHECK(!registry.register_tool(clash, double_handler()));
    MIRA_CHECK(registry.size() == 1);

    auto sorted = registry.exposed_tools();
    MIRA_CHECK(sorted.size() == 1);
    MIRA_CHECK(sorted.front().wire_name == "double_number");
    MIRA_CHECK(!sorted.front().spec_digest.to_string().empty());
    // Deterministic snapshot: same registration state, same digest.
    MIRA_CHECK(registry.exposed_tools().front().spec_digest == sorted.front().spec_digest);
    return 0;
}

int execute_fail_closed_paths() {
    BuiltinToolRegistry registry;
    const auto spec = number_tool_spec(ToolId::generate());
    MIRA_CHECK(registry.register_tool(spec, double_handler()));
    const auto context = plain_context();

    // Unknown tool id.
    auto unknown = proposal_for(spec, parse_json(R"json({"value": 2})json").value());
    unknown.tool_id = ToolId::generate();
    MIRA_CHECK(!registry.execute(unknown, context));

    // Identity no longer matches the exposed snapshot.
    auto renamed = proposal_for(spec, parse_json(R"json({"value": 2})json").value());
    renamed.wire_name = "double_number_v2";
    MIRA_CHECK(!registry.execute(renamed, context));

    // At most once per operation id (W-02).
    const auto operation = OperationId::generate();
    auto once = proposal_for(spec, parse_json(R"json({"value": 2})json").value(), operation);
    const auto first = registry.execute(once, context);
    MIRA_CHECK(first && !first.value().failed);
    const auto again = registry.execute(once, context);
    MIRA_CHECK(!again);

    // Model-attributable failures come back as failed records, not errors.
    auto invalid = proposal_for(spec, parse_json(R"json({"value": "two"})json").value());
    const auto rejected = registry.execute(invalid, context);
    MIRA_CHECK(rejected && rejected.value().failed);
    MIRA_CHECK(rejected.value().safe_error_summary.find("schema validation") != std::string::npos);

    auto missing = proposal_for(spec, parse_json(R"json({})json").value());
    const auto absent = registry.execute(missing, context);
    MIRA_CHECK(absent && absent.value().failed);

    // Handler errors and escaping exceptions stay records for the model.
    BuiltinToolSpec failing_spec = number_tool_spec(ToolId::generate(), "failing_number");
    MIRA_CHECK(registry.register_tool(
        failing_spec, [](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "mira.test";
            error.safe_message = "boom";
            return error;
        }));
    auto failing = proposal_for(failing_spec, parse_json(R"json({"value": 1})json").value());
    const auto failed = registry.execute(failing, context);
    MIRA_CHECK(failed && failed.value().failed);
    MIRA_CHECK(failed.value().safe_error_summary == "boom");

    BuiltinToolSpec throwing_spec = number_tool_spec(ToolId::generate(), "throwing_number");
    MIRA_CHECK(registry.register_tool(
        throwing_spec, [](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            throw std::runtime_error("escaped");
        }));
    auto throwing = proposal_for(throwing_spec, parse_json(R"json({"value": 1})json").value());
    const auto caught = registry.execute(throwing, context);
    MIRA_CHECK(caught && caught.value().failed);
    MIRA_CHECK(caught.value().safe_error_summary.find("exception") != std::string::npos);

    // Oversized results are rejected records, never unbounded payloads.
    BuiltinToolSpec chatty_spec = number_tool_spec(ToolId::generate());
    chatty_spec.wire_name = "chatty";
    MIRA_CHECK(registry.register_tool(
        chatty_spec, [](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
            JsonValue::Object blob;
            blob.emplace_back("text", std::string(
                                          static_cast<std::size_t>(
                                              kDefaultToolBridgeLimits.max_result_bytes) + 1,
                                          'x'));
            return JsonValue(std::move(blob));
        }));
    auto chatty = proposal_for(chatty_spec, parse_json(R"json({"value": 1})json").value());
    const auto oversized = registry.execute(chatty, context);
    MIRA_CHECK(oversized && oversized.value().failed);
    MIRA_CHECK(oversized.value().safe_error_summary.find("size limit") != std::string::npos);
    return 0;
}

int wait_tool_behavior() {
    BuiltinToolRegistry registry;
    auto registration = make_wait_tool();
    const auto spec = registration.spec;
    MIRA_CHECK(registry.register_tool(spec, registration.handler));

    const auto started = std::chrono::steady_clock::now();
    const auto outcome =
        registry.execute(proposal_for(spec, parse_json(R"json({"duration_ms": 60})json").value()),
                         plain_context());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    MIRA_CHECK(outcome && !outcome.value().failed);
    MIRA_CHECK(outcome.value().result.find("waited_ms") != nullptr);
    MIRA_CHECK(elapsed.count() >= 50);

    // Out-of-range arguments are model-attributable failures, not dispatches.
    const auto over =
        registry.execute(proposal_for(spec, parse_json(R"json({"duration_ms": 99999})json").value()),
                         plain_context());
    MIRA_CHECK(over && over.value().failed);

    // Cancellation propagates as a Result error for the loop to settle on.
    OperationContext cancelled_context = plain_context();
    cancelled_context.cancellation_requested = [] { return true; };
    const auto cancelled = registry.execute(
        proposal_for(spec, parse_json(R"json({"duration_ms": 5000})json").value()),
        cancelled_context);
    MIRA_CHECK(!cancelled);
    MIRA_CHECK(cancelled.error().code == ErrorCode::Cancelled);
    return 0;
}

[[nodiscard]] AppendRequest conversation_event(const SessionId &session, std::string type,
                                               JsonValue detail) {
    JsonValue::Object envelope;
    envelope.emplace_back("task_id", TaskId::generate().to_string());
    envelope.emplace_back("detail", std::move(detail));
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = RuntimeId::generate();
    append.session_id = session;
    append.payload = EventPayload{std::move(type), to_json_string(JsonValue(std::move(envelope))),
                                  EventClass::State};
    return append;
}

int conversation_projection() {
    MemoryEventStore store;
    const auto session = SessionId::generate();

    // Empty sessions project to an empty view.
    const auto empty = build_conversation_view(store, session);
    MIRA_CHECK(empty && empty.value().empty());

    const auto message = conversation_event(
        session, "UserMessageInjected",
        JsonValue::Object{{"text", "use DingTalk instead"}, {"bytes", std::int64_t{21}}});
    const auto unrelated = conversation_event(session, "ActionDispatched",
                                               JsonValue::Object{{"action", "tap"}});
    const auto settled =
        conversation_event(session, "LoopSettled",
                           JsonValue::Object{{"outcome", "Completed"},
                                             {"steps", std::int64_t{2}},
                                             {"recoveries", std::int64_t{0}}});
    MIRA_CHECK(store.append(message).has_value());
    MIRA_CHECK(store.append(unrelated).has_value());
    MIRA_CHECK(store.append(settled).has_value());

    const auto view = build_conversation_view(store, session);
    MIRA_CHECK(view);
    MIRA_CHECK(view.value().size() == 2);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[0].text == "use DingTalk instead");
    MIRA_CHECK(view.value()[0].origin == message.event_id);
    MIRA_CHECK(view.value()[1].kind == ConversationEntry::Kind::LoopOutcome);
    MIRA_CHECK(view.value()[1].text.find("Completed") != std::string::npos);

    // Unreadable payloads fail closed instead of vanishing from the view.
    const auto corrupt = conversation_event(session, "UserMessageInjected",
                                            JsonValue::Object{{"bytes", std::int64_t{1}}});
    MIRA_CHECK(store.append(corrupt).has_value());
    MIRA_CHECK(!build_conversation_view(store, session));
    return 0;
}

} // namespace

int main() {
    if (const int code = register_rejects_bad_specs(); code != 0) {
        return code;
    }
    if (const int code = execute_fail_closed_paths(); code != 0) {
        return code;
    }
    if (const int code = wait_tool_behavior(); code != 0) {
        return code;
    }
    return conversation_projection();
}
