#include <mira/model_tool.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace mira {

const ToolBridgeLimits kDefaultToolBridgeLimits{};

namespace {

[[nodiscard]] Error bridge_error(ModelDomainCode code, std::string message) {
    return make_model_error(code, std::move(message), false, std::nullopt);
}

} // namespace

bool is_known_hosted_tool_name(std::string_view wire_name) {
    static const std::array<std::string_view, 12> kHosted = {
        "web_search", "web_search_preview", "file_search", "code_interpreter",
        "computer_use_preview", "computer-use", "mcp", "bash", "shell", "terminal",
        "image_generation", "canvas",
    };
    return std::find(kHosted.begin(), kHosted.end(), wire_name) != kHosted.end();
}

OperationId derive_tool_operation_id(const ModelRequestId &request_id,
                                    const ProviderToolCallId &call_id, const ToolId &tool_id,
                                    const Hash &arguments_digest) {
    JsonValue::Object root;
    root.emplace_back("request_id", request_id.to_string());
    root.emplace_back("provider_call_id", call_id.value);
    root.emplace_back("tool_id", tool_id.to_string());
    root.emplace_back("arguments_digest", arguments_digest.to_string());
    const auto digest = canonical_json_digest(JsonValue(std::move(root)));
    Id128::Bytes bytes{};
    std::copy(digest.bytes.begin(), digest.bytes.begin() + 16, bytes.begin());
    return OperationId{Id128(bytes)};
}

Result<ToolProposalBatch> resolve_tool_calls(const ModelRequest &request,
                                             const ModelResponse &response,
                                             const ToolBridgeLimits &limits) {
    ToolProposalBatch batch;
    std::vector<const ToolCallOutput *> calls;
    for (const auto &item : response.output) {
        if (const auto *call = std::get_if<ToolCallOutput>(&item)) {
            calls.push_back(call);
        }
    }
    if (calls.empty()) {
        return batch;
    }
    if (calls.size() > limits.max_calls_per_response) {
        return bridge_error(ModelDomainCode::ProtocolViolation,
                            "tool call batch exceeds the per-response limit");
    }
    for (const auto *call : calls) {
        const auto exposed = std::find_if(
            request.tools.begin(), request.tools.end(),
            [&](const ExposedToolSpec &tool) { return tool.wire_name == call->provider_name; });
        if (exposed == request.tools.end()) {
            if (is_known_hosted_tool_name(call->provider_name)) {
                return bridge_error(ModelDomainCode::ProtocolViolation,
                                    "hosted provider tool was not requested: " + call->provider_name);
            }
            return bridge_error(ModelDomainCode::ProtocolViolation,
                                "tool call references a tool that was not exposed");
        }
        if (call->tool_id != exposed->tool_id) {
            return bridge_error(ModelDomainCode::ProtocolViolation,
                                "tool call identity does not match the exposed tool");
        }
        if (!call->arguments.is_object()) {
            return bridge_error(ModelDomainCode::ProtocolViolation,
                                "tool arguments were not a complete JSON object");
        }
        const auto arguments_text = to_json_string(call->arguments);
        if (arguments_text.size() > limits.max_arguments_bytes) {
            return bridge_error(ModelDomainCode::ResponseTooLarge,
                                "tool arguments exceed the size limit");
        }
        const auto digest = digest_string(arguments_text);
        if (digest != call->arguments_digest) {
            return bridge_error(ModelDomainCode::ProtocolViolation,
                                "tool arguments digest does not match the payload");
        }

        // Duplicate call IDs: identical digest collapses, differing digest is a
        // protocol violation rather than a "best guess" pick.
        const auto existing = std::find_if(
            batch.proposals.begin(), batch.proposals.end(),
            [&](const ToolProposal &proposal) {
                return proposal.provider_call_id == call->provider_call_id;
            });
        if (existing != batch.proposals.end()) {
            if (existing->arguments_digest == digest) {
                existing->deduplicated = true;
                batch.diagnostics.push_back(
                    ToolBridgeDiagnostic{call->provider_call_id, "duplicate call collapsed"});
                continue;
            }
            return bridge_error(ModelDomainCode::ProtocolViolation,
                                "duplicate tool call id carries different arguments");
        }

        ToolProposal proposal;
        proposal.provider_call_id = call->provider_call_id;
        proposal.tool_id = exposed->tool_id;
        proposal.wire_name = exposed->wire_name;
        proposal.tool_version = exposed->version;
        proposal.arguments = call->arguments;
        proposal.arguments_digest = digest;
        proposal.operation_id =
            derive_tool_operation_id(request.request_id, call->provider_call_id, exposed->tool_id,
                                     digest);
        proposal.has_side_effects = exposed->has_side_effects;
        batch.proposals.push_back(std::move(proposal));
    }
    return batch;
}

Result<std::vector<JsonValue>> build_tool_result_input(
    ProtocolDialect dialect, std::span<const ToolExecutionRecord> records) {
    std::vector<JsonValue> items;
    for (const auto &record : records) {
        if (record.provider_call_id.value.empty()) {
            return bridge_error(ModelDomainCode::InvalidModelRequest,
                                "tool result record requires a provider call id");
        }
        const auto result_size = to_json_string(record.result).size();
        if (result_size > kDefaultToolBridgeLimits.max_result_bytes &&
            !record.large_payload.has_value()) {
            return bridge_error(ModelDomainCode::ResponseTooLarge,
                                "tool result payload must be stored as an artifact");
        }
        JsonValue::Object output;
        if (record.failed) {
            output.emplace_back("status", "error");
            output.emplace_back("error", record.safe_error_summary);
        } else {
            output.emplace_back("status", "ok");
            if (record.large_payload.has_value()) {
                // Large payloads are replaced by their artifact reference;
                // inline bytes never travel back to the provider.
                JsonValue::Object reference;
                reference.emplace_back("artifact_id", record.large_payload->id.to_string());
                reference.emplace_back("media_type", record.large_payload->media_type);
                reference.emplace_back("byte_size",
                                       static_cast<std::int64_t>(record.large_payload->byte_size));
                output.emplace_back("payload", std::move(reference));
            } else {
                output.emplace_back("result", record.result);
            }
        }
        const auto serialized = to_json_string(JsonValue(std::move(output)));
        switch (dialect) {
        case ProtocolDialect::OpenAIResponsesV1: {
            JsonValue::Object item;
            item.emplace_back("type", "function_call_output");
            item.emplace_back("call_id", record.provider_call_id.value);
            item.emplace_back("output", serialized);
            items.emplace_back(std::move(item));
            break;
        }
        case ProtocolDialect::OpenAIChatCompletionsV1: {
            JsonValue::Object item;
            item.emplace_back("role", "tool");
            item.emplace_back("tool_call_id", record.provider_call_id.value);
            item.emplace_back("content", serialized);
            items.emplace_back(std::move(item));
            break;
        }
        }
    }
    return items;
}

} // namespace mira
