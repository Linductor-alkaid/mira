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
        request.generation.service_tier || request.data_policy.organization ||
        request.data_policy.project || !request.generation.max_output_tokens ||
        *request.generation.max_output_tokens == 0)
        return unsupported();
    for (const auto &item : request.input)
        for (const auto &part : item.content)
            if (const auto *image = std::get_if<ImagePart>(&part);
                image && image->detail != ImageDetail::Auto)
                return unsupported();
    // Reuse the verified canonical text/image/tool and generation validation;
    // only this intermediate representation is translated, never endpoints.
    if (request.generation.thinking && *request.generation.thinking != ThinkingMode::Disabled &&
        *request.generation.thinking != ThinkingMode::Adaptive)
        return unsupported();
    if (request.generation.thinking &&
        profile.capabilities.generation.thinking == ParamMapping::Unsupported)
        return unsupported();
    if (request.generation.reasoning_effort &&
        (profile.capabilities.generation.reasoning_effort == ParamMapping::Unsupported ||
         *request.generation.reasoning_effort == ReasoningEffort::Minimal))
        return unsupported();
    // Split assistant replay for common validation, then restore its exact block order.
    auto translated = request;
    translated.generation.thinking.reset();
    translated.generation.reasoning_effort.reset();
    translated.input.clear();
    std::vector<JsonValue::Array> assistant_groups;
    bool last_assistant = false;
    for (const auto &item : request.input) {
        if (item.role == ModelRole::Assistant) {
            if (!last_assistant)
                assistant_groups.emplace_back();
            last_assistant = true;
            for (const auto &part : item.content) {
                JsonValue block;
                if (const auto *thinking = std::get_if<ThinkingPart>(&part)) {
                    if (thinking->text.size() > 4 * 1024 * 1024 ||
                        thinking->signature.size() > 4 * 1024 * 1024 ||
                        (thinking->redacted && !thinking->signature.empty()))
                        return unsupported();
                    block = thinking->redacted
                                ? JsonValue{JsonValue::Object{{"type", "redacted_thinking"},
                                                              {"data", thinking->text}}}
                                : JsonValue{JsonValue::Object{{"type", "thinking"},
                                                              {"thinking", thinking->text},
                                                              {"signature", thinking->signature}}};
                } else {
                    auto single = item;
                    single.content = {part};
                    translated.input.push_back(std::move(single));
                    if (const auto *text = std::get_if<TextPart>(&part))
                        block = JsonValue::Object{{"type", "text"}, {"text", text->text}};
                    else if (const auto *call = std::get_if<ToolCallPart>(&part))
                        block = JsonValue::Object{{"type", "tool_use"},
                                                  {"id", call->provider_call_id.value},
                                                  {"name", call->wire_name},
                                                  {"input", call->arguments}};
                    else if (std::holds_alternative<ImagePart>(part))
                        block = JsonValue::Object{{"type", "validated_image"}};
                    else
                        return unsupported();
                }
                assistant_groups.back().push_back(std::move(block));
            }
            // Keep an assistant position even for a reasoning-only message.
            if (!item.content.empty() &&
                std::all_of(item.content.begin(), item.content.end(), [](const auto &part) {
                    return std::holds_alternative<ThinkingPart>(part);
                })) {
                auto placeholder = item;
                placeholder.content = {TextPart{""}};
                translated.input.push_back(std::move(placeholder));
            }
        } else {
            if (item.role != ModelRole::System && item.role != ModelRole::Developer)
                last_assistant = false;
            if (std::any_of(item.content.begin(), item.content.end(), [](const auto &part) {
                    return std::holds_alternative<ThinkingPart>(part);
                }))
                return unsupported();
            translated.input.push_back(item);
        }
    }
    auto common = ChatCompletionsV1Mapper{}.encode_request(translated, profile, stream, artifacts);
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
    std::size_t assistant_index = 0;
    for (auto &message : messages)
        if (*message.find("role")->as_string() == "assistant") {
            if (assistant_index >= assistant_groups.size())
                return unsupported();
            auto replay = assistant_groups[assistant_index++];
            const auto &validated = *message.find("content")->as_array();
            auto image = validated.begin();
            for (auto &block : replay)
                if (*block.find("type")->as_string() == "validated_image") {
                    image = std::find_if(image, validated.end(), [](const auto &part) {
                        return string(part, "type") && *string(part, "type") == "image";
                    });
                    if (image == validated.end())
                        return unsupported();
                    block = *image++;
                }
            message.set("content", std::move(replay));
        }
    if (request.generation.thinking)
        root.emplace_back("thinking", JsonValue::Object{{"type", *request.generation.thinking ==
                                                                         ThinkingMode::Adaptive
                                                                     ? "adaptive"
                                                                     : "disabled"}});
    if (request.generation.reasoning_effort) {
        const auto effort = *request.generation.reasoning_effort;
        const char *name = nullptr;
        switch (effort) {
        case ReasoningEffort::Low:
            name = "low";
            break;
        case ReasoningEffort::Medium:
            name = "medium";
            break;
        case ReasoningEffort::High:
            name = "high";
            break;
        case ReasoningEffort::XHigh:
            name = "xhigh";
            break;
        case ReasoningEffort::Max:
            name = "max";
            break;
        default:
            return unsupported();
        }
        root.emplace_back("output_config", JsonValue::Object{{"effort", name}});
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
        } else if (*kind == "thinking" || *kind == "redacted_thinking") {
            const bool redacted = *kind == "redacted_thinking";
            const auto *value = string(block, redacted ? "data" : "thinking");
            const auto *signature = string(block, "signature");
            if (!value || value->size() > 4 * 1024 * 1024 ||
                (!redacted && (!signature || signature->size() > 4 * 1024 * 1024)))
                return invalid();
        } else
            return unsupported();
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
    auto decoded = ChatCompletionsV1Mapper{}.decode_response(
        request, profile,
        WireHttpResponse{wire.status, wire.headers,
                         to_json_string(JsonValue{std::move(converted)})});
    if (!decoded)
        return decoded.error();
    // Normalization validates calls/finish status; preserve original assistant block order.
    auto result = std::move(decoded).value();
    auto normalized = std::move(result.output);
    result.output.clear();
    for (const auto &block : *content->as_array()) {
        const auto &kind = *string(block, "type");
        if (kind == "thinking" || kind == "redacted_thinking") {
            const bool redacted = kind == "redacted_thinking";
            result.output.emplace_back(ThinkingPart{*string(block, redacted ? "data" : "thinking"),
                                                    redacted ? "" : *string(block, "signature"),
                                                    redacted});
        } else if (kind == "text") {
            MessageOutput output_message;
            output_message.content.emplace_back(OutputTextPart{*string(block, "text"), {}});
            result.output.emplace_back(std::move(output_message));
        } else if (kind == "tool_use") {
            auto found = std::find_if(normalized.begin(), normalized.end(), [&](const auto &item) {
                const auto *call = std::get_if<ToolCallOutput>(&item);
                return call && call->provider_call_id.value == *string(block, "id");
            });
            if (found == normalized.end())
                return invalid();
            result.output.push_back(*found);
        }
    }
    // Preserve refusals generated by normalized stop status.
    for (const auto &item : normalized)
        if (std::holds_alternative<RefusalOutput>(item))
            result.output.push_back(item);
    return result;
}
} // namespace mira
