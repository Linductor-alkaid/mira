#include "model_tool_wire.hpp"

#include <algorithm>

namespace mira::tool_wire {
namespace {

[[nodiscard]] Error wire_error(std::string message) {
    return make_model_error(ModelDomainCode::InvalidModelRequest, std::move(message), false,
                            std::nullopt);
}

} // namespace

ItemShape classify_item(const ModelInputItem &item) {
    bool calls = false;
    bool results = false;
    for (const auto &part : item.content) {
        if (std::get_if<ToolCallPart>(&part) != nullptr) {
            calls = true;
        } else if (std::get_if<ToolResultPart>(&part) != nullptr) {
            results = true;
        } else if (calls || results) {
            return ItemShape::Mixed;
        }
    }
    if (calls && results) {
        return ItemShape::Mixed;
    }
    if (calls) {
        return ItemShape::CallsOnly;
    }
    if (results) {
        return ItemShape::ResultsOnly;
    }
    return ItemShape::Ordinary;
}

Result<std::vector<JsonValue>> encode_calls(const ModelInputItem &item, ProtocolDialect dialect) {
    std::vector<JsonValue> encoded;
    if (dialect == ProtocolDialect::AnthropicMessagesV1) {
        JsonValue::Array blocks;
        for (const auto &part : item.content) {
            const auto *call = std::get_if<ToolCallPart>(&part);
            if (!call)
                return wire_error("tool call item mixes other content");
            blocks.emplace_back(JsonValue::Object{{"type", "tool_use"},
                                                  {"id", call->provider_call_id.value},
                                                  {"name", call->wire_name},
                                                  {"input", call->arguments}});
        }
        encoded.emplace_back(
            JsonValue::Object{{"role", "assistant"}, {"content", std::move(blocks)}});
        return encoded;
    }
    if (dialect == ProtocolDialect::OpenAIChatCompletionsV1) {
        JsonValue::Object message;
        message.emplace_back("role", "assistant");
        message.emplace_back("content", JsonValue());
        JsonValue::Array calls;
        for (const auto &part : item.content) {
            const auto *call = std::get_if<ToolCallPart>(&part);
            if (call == nullptr) {
                return wire_error("tool call items must not mix other content");
            }
            JsonValue::Object entry;
            entry.emplace_back("id", call->provider_call_id.value);
            entry.emplace_back("type", "function");
            JsonValue::Object function;
            function.emplace_back("name", call->wire_name);
            function.emplace_back("arguments", to_json_string(call->arguments));
            entry.emplace_back("function", std::move(function));
            calls.emplace_back(std::move(entry));
        }
        message.emplace_back("tool_calls", std::move(calls));
        encoded.emplace_back(std::move(message));
        return encoded;
    }
    for (const auto &part : item.content) {
        const auto *call = std::get_if<ToolCallPart>(&part);
        if (call == nullptr) {
            return wire_error("tool call items must not mix other content");
        }
        JsonValue::Object item_json;
        item_json.emplace_back("type", "function_call");
        item_json.emplace_back("call_id", call->provider_call_id.value);
        item_json.emplace_back("name", call->wire_name);
        item_json.emplace_back("arguments", to_json_string(call->arguments));
        encoded.emplace_back(std::move(item_json));
    }
    return encoded;
}

Result<std::vector<JsonValue>> encode_results(const ModelInputItem &item, ProtocolDialect dialect) {
    std::vector<ToolExecutionRecord> records;
    records.reserve(item.content.size());
    for (const auto &part : item.content) {
        const auto *result = std::get_if<ToolResultPart>(&part);
        if (result == nullptr) {
            return wire_error("tool result items must not mix other content");
        }
        ToolExecutionRecord record;
        record.provider_call_id = result->provider_call_id;
        record.tool_id = result->tool_id;
        record.result = result->result;
        record.large_payload = result->large_payload;
        record.failed = result->failed;
        record.safe_error_summary = result->safe_error_summary;
        records.push_back(std::move(record));
    }
    return build_tool_result_input(dialect, records);
}

} // namespace mira::tool_wire
