#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_tool.hpp>

#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

[[nodiscard]] std::pair<ModelRequest, ToolId> tool_request() {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = ModelProfileId::generate();
    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart text;
    text.text = "s";
    system_item.content.emplace_back(std::move(text));
    request.input = {std::move(system_item)};
    request.data_policy.store = false;
    const auto tool_id = ToolId::generate();
    request.tools.push_back(ExposedToolSpec{
        tool_id, SemanticVersion{2, 1, 0}, "lookup", "finds things",
        JsonSchema{parse_json(R"({"type":"object"})").value()}, Hash{}, true});
    return {std::move(request), tool_id};
}

[[nodiscard]] ModelResponse call_response(const ModelRequest &request,
                                          const std::string &call_id, const std::string &arguments,
                                          const std::string &name = "lookup") {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = "m";
    response.status = ModelCompletionStatus::Completed;
    ToolCallOutput call;
    call.provider_call_id = ProviderToolCallId{call_id};
    call.provider_name = name;
    for (const auto &tool : request.tools) {
        if (tool.wire_name == name) {
            call.tool_id = tool.tool_id;
        }
    }
    call.arguments = parse_json(arguments).value();
    call.arguments_digest = digest_string(to_json_string(call.arguments));
    response.output.emplace_back(std::move(call));
    response.usage.quality = UsageQuality::Missing;
    return response;
}

int resolves_and_derives_stable_operations() {
    auto [request, tool_id] = tool_request();
    const auto response = call_response(request, "call_1", R"({"q":"mira"})");
    auto batch = resolve_tool_calls(request, response);
    MIRA_CHECK(batch.has_value());
    MIRA_CHECK(batch.value().proposals.size() == 1);
    const auto &proposal = batch.value().proposals[0];
    MIRA_CHECK(proposal.tool_id == tool_id);
    MIRA_CHECK(proposal.wire_name == "lookup");
    MIRA_CHECK(proposal.has_side_effects);
    MIRA_CHECK(!proposal.operation_id.value.is_nil());
    const SemanticVersion expected_version{2, 1, 0};
    MIRA_CHECK(proposal.tool_version == expected_version);

    // Stable: the same inputs derive the same operation ID.
    auto again = resolve_tool_calls(request, response);
    MIRA_CHECK(again.value().proposals[0].operation_id == proposal.operation_id);
    // Different arguments derive a different operation ID.
    auto other = resolve_tool_calls(request, call_response(request, "call_1", R"({"q":"beta"})"));
    MIRA_CHECK(other.value().proposals[0].operation_id != proposal.operation_id);
    return 0;
}

int fails_closed_on_hosted_and_unknown_tools() {
    auto [request, tool_id] = tool_request();
    // A hosted web search call is a protocol violation, never a capability.
    auto hosted = resolve_tool_calls(request, call_response(request, "c1", "{}", "web_search"));
    MIRA_CHECK(!hosted.has_value());
    MIRA_CHECK(hosted.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));

    auto computer = resolve_tool_calls(request,
                                       call_response(request, "c1", "{}", "computer_use_preview"));
    MIRA_CHECK(!computer.has_value());

    auto unknown = resolve_tool_calls(request, call_response(request, "c1", "{}", "mystery"));
    MIRA_CHECK(!unknown.has_value());

    // Identity mismatch between name and resolved tool id.
    auto response = call_response(request, "c1", "{}");
    auto &call = std::get<ToolCallOutput>(response.output[0]);
    call.tool_id = ToolId::generate();
    auto mismatch = resolve_tool_calls(request, response);
    MIRA_CHECK(!mismatch.has_value());

    // Digest mismatch is a violation.
    auto digest_bad = call_response(request, "c1", R"({"q":"x"})");
    std::get<ToolCallOutput>(digest_bad.output[0]).arguments_digest = Sha256Digest{};
    MIRA_CHECK(!resolve_tool_calls(request, digest_bad).has_value());
    static_cast<void>(tool_id);
    return 0;
}

int duplicate_call_ids_collapse_or_reject() {
    auto [request, tool_id] = tool_request();
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = "m";
    response.status = ModelCompletionStatus::Completed;
    response.usage.quality = UsageQuality::Missing;
    for (int index = 0; index < 2; ++index) {
        response.output.push_back(
            std::get<ToolCallOutput>(call_response(request, "dup", R"({"a":1})").output[0]));
    }
    auto collapsed = resolve_tool_calls(request, response);
    MIRA_CHECK(collapsed.has_value());
    MIRA_CHECK(collapsed.value().proposals.size() == 1);
    MIRA_CHECK(collapsed.value().proposals[0].deduplicated);
    MIRA_CHECK(!collapsed.value().diagnostics.empty());

    // Same id, different arguments: conflict, not a best guess.
    auto conflict_response = response;
    std::get<ToolCallOutput>(conflict_response.output[1]) =
        std::get<ToolCallOutput>(call_response(request, "dup", R"({"a":2})").output[0]);
    auto conflict = resolve_tool_calls(request, conflict_response);
    MIRA_CHECK(!conflict.has_value());
    MIRA_CHECK(conflict.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::ProtocolViolation));

    // Batch limits apply.
    ModelResponse many;
    many.contract_version = SchemaVersion{1, 0};
    many.requested_model = "m";
    many.status = ModelCompletionStatus::Completed;
    many.usage.quality = UsageQuality::Missing;
    ToolBridgeLimits limits;
    limits.max_calls_per_response = 4;
    for (int index = 0; index < 5; ++index) {
        many.output.push_back(
            std::get<ToolCallOutput>(call_response(request, "c" + std::to_string(index), "{}").output[0]));
    }
    MIRA_CHECK(!resolve_tool_calls(request, many, limits).has_value());
    return 0;
}

int result_backfill_wire_items() {
    auto [request, tool_id] = tool_request();
    ToolExecutionRecord record;
    record.provider_call_id = ProviderToolCallId{"call_1"};
    record.tool_id = tool_id;
    record.result = parse_json(R"({"rows":3})").value();

    // The nested output document is serialized inside a string field; parse
    // it back out before asserting on its content.
    const auto parse_output = [](const JsonValue &item) {
        const auto *output = item.find("output");
        if (output == nullptr || !output->is_string()) {
            return JsonValue::Object{};
        }
        auto parsed = parse_json(*output->as_string());
        return parsed.has_value() && parsed.value().is_object()
                   ? *parsed.value().as_object()
                   : JsonValue::Object{};
    };
    auto responses_items = build_tool_result_input(
        ProtocolDialect::OpenAIResponsesV1, std::vector<ToolExecutionRecord>{record});
    MIRA_CHECK(responses_items.has_value());
    const auto responses_text = to_json_string(responses_items.value()[0]);
    MIRA_CHECK(responses_text.find("\"type\":\"function_call_output\"") != std::string::npos);
    MIRA_CHECK(responses_text.find("\"call_id\":\"call_1\"") != std::string::npos);
    const auto responses_output = parse_output(responses_items.value()[0]);
    MIRA_CHECK(responses_output[0].first == "status");
    MIRA_CHECK(responses_output[0].second.as_string() != nullptr &&
               *responses_output[0].second.as_string() == "ok");

    auto chat_items = build_tool_result_input(
        ProtocolDialect::OpenAIChatCompletionsV1, std::vector<ToolExecutionRecord>{record});
    MIRA_CHECK(chat_items.has_value());
    const auto chat_text = to_json_string(chat_items.value()[0]);
    MIRA_CHECK(chat_text.find("\"role\":\"tool\"") != std::string::npos);
    MIRA_CHECK(chat_text.find("\"tool_call_id\":\"call_1\"") != std::string::npos);

    // Failed tools backfill structured errors, never silent drops.
    ToolExecutionRecord failed = record;
    failed.failed = true;
    failed.safe_error_summary = "permission denied";
    auto failed_items = build_tool_result_input(
        ProtocolDialect::OpenAIResponsesV1, std::vector<ToolExecutionRecord>{failed});
    MIRA_CHECK(failed_items.has_value());
    const auto failed_output = parse_output(failed_items.value()[0]);
    MIRA_CHECK(failed_output[0].second.as_string() != nullptr &&
               *failed_output[0].second.as_string() == "error");

    // Oversize payloads must reference an artifact, not inline bytes. The
    // payload is built directly (not parsed) so parser string limits stay
    // out of this check.
    ToolExecutionRecord large = record;
    std::string big(1'200'000, 'x');
    large.result = JsonValue(JsonValue::Object{{"blob", JsonValue(big)}});
    MIRA_CHECK(!build_tool_result_input(ProtocolDialect::OpenAIResponsesV1,
                                        std::vector<ToolExecutionRecord>{large})
                    .has_value());
    large.large_payload = ArtifactRef{ArtifactId::generate(), digest_string("x"), 4096,
                                      "application/octet-stream", Sensitivity::Internal};
    auto with_artifact = build_tool_result_input(
        ProtocolDialect::OpenAIResponsesV1, std::vector<ToolExecutionRecord>{large});
    MIRA_CHECK(with_artifact.has_value());
    const auto artifact_output = parse_output(with_artifact.value()[0]);
    const auto payload_text = to_json_string(JsonValue(artifact_output));
    MIRA_CHECK(payload_text.find("artifact_id") != std::string::npos);
    return 0;
}

} // namespace

int main() {
    if (const int status = resolves_and_derives_stable_operations(); status != 0) {
        return status;
    }
    if (const int status = fails_closed_on_hosted_and_unknown_tools(); status != 0) {
        return status;
    }
    if (const int status = duplicate_call_ids_collapse_or_reject(); status != 0) {
        return status;
    }
    if (const int status = result_backfill_wire_items(); status != 0) {
        return status;
    }
    return 0;
}
