#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <set>

namespace mira {
namespace {
Error unsupported() {
    return make_model_error(ModelDomainCode::CapabilityMismatch,
                            "Messages field or content has no verified canonical mapping");
}
Error invalid() {
    return make_model_error(ModelDomainCode::ProtocolViolation, "invalid Messages response");
}
const std::string *string(const JsonValue &object, const char *key) {
    const auto *value = object.find(key);
    return value ? value->as_string() : nullptr;
}
} // namespace

Result<JsonValue> AnthropicMessagesV1Mapper::encode_request(const ModelRequest &request,
                                                            const ModelProfile &profile,
                                                            bool stream,
                                                            IArtifactSource &artifacts) const {
    if (request.output_contract.mode != OutputMode::Text || request.generation.seed ||
        request.generation.reasoning_effort || request.generation.service_tier ||
        request.data_policy.organization || request.data_policy.project ||
        !request.generation.max_output_tokens || *request.generation.max_output_tokens == 0)
        return unsupported();
    for (const auto &item : request.input)
        for (const auto &part : item.content)
            if (const auto *image = std::get_if<ImagePart>(&part);
                image && image->detail != ImageDetail::Auto)
                return unsupported();
    // Reuse the verified canonical text/image/tool and generation validation;
    // only this intermediate representation is translated, never endpoints.
    auto common = ChatCompletionsV1Mapper{}.encode_request(request, profile, stream, artifacts);
    if (!common)
        return common.error();
    JsonValue::Object root{
        {"model", profile.model_selector},
        {"max_tokens", static_cast<std::int64_t>(*request.generation.max_output_tokens)}};
    for (const char *key : {"temperature", "top_p", "stream"})
        if (const auto *field = common.value().find(key))
            root.emplace_back(key, *field);
    JsonValue::Array messages, system;
    for (const auto &message : *common.value().find("messages")->as_array()) {
        const auto &role = *message.find("role")->as_string();
        JsonValue::Array blocks;
        if (role == "tool") {
            blocks.emplace_back(JsonValue::Object{{"type", "tool_result"},
                                                  {"tool_use_id", *message.find("tool_call_id")},
                                                  {"content", *message.find("content")}});
            auto output = parse_json(*message.find("content")->as_string());
            if (output && string(output.value(), "status") &&
                *string(output.value(), "status") == "error")
                blocks.back().set("is_error", true);
        } else if (const auto *content = message.find("content"); content && !content->is_null()) {
            if (const auto *text = content->as_string()) {
                if (!text->empty())
                    blocks.emplace_back(JsonValue::Object{{"type", "text"}, {"text", *text}});
            } else {
                for (const auto &part : *content->as_array()) {
                    if (*part.find("type")->as_string() == "text")
                        blocks.push_back(part);
                    else {
                        const auto &url = *part.find("image_url")->find("url")->as_string();
                        const auto delimiter = url.find(";base64,");
                        if (delimiter == std::string::npos)
                            return unsupported();
                        blocks.emplace_back(JsonValue::Object{
                            {"type", "image"},
                            {"source",
                             JsonValue::Object{{"type", "base64"},
                                               {"media_type", url.substr(5, delimiter - 5)},
                                               {"data", url.substr(delimiter + 8)}}}});
                    }
                }
            }
        }
        if (const auto *calls = message.find("tool_calls"))
            for (const auto &call : *calls->as_array()) {
                const auto *function = call.find("function");
                auto args = parse_json(*function->find("arguments")->as_string());
                if (!args || !args.value().is_object())
                    return unsupported();
                blocks.emplace_back(JsonValue::Object{{"type", "tool_use"},
                                                      {"id", *call.find("id")},
                                                      {"name", *function->find("name")},
                                                      {"input", std::move(args).value()}});
            }
        if (role == "system" || role == "developer") {
            for (const auto &block : blocks) {
                if (string(block, "type") == nullptr || *string(block, "type") != "text")
                    return unsupported();
                system.push_back(block);
            }
        } else {
            const std::string wire_role = role == "assistant" ? "assistant" : "user";
            // Consecutive tool results form a single user content block list.
            if (!messages.empty() && *messages.back().find("role")->as_string() == wire_role) {
                auto parts = *messages.back().find("content")->as_array();
                parts.insert(parts.end(), blocks.begin(), blocks.end());
                messages.back().set("content", std::move(parts));
            } else
                messages.emplace_back(
                    JsonValue::Object{{"role", wire_role}, {"content", std::move(blocks)}});
        }
    }
    if (!system.empty())
        root.emplace_back("system", std::move(system));
    root.emplace_back("messages", std::move(messages));
    if (const auto *tools = common.value().find("tools")) {
        JsonValue::Array converted;
        for (const auto &tool : *tools->as_array()) {
            const auto &function = *tool.find("function");
            converted.emplace_back(
                JsonValue::Object{{"name", *function.find("name")},
                                  {"description", *function.find("description")},
                                  {"input_schema", *function.find("parameters")}});
        }
        root.emplace_back("tools", std::move(converted));
        const auto *choice = common.value().find("tool_choice");
        if (choice->is_string()) {
            const auto &type = *choice->as_string();
            root.emplace_back("tool_choice",
                              JsonValue::Object{{"type", type == "required" ? "any" : type}});
        } else
            root.emplace_back("tool_choice",
                              JsonValue::Object{{"type", "tool"},
                                                {"name", *choice->find("function")->find("name")}});
    }
    return JsonValue{std::move(root)};
}

Result<ModelResponse>
AnthropicMessagesV1Mapper::decode_response(const ModelRequest &request, const ModelProfile &profile,
                                           const WireHttpResponse &wire) const {
    if (wire.status < 200 || wire.status >= 300)
        return map_http_error_status(wire);
    auto parsed = parse_json(wire.body);
    if (!parsed || !parsed.value().is_object())
        return invalid();
    const auto &body = parsed.value();
    const auto *id = string(body, "id"), *model = string(body, "model"),
               *type = string(body, "type"), *role = string(body, "role"),
               *stop = string(body, "stop_reason");
    const auto *content = body.find("content");
    if (!id || id->empty() || id->size() > 1024 || !model || model->empty() ||
        model->size() > 1024 || !type || *type != "message" || !role || *role != "assistant" ||
        !stop || !content || !content->is_array() || content->as_array()->size() > 64)
        return invalid();
    std::string finish, text;
    if (*stop == "end_turn" || *stop == "stop_sequence")
        finish = "stop";
    else if (*stop == "tool_use")
        finish = "tool_calls";
    else if (*stop == "max_tokens")
        finish = "length";
    else if (*stop == "refusal")
        finish = "content_filter";
    else
        return unsupported();
    JsonValue::Array calls;
    std::set<std::string> call_ids;
    for (const auto &block : *content->as_array()) {
        const auto *kind = string(block, "type");
        if (!kind)
            return invalid();
        if (*kind == "text") {
            const auto *part = string(block, "text");
            if (!part || part->size() > 4 * 1024 * 1024 - text.size())
                return invalid();
            text += *part;
        } else if (*kind == "tool_use") {
            const auto *call_id = string(block, "id"), *name = string(block, "name");
            const auto *input = block.find("input");
            if (!call_id || call_id->empty() || call_id->size() > 1024 || !name || name->empty() ||
                name->size() > 1024 || !input || !input->is_object() ||
                !call_ids.insert(*call_id).second)
                return invalid();
            calls.emplace_back(JsonValue::Object{
                {"id", *call_id},
                {"type", "function"},
                {"function",
                 JsonValue::Object{{"name", *name}, {"arguments", to_json_string(*input)}}}});
        } else
            return unsupported(); // Signed thinking must never be silently discarded.
    }
    if ((finish == "tool_calls") != !calls.empty() && finish != "length")
        return invalid();
    JsonValue::Object message{{"role", "assistant"}, {"content", text}};
    if (!calls.empty())
        message.emplace_back("tool_calls", std::move(calls));
    JsonValue::Object converted{
        {"id", *id},
        {"model", *model},
        {"choices",
         JsonValue::Array{JsonValue::Object{
             {"index", 0}, {"message", std::move(message)}, {"finish_reason", finish}}}}};
    if (const auto *usage = body.find("usage")) {
        if (!usage->is_object())
            return invalid();
        for (const char *key : {"input_tokens", "output_tokens", "cache_read_input_tokens",
                                "cache_creation_input_tokens"}) {
            if (const auto *count = usage->find(key);
                count && (!count->as_integer() || *count->as_integer() < 0))
                return invalid();
        }
        auto mapped = *usage;
        if (const auto *cached = usage->find("cache_read_input_tokens"))
            mapped.set("input_tokens_details", JsonValue::Object{{"cached_tokens", *cached}});
        // Anthropic input_tokens excludes cache reads/creation. Context accounting
        // needs the total current input, not only the uncached suffix.
        std::uint64_t total = 0;
        for (const char *key :
             {"input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"})
            if (const auto *count = usage->find(key)) {
                const auto tokens = static_cast<std::uint64_t>(*count->as_integer());
                if (tokens > static_cast<std::uint64_t>(INT64_MAX) - total)
                    return invalid();
                total += tokens;
            }
        if (usage->find("input_tokens"))
            mapped.set("input_tokens", JsonValue{static_cast<std::int64_t>(total)});
        converted.emplace_back("usage", std::move(mapped));
    }
    return ChatCompletionsV1Mapper{}.decode_response(
        request, profile,
        WireHttpResponse{wire.status, wire.headers,
                         to_json_string(JsonValue{std::move(converted)})});
}
} // namespace mira
