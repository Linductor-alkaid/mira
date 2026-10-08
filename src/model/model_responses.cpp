#include <mira/model_dialect.hpp>

#include "model_dialect_internal.hpp"
#include "model_tool_wire.hpp"

#include <algorithm>

namespace mira {

// ---------------------------------------------------------------------------
// openai.responses.v1
// ---------------------------------------------------------------------------

Result<JsonValue> ResponsesV1Mapper::encode_request(const ModelRequest &request,
                                                    const ModelProfile &profile, bool stream,
                                                    IArtifactSource &artifacts) const {
    for (const auto &item : request.input)
        for (const auto &part : item.content)
            if (std::holds_alternative<ThinkingPart>(part))
                return make_model_error(ModelDomainCode::CapabilityMismatch,
                                        "thinking replay requires Messages mapper");
    if (auto gate = gate_generation_options(profile, request.generation); !gate) {
        return gate.error();
    }
    JsonValue::Object root;
    root.emplace_back("model", profile.model_selector);

    JsonValue::Array input;
    for (const auto &item : request.input) {
        // Canonical tool round-trip items (DEC-047) render as native
        // function_call / function_call_output items, not role+content.
        switch (tool_wire::classify_item(item)) {
        case tool_wire::ItemShape::Mixed:
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "tool call/result parts must not mix with other content");
        case tool_wire::ItemShape::CallsOnly:
        case tool_wire::ItemShape::ResultsOnly: {
            if (!profile.capabilities.function_tools.supported) {
                return make_model_error(ModelDomainCode::CapabilityMismatch,
                                        "profile does not support function tools");
            }
            const auto dialect = ProtocolDialect::OpenAIResponsesV1;
            auto encoded = tool_wire::classify_item(item) == tool_wire::ItemShape::CallsOnly
                               ? tool_wire::encode_calls(item, dialect)
                               : tool_wire::encode_results(item, dialect);
            if (!encoded) {
                return encoded.error();
            }
            for (auto &wire : encoded.value()) {
                input.emplace_back(std::move(wire));
            }
            continue;
        }
        case tool_wire::ItemShape::Ordinary:
            break;
        }
        JsonValue::Object item_json;
        switch (item.role) {
        case ModelRole::System:
            item_json.emplace_back("role", "system");
            break;
        case ModelRole::Developer:
            item_json.emplace_back("role", "developer");
            break;
        case ModelRole::User:
            item_json.emplace_back("role", "user");
            break;
        case ModelRole::Assistant:
            item_json.emplace_back("role", "assistant");
            break;
        case ModelRole::Unknown:
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "input items must carry a known role");
        }
        JsonValue::Array parts;
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                JsonValue::Object part_json;
                part_json.emplace_back("type", "input_text");
                part_json.emplace_back("text", text->text);
                parts.emplace_back(std::move(part_json));
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                if (!profile.capabilities.image_input.supported) {
                    return make_model_error(ModelDomainCode::CapabilityMismatch,
                                            "profile does not accept image input");
                }
                if (const auto gate = require_image_media_type(image->source); !gate) {
                    return gate.error();
                }
                JsonValue::Object part_json;
                part_json.emplace_back("type", "input_image");
                if (auto file_id = artifacts.remote_file_id(image->source); file_id.has_value()) {
                    part_json.emplace_back("file_id", *file_id);
                } else {
                    auto data_url = fetch_image_data_url(image->source, artifacts);
                    if (!data_url) {
                        return data_url.error();
                    }
                    part_json.emplace_back("image_url", std::move(data_url).value());
                }
                switch (image->detail) {
                case ImageDetail::Low:
                    part_json.emplace_back("detail", "low");
                    break;
                case ImageDetail::High:
                    part_json.emplace_back("detail", "high");
                    break;
                case ImageDetail::Auto:
                case ImageDetail::Original:
                    part_json.emplace_back("detail", "auto");
                    break;
                }
                parts.emplace_back(std::move(part_json));
            } else if (const auto *file = std::get_if<FilePart>(&part)) {
                if (!profile.capabilities.file_input.supported) {
                    return make_model_error(ModelDomainCode::CapabilityMismatch,
                                            "profile does not support file input");
                }
                JsonValue::Object part_json;
                part_json.emplace_back("type", "input_file");
                if (auto file_id = artifacts.remote_file_id(file->source); file_id.has_value()) {
                    part_json.emplace_back("file_id", *file_id);
                } else {
                    auto data_url = fetch_image_data_url(file->source, artifacts);
                    if (!data_url) {
                        return data_url.error();
                    }
                    part_json.emplace_back("file_data", std::move(data_url).value());
                    part_json.emplace_back("filename", file->display_name);
                }
                parts.emplace_back(std::move(part_json));
            }
        }
        item_json.emplace_back("content", std::move(parts));
        input.emplace_back(std::move(item_json));
    }
    if (request.continuation.has_value() &&
        request.continuation->previous_response_id.has_value()) {
        root.emplace_back("previous_response_id", *request.continuation->previous_response_id);
    }
    root.emplace_back("input", std::move(input));

    switch (request.output_contract.mode) {
    case OutputMode::StrictJsonSchema:
    case OutputMode::JsonObject: {
        if (request.output_contract.mode == OutputMode::StrictJsonSchema &&
            !profile.capabilities.strict_json_schema.supported) {
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "profile does not support strict json schema output");
        }
        JsonValue::Object format;
        format.emplace_back("type", request.output_contract.mode == OutputMode::StrictJsonSchema
                                        ? "json_schema"
                                        : "json_object");
        if (request.output_contract.mode == OutputMode::StrictJsonSchema) {
            JsonValue::Object schema_format;
            schema_format.emplace_back("name", "decision");
            schema_format.emplace_back("strict", true);
            schema_format.emplace_back("schema", request.output_contract.schema.root);
            format.emplace_back("json_schema", std::move(schema_format));
        }
        root.emplace_back("text", JsonValue::Object{{"format", std::move(format)}});
        break;
    }
    case OutputMode::StrictFunctionTool:
    case OutputMode::Text:
        // Tool-driven and text modes declare no text format.
        break;
    }

    if (!request.tools.empty()) {
        if (!profile.capabilities.function_tools.supported) {
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "profile does not support function tools");
        }
        JsonValue::Array tools;
        for (const auto &tool : request.tools) {
            JsonValue::Object tool_json;
            tool_json.emplace_back("type", "function");
            tool_json.emplace_back("name", tool.wire_name);
            tool_json.emplace_back("description", tool.description);
            tool_json.emplace_back("parameters", tool.parameters_schema.root);
            tool_json.emplace_back("strict", true);
            tools.emplace_back(std::move(tool_json));
        }
        root.emplace_back("tools", std::move(tools));
        switch (request.tool_choice.mode) {
        case ToolChoiceMode::Auto:
            root.emplace_back("tool_choice", "auto");
            break;
        case ToolChoiceMode::None:
            root.emplace_back("tool_choice", "none");
            break;
        case ToolChoiceMode::Required:
            // `required` is never silently downgraded to `auto`.
            root.emplace_back("tool_choice", "required");
            break;
        case ToolChoiceMode::Named: {
            const auto found = std::find_if(
                request.tools.begin(), request.tools.end(), [&](const ExposedToolSpec &tool) {
                    return tool.tool_id == request.tool_choice.required_tool;
                });
            if (found == request.tools.end()) {
                return make_model_error(ModelDomainCode::InvalidModelRequest,
                                        "named tool choice was not exposed");
            }
            JsonValue::Object choice;
            choice.emplace_back("type", "function");
            choice.emplace_back("name", found->wire_name);
            root.emplace_back("tool_choice", std::move(choice));
            break;
        }
        }
    }

    if (request.generation.max_output_tokens.has_value()) {
        root.emplace_back("max_output_tokens",
                          static_cast<std::int64_t>(*request.generation.max_output_tokens));
    }
    if (request.generation.temperature.has_value()) {
        root.emplace_back("temperature", *request.generation.temperature);
    }
    if (request.generation.top_p.has_value()) {
        root.emplace_back("top_p", *request.generation.top_p);
    }

    // `store` is always explicit; the provider default is never relied upon.
    root.emplace_back("store", request.data_policy.store.value_or(false));
    if (request.data_policy.organization.has_value()) {
        // Org/project selection is a header-level concern handled by the
        // transport; the body never carries it.
    }
    root.emplace_back("stream", stream);
    return JsonValue(std::move(root));
}

Result<ModelResponse> decode_responses_terminal_body(const ModelRequest &request,
                                                     const ModelProfile &profile,
                                                     const JsonValue &body) {
    ModelResponse response;
    response.contract_version = request.contract_version;
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = profile.model_selector;

    const auto *id = body.find("id");
    if (id != nullptr && id->is_string()) {
        response.provider_response_id = *id->as_string();
    }
    const auto *model = body.find("model");
    if (model != nullptr && model->is_string()) {
        response.resolved_model = *model->as_string();
    }

    const auto *status = body.find("status");
    if (status == nullptr || !status->is_string()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "response carries no terminal status");
    }
    const auto &status_text = *status->as_string();
    if (status_text == "in_progress" || status_text == "queued") {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "non-terminal status on a synchronous response");
    }
    if (status_text == "completed") {
        response.status = ModelCompletionStatus::Completed;
    } else if (status_text == "failed") {
        response.status = ModelCompletionStatus::Failed;
    } else if (status_text == "incomplete") {
        response.status = ModelCompletionStatus::Incomplete;
        response.incomplete_reason = IncompleteReason::Other;
        if (const auto *details = body.find("incomplete_details");
            details != nullptr && details->is_object()) {
            if (const auto *reason = details->find("reason");
                reason != nullptr && reason->is_string()) {
                if (*reason->as_string() == "max_output_tokens") {
                    response.incomplete_reason = IncompleteReason::MaxOutputTokens;
                }
            }
        }
    } else if (status_text == "cancelled") {
        response.status = ModelCompletionStatus::Cancelled;
    } else {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "response carries an unknown terminal status");
    }

    const auto *output = body.find("output");
    if (output != nullptr && output->is_array()) {
        for (const auto &item : *output->as_array()) {
            if (!item.is_object()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "output item is not an object");
            }
            const auto *type = item.find("type");
            if (type == nullptr || !type->is_string()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "output item carries no type");
            }
            const auto &type_text = *type->as_string();
            if (type_text == "message") {
                MessageOutput message;
                message.role = ModelRole::Assistant;
                if (const auto *content = item.find("content");
                    content != nullptr && content->is_array()) {
                    for (const auto &part : *content->as_array()) {
                        if (!part.is_object()) {
                            return make_model_error(ModelDomainCode::ProtocolViolation,
                                                    "message content part is not an object");
                        }
                        const auto *part_type = part.find("type");
                        if (part_type == nullptr || !part_type->is_string()) {
                            return make_model_error(ModelDomainCode::ProtocolViolation,
                                                    "message content part carries no type");
                        }
                        const auto &part_type_text = *part_type->as_string();
                        if (part_type_text == "output_text") {
                            OutputTextPart text_part;
                            if (const auto *text = part.find("text");
                                text != nullptr && text->is_string()) {
                                text_part.text = *text->as_string();
                            }
                            message.content.emplace_back(std::move(text_part));
                        } else if (part_type_text == "refusal") {
                            OutputRefusalPart refusal_part;
                            if (const auto *refusal = part.find("refusal");
                                refusal != nullptr && refusal->is_string()) {
                                refusal_part.safe_summary = *refusal->as_string();
                            }
                            message.content.emplace_back(std::move(refusal_part));
                        } else {
                            // Unknown content part: diagnostic summary only.
                            UnknownOutput unknown;
                            unknown.provider_type = "responses.content_part." + part_type_text;
                            unknown.payload_digest = digest_string(to_json_string(part));
                            response.output.emplace_back(std::move(unknown));
                        }
                    }
                }
                response.output.emplace_back(std::move(message));
            } else if (type_text == "function_call") {
                ToolCallOutput call;
                const auto *call_id = item.find("call_id");
                if (call_id == nullptr || !call_id->is_string()) {
                    return make_model_error(ModelDomainCode::ProtocolViolation,
                                            "function call carries no call id");
                }
                call.provider_call_id = ProviderToolCallId{*call_id->as_string()};
                const auto *name = item.find("name");
                if (name == nullptr || !name->is_string()) {
                    return make_model_error(ModelDomainCode::ProtocolViolation,
                                            "function call carries no name");
                }
                call.provider_name = *name->as_string();
                const auto found = std::find_if(request.tools.begin(), request.tools.end(),
                                                [&](const ExposedToolSpec &tool) {
                                                    return tool.wire_name == call.provider_name;
                                                });
                if (found != request.tools.end()) {
                    call.tool_id = found->tool_id;
                }
                const auto *arguments = item.find("arguments");
                if (arguments != nullptr && arguments->is_string()) {
                    auto decoded = parse_json(*arguments->as_string());
                    if (!decoded || !decoded.value().is_object()) {
                        return make_model_error(ModelDomainCode::ProtocolViolation,
                                                "function arguments are not a json object");
                    }
                    call.arguments = std::move(decoded).value();
                } else {
                    call.arguments = JsonValue::Object{};
                }
                call.arguments_digest = digest_string(to_json_string(call.arguments));
                response.output.emplace_back(std::move(call));
            } else if (type_text == "refusal") {
                RefusalOutput refusal;
                if (const auto *text = item.find("refusal"); text != nullptr && text->is_string()) {
                    refusal.safe_summary = *text->as_string();
                }
                response.output.emplace_back(std::move(refusal));
            } else if (type_text == "reasoning") {
                // Reasoning items surface as bounded thinking (DEC-052): raw
                // reasoning text when the provider exposes it, otherwise the
                // provider summary under redacted semantics. Items carrying
                // neither (e.g. encrypted-only payloads) keep the digest
                // fallback because their content cannot be displayed.
                const auto collect = [&](const char *key, std::vector<std::string> &into) -> bool {
                    const auto *parts = item.find(key);
                    if (parts == nullptr || parts->is_null()) {
                        return true;
                    }
                    if (!parts->is_array()) {
                        return false;
                    }
                    for (const auto &part : *parts->as_array()) {
                        if (!part.is_object()) {
                            return false;
                        }
                        const auto *part_type = part.find("type");
                        if (part_type == nullptr || !part_type->is_string()) {
                            return false;
                        }
                        const auto &part_type_text = *part_type->as_string();
                        const bool raw = part_type_text == "reasoning_text";
                        if (!raw && part_type_text != "summary_text") {
                            continue; // Not a displayable reasoning text part.
                        }
                        const auto *text = part.find("text");
                        if (text == nullptr || !text->is_string()) {
                            return false;
                        }
                        if (!text->as_string()->empty()) {
                            into.push_back(*text->as_string());
                        }
                    }
                    return true;
                };
                std::vector<std::string> raw, summary;
                if (!collect("content", raw) || !collect("summary", summary)) {
                    return make_model_error(ModelDomainCode::ProtocolViolation,
                                            "reasoning item carries malformed text parts");
                }
                const auto &source = raw.empty() ? summary : raw;
                const bool redacted = raw.empty();
                bool emitted = false;
                for (const auto &text : source) {
                    if (text.size() > kMaxThinkingBytes) {
                        return make_model_error(ModelDomainCode::ProtocolViolation,
                                                "reasoning item text exceeds the thinking budget");
                    }
                    response.output.emplace_back(ThinkingPart{text, {}, redacted});
                    emitted = true;
                }
                if (!emitted) {
                    UnknownOutput unknown;
                    unknown.provider_type = "responses.item." + type_text;
                    unknown.payload_digest = digest_string(to_json_string(item));
                    response.output.emplace_back(std::move(unknown));
                }
            } else {
                // Unknown item: pure diagnostic types keep a digest summary;
                // anything action-shaped fails closed.
                UnknownOutput unknown;
                unknown.provider_type = "responses.item." + type_text;
                unknown.payload_digest = digest_string(to_json_string(item));
                response.output.emplace_back(std::move(unknown));
            }
        }
    }

    response.usage = parse_usage_object(body.find("usage"), false);
    return response;
}

Result<ModelResponse> ResponsesV1Mapper::decode_response(const ModelRequest &request,
                                                         const ModelProfile &profile,
                                                         const WireHttpResponse &wire) const {
    if (wire.status < 200 || wire.status >= 300) {
        return map_http_error_status(wire);
    }
    auto parsed = parse_json(wire.body);
    if (!parsed || !parsed.value().is_object()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "response body is not a json object");
    }
    auto response = decode_responses_terminal_body(request, profile, parsed.value());
    if (!response) {
        return response;
    }
    response.value().rate_limit = parse_rate_limit_headers(wire.headers);
    return response;
}

} // namespace mira
