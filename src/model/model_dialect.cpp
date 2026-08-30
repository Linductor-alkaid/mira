#include <mira/model_dialect.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <utility>

namespace mira {
namespace {

constexpr std::size_t kMaxInlineImageBytes = 8ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::string lowercase_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

[[nodiscard]] const std::string *find_header(const std::vector<std::pair<std::string, std::string>> &headers,
                                             const std::string &name) {
    const auto lower = lowercase_copy(name);
    for (const auto &header : headers) {
        if (lowercase_copy(header.first) == lower) {
            return &header.second;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> provider_error_code(const JsonValue &body) {
    if (!body.is_object()) {
        return std::nullopt;
    }
    const auto *error = body.find("error");
    if (error == nullptr || !error->is_object()) {
        return std::nullopt;
    }
    const auto *code = error->find("code");
    if (code != nullptr && code->is_string()) {
        return *code->as_string();
    }
    const auto *type = error->find("type");
    if (type != nullptr && type->is_string()) {
        return *type->as_string();
    }
    return std::nullopt;
}

[[nodiscard]] ModelUsage parse_usage_object(const JsonValue *usage, bool from_stream_eof) {
    ModelUsage result;
    if (usage == nullptr || !usage->is_object()) {
        result.quality = UsageQuality::Missing;
        return result;
    }
    const auto read = [&usage](const char *key) -> std::optional<std::uint64_t> {
        if (const auto *field = usage->find(key); field != nullptr && field->is_integer()) {
            return static_cast<std::uint64_t>(field->as_integer().value());
        }
        return std::nullopt;
    };
    result.input_tokens = read("input_tokens");
    if (!result.input_tokens.has_value()) {
        result.input_tokens = read("prompt_tokens");
    }
    result.output_tokens = read("output_tokens");
    if (!result.output_tokens.has_value()) {
        result.output_tokens = read("completion_tokens");
    }
    if (const auto *details = usage->find("input_tokens_details");
        details != nullptr && details->is_object()) {
        if (const auto *cached = details->find("cached_tokens");
            cached != nullptr && cached->is_integer()) {
            result.cached_input_tokens = static_cast<std::uint64_t>(cached->as_integer().value());
        }
    }
    if (result.cached_input_tokens == std::nullopt) {
        if (const auto *details = usage->find("prompt_tokens_details");
            details != nullptr && details->is_object()) {
            if (const auto *cached = details->find("cached_tokens");
                cached != nullptr && cached->is_integer()) {
                result.cached_input_tokens =
                    static_cast<std::uint64_t>(cached->as_integer().value());
            }
        }
    }
    if (const auto *details = usage->find("output_tokens_details");
        details != nullptr && details->is_object()) {
        if (const auto *reasoning = details->find("reasoning_tokens");
            reasoning != nullptr && reasoning->is_integer()) {
            result.reasoning_tokens =
                static_cast<std::uint64_t>(reasoning->as_integer().value());
        }
    }
    if (result.reasoning_tokens == std::nullopt) {
        if (const auto *details = usage->find("completion_tokens_details");
            details != nullptr && details->is_object()) {
            if (const auto *reasoning = details->find("reasoning_tokens");
                reasoning != nullptr && reasoning->is_integer()) {
                result.reasoning_tokens =
                    static_cast<std::uint64_t>(reasoning->as_integer().value());
            }
        }
    }
    const bool any = result.input_tokens.has_value() || result.output_tokens.has_value() ||
                     result.cached_input_tokens.has_value() || result.reasoning_tokens.has_value();
    if (!any) {
        result.quality = UsageQuality::Missing;
    } else if (from_stream_eof) {
        result.quality = UsageQuality::Partial;
    } else {
        result.quality = UsageQuality::ProviderReported;
    }
    return result;
}

[[nodiscard]] Result<std::string> fetch_image_data_url(const ArtifactRef &reference,
                                                       IArtifactSource &artifacts) {
    if (reference.byte_size > kMaxInlineImageBytes) {
        return make_model_error(ModelDomainCode::ResponseTooLarge,
                                "inline image exceeds the transport size limit");
    }
    auto bytes = artifacts.fetch(reference);
    if (!bytes) {
        return bytes.error();
    }
    if (bytes.value().empty()) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "image artifact payload is empty");
    }
    return "data:" + reference.media_type + ";base64," + base64_encode(bytes.value());
}

[[nodiscard]] Result<void> gate_generation_options(const ModelProfile &profile,
                                                   const ModelGenerationOptions &generation) {
    const auto unsupported = unsupported_generation_parameters(profile.capabilities.generation,
                                                               generation);
    if (!unsupported.empty()) {
        std::string message = "generation parameters cannot be represented by the dialect:";
        for (const auto &item : unsupported) {
            message += " ";
            message += item;
        }
        return make_model_error(ModelDomainCode::CapabilityMismatch, std::move(message));
    }
    return Result<void>{};
}

} // namespace

std::string base64_encode(std::span<const std::byte> bytes) {
    static constexpr std::array<char, 64> kAlphabet = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto b0 = static_cast<std::uint32_t>(bytes[index]);
        const auto b1 = static_cast<std::uint32_t>(bytes[index + 1]);
        const auto b2 = static_cast<std::uint32_t>(bytes[index + 2]);
        const auto triple = (b0 << 16U) | (b1 << 8U) | b2;
        output.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        output.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        output.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        output.push_back(kAlphabet[triple & 0x3FU]);
        index += 3;
    }
    const auto remaining = bytes.size() - index;
    if (remaining == 1) {
        const auto b0 = static_cast<std::uint32_t>(bytes[index]);
        output.push_back(kAlphabet[(b0 >> 2U) & 0x3FU]);
        output.push_back(kAlphabet[(b0 << 4U) & 0x30U]);
        output.append("==");
    } else if (remaining == 2) {
        const auto b0 = static_cast<std::uint32_t>(bytes[index]);
        const auto b1 = static_cast<std::uint32_t>(bytes[index + 1]);
        output.push_back(kAlphabet[(b0 >> 2U) & 0x3FU]);
        output.push_back(kAlphabet[((b0 << 4U) | (b1 >> 4U)) & 0x3FU]);
        output.push_back(kAlphabet[(b1 << 2U) & 0x3CU]);
        output.push_back('=');
    }
    return output;
}

Error map_http_error_status(const WireHttpResponse &wire) {
    // Provider error codes are short symbols, not response bodies; they may
    // appear in safe messages after bounded sanitization.
    std::string provider_code;
    if (auto parsed = parse_json(wire.body); parsed && parsed.value().is_object()) {
        if (auto code = provider_error_code(parsed.value()); code.has_value()) {
            static constexpr std::size_t kMaxCodeBytes = 64;
            if (code->size() <= kMaxCodeBytes &&
                std::all_of(code->begin(), code->end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
                           c == '-' || c == '.';
                })) {
                provider_code = " (provider code: " + *code + ")";
            }
        }
    }
    const auto message_with_code = [&provider_code](const char *base) {
        return std::string(base) + provider_code;
    };
    switch (wire.status) {
    case 401:
        return make_model_error(ModelDomainCode::AuthenticationFailed,
                                message_with_code("provider rejected credentials"), false);
    case 403:
        return make_model_error(ModelDomainCode::ProviderPermissionDenied,
                                message_with_code("provider denied access to the model or project"),
                                false);
    case 404:
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                message_with_code("provider reported an unknown model or endpoint"),
                                false);
    case 400:
    case 422:
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                message_with_code("provider rejected the request as invalid"),
                                false);
    case 413:
        return make_model_error(ModelDomainCode::ResponseTooLarge,
                                message_with_code("provider rejected the request payload size"),
                                false);
    case 408:
        return make_model_error(ModelDomainCode::TransportFailed,
                                message_with_code("provider reported a request timeout"), true);
    case 429:
        return make_model_error(ModelDomainCode::RateLimited,
                                message_with_code("provider rate limited the request"), true);
    case 500:
    case 502:
    case 503:
    case 504:
        return make_model_error(ModelDomainCode::ProviderOverloaded,
                                message_with_code("provider reported a retryable server failure"),
                                true);
    default:
        if (wire.status >= 500) {
            return make_model_error(ModelDomainCode::ProviderOverloaded,
                                    message_with_code("provider reported a retryable server failure"),
                                    true);
        }
        // Unknown terminal statuses fail closed instead of being guessed.
        return make_model_error(
            ModelDomainCode::ProtocolViolation,
            "unexpected provider http status: " + std::to_string(wire.status) + provider_code,
            false);
    }
}

std::optional<std::chrono::milliseconds>
parse_retry_after(const std::vector<std::pair<std::string, std::string>> &headers,
                  std::chrono::milliseconds cap) {
    const auto *value = find_header(headers, "Retry-After");
    if (value == nullptr) {
        return std::nullopt;
    }
    std::chrono::milliseconds delay{0};
    if (!value->empty() && std::all_of(value->begin(), value->end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        try {
            delay = std::chrono::milliseconds(std::stoll(*value) * 1000);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    } else {
        // HTTP-date form; parsed with the platform-neutral time functions.
        std::tm parsed{};
        std::istringstream stream(*value);
        stream >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S");
        if (stream.fail()) {
            return std::nullopt;
        }
        const auto target = std::mktime(&parsed);
        const auto now = std::time(nullptr);
        if (target <= now) {
            return std::chrono::milliseconds{0};
        }
        delay = std::chrono::milliseconds((target - now) * 1000);
    }
    if (delay > cap) {
        delay = cap;
    }
    return delay;
}

RateLimitMetadata
parse_rate_limit_headers(const std::vector<std::pair<std::string, std::string>> &headers) {
    RateLimitMetadata metadata;
    const auto parse_count = [](const std::string &text) -> std::optional<std::uint64_t> {
        if (text.empty()) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint64_t>(std::stoll(text));
        } catch (const std::exception &) {
            return std::nullopt;
        }
    };
    if (const auto *value = find_header(headers, "x-ratelimit-remaining-requests")) {
        metadata.remaining_requests = parse_count(*value);
    }
    if (const auto *value = find_header(headers, "x-ratelimit-remaining-tokens")) {
        metadata.remaining_tokens = parse_count(*value);
    }
    return metadata;
}

// ---------------------------------------------------------------------------
// openai.responses.v1
// ---------------------------------------------------------------------------

Result<JsonValue> ResponsesV1Mapper::encode_request(const ModelRequest &request,
                                                    const ModelProfile &profile, bool stream,
                                                    IArtifactSource &artifacts) const {
    if (auto gate = gate_generation_options(profile, request.generation); !gate) {
        return gate.error();
    }
    JsonValue::Object root;
    root.emplace_back("model", profile.model_selector);

    JsonValue::Array input;
    for (const auto &item : request.input) {
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
                auto data_url = fetch_image_data_url(image->source, artifacts);
                if (!data_url) {
                    return data_url.error();
                }
                JsonValue::Object part_json;
                part_json.emplace_back("type", "input_image");
                part_json.emplace_back("image_url", std::move(data_url).value());
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
                                            "profile does not accept file input");
                }
                auto data_url = fetch_image_data_url(file->source, artifacts);
                if (!data_url) {
                    return data_url.error();
                }
                JsonValue::Object part_json;
                part_json.emplace_back("type", "input_file");
                part_json.emplace_back("file_data", std::move(data_url).value());
                part_json.emplace_back("filename", file->display_name);
                parts.emplace_back(std::move(part_json));
            }
        }
        item_json.emplace_back("content", std::move(parts));
        input.emplace_back(std::move(item_json));
    }
    if (request.continuation.has_value() &&
        request.continuation->previous_response_id.has_value()) {
        root.emplace_back("previous_response_id",
                          *request.continuation->previous_response_id);
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
                const auto found = std::find_if(
                    request.tools.begin(), request.tools.end(),
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
                if (const auto *text = item.find("refusal");
                    text != nullptr && text->is_string()) {
                    refusal.safe_summary = *text->as_string();
                }
                response.output.emplace_back(std::move(refusal));
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

// ---------------------------------------------------------------------------
// openai.chat-completions.v1
// ---------------------------------------------------------------------------

Result<JsonValue> ChatCompletionsV1Mapper::encode_request(const ModelRequest &request,
                                                          const ModelProfile &profile, bool stream,
                                                          IArtifactSource &artifacts) const {
    if (auto gate = gate_generation_options(profile, request.generation); !gate) {
        return gate.error();
    }
    // Fields with no Chat Completions representation fail closed; the mapper
    // never silently switches to the Responses endpoint instead.
    if (request.continuation.has_value()) {
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "continuation cannot be represented on this dialect");
    }
    if (request.data_policy.store.has_value() && *request.data_policy.store) {
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "remote storage cannot be represented on this dialect");
    }
    if (stream && !profile.capabilities.sse.supported) {
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "chat completions sse is not fixture-verified for this profile");
    }

    JsonValue::Object root;
    root.emplace_back("model", profile.model_selector);

    JsonValue::Array messages;
    for (const auto &item : request.input) {
        JsonValue::Object message;
        switch (item.role) {
        case ModelRole::System:
            message.emplace_back("role", "system");
            break;
        case ModelRole::Developer:
            message.emplace_back("role", "developer");
            break;
        case ModelRole::User:
            message.emplace_back("role", "user");
            break;
        case ModelRole::Assistant:
            message.emplace_back("role", "assistant");
            break;
        case ModelRole::Unknown:
            return make_model_error(ModelDomainCode::InvalidModelRequest,
                                    "input items must carry a known role");
        }
        bool has_image = false;
        for (const auto &part : item.content) {
            if (std::get_if<ImagePart>(&part) != nullptr) {
                has_image = true;
            }
            if (std::get_if<FilePart>(&part) != nullptr) {
                return make_model_error(ModelDomainCode::CapabilityMismatch,
                                        "file parts cannot be represented on this dialect");
            }
        }
        if (!has_image) {
            // Plain text content stays a simple string on the wire.
            std::string text;
            for (const auto &part : item.content) {
                if (const auto *text_part = std::get_if<TextPart>(&part)) {
                    text += text_part->text;
                }
            }
            message.emplace_back("content", text);
        } else {
            if (!profile.capabilities.image_input.supported) {
                return make_model_error(ModelDomainCode::CapabilityMismatch,
                                        "profile does not accept image input");
            }
            JsonValue::Array parts;
            for (const auto &part : item.content) {
                if (const auto *text_part = std::get_if<TextPart>(&part)) {
                    JsonValue::Object part_json;
                    part_json.emplace_back("type", "text");
                    part_json.emplace_back("text", text_part->text);
                    parts.emplace_back(std::move(part_json));
                } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                    auto data_url = fetch_image_data_url(image->source, artifacts);
                    if (!data_url) {
                        return data_url.error();
                    }
                    JsonValue::Object part_json;
                    part_json.emplace_back("type", "image_url");
                    JsonValue::Object url;
                    url.emplace_back("url", std::move(data_url).value());
                    switch (image->detail) {
                    case ImageDetail::Low:
                        url.emplace_back("detail", "low");
                        break;
                    case ImageDetail::High:
                        url.emplace_back("detail", "high");
                        break;
                    case ImageDetail::Auto:
                    case ImageDetail::Original:
                        url.emplace_back("detail", "auto");
                        break;
                    }
                    part_json.emplace_back("image_url", std::move(url));
                    parts.emplace_back(std::move(part_json));
                }
            }
            message.emplace_back("content", std::move(parts));
        }
        messages.emplace_back(std::move(message));
    }
    root.emplace_back("messages", std::move(messages));

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
        root.emplace_back("response_format", std::move(format));
        break;
    }
    case OutputMode::StrictFunctionTool:
    case OutputMode::Text:
        // Tool-driven and text modes declare no response format.
        break;
    }

    if (!request.tools.empty()) {
        if (!profile.capabilities.function_tools.supported) {
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "profile does not support function tools");
        }
        JsonValue::Array tools;
        for (const auto &tool : request.tools) {
            JsonValue::Object function;
            function.emplace_back("name", tool.wire_name);
            function.emplace_back("description", tool.description);
            function.emplace_back("parameters", tool.parameters_schema.root);
            function.emplace_back("strict", true);
            JsonValue::Object tool_json;
            tool_json.emplace_back("type", "function");
            tool_json.emplace_back("function", std::move(function));
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
            JsonValue::Object function;
            function.emplace_back("name", found->wire_name);
            JsonValue::Object choice;
            choice.emplace_back("type", "function");
            choice.emplace_back("function", std::move(function));
            root.emplace_back("tool_choice", std::move(choice));
            break;
        }
        }
    }

    if (request.generation.max_output_tokens.has_value()) {
        root.emplace_back("max_completion_tokens",
                          static_cast<std::int64_t>(*request.generation.max_output_tokens));
    }
    if (request.generation.temperature.has_value()) {
        root.emplace_back("temperature", *request.generation.temperature);
    }
    if (request.generation.top_p.has_value()) {
        root.emplace_back("top_p", *request.generation.top_p);
    }
    if (request.generation.seed.has_value() &&
        profile.capabilities.generation.seed != ParamMapping::Unsupported) {
        root.emplace_back("seed", static_cast<std::int64_t>(*request.generation.seed));
    }
    if (request.generation.reasoning_effort.has_value() &&
        profile.capabilities.generation.reasoning_effort != ParamMapping::Unsupported) {
        switch (*request.generation.reasoning_effort) {
        case ReasoningEffort::Minimal:
            root.emplace_back("reasoning_effort", "minimal");
            break;
        case ReasoningEffort::Low:
            root.emplace_back("reasoning_effort", "low");
            break;
        case ReasoningEffort::Medium:
            root.emplace_back("reasoning_effort", "medium");
            break;
        case ReasoningEffort::High:
            root.emplace_back("reasoning_effort", "high");
            break;
        }
    }
    if (request.generation.service_tier.has_value() &&
        profile.capabilities.generation.service_tier != ParamMapping::Unsupported) {
        switch (*request.generation.service_tier) {
        case ServiceTier::Auto:
            root.emplace_back("service_tier", "auto");
            break;
        case ServiceTier::Default:
            root.emplace_back("service_tier", "default");
            break;
        case ServiceTier::Flex:
            root.emplace_back("service_tier", "flex");
            break;
        case ServiceTier::Priority:
            root.emplace_back("service_tier", "priority");
            break;
        }
    }
    if (stream) {
        root.emplace_back("stream", true);
        root.emplace_back("stream_options", JsonValue::Object{{"include_usage", true}});
    }
    return JsonValue(std::move(root));
}

Result<ModelResponse> ChatCompletionsV1Mapper::decode_response(const ModelRequest &request,
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
    const auto &body = parsed.value();

    ModelResponse response;
    response.contract_version = request.contract_version;
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = profile.model_selector;
    response.rate_limit = parse_rate_limit_headers(wire.headers);

    const auto *id = body.find("id");
    if (id != nullptr && id->is_string()) {
        response.provider_response_id = *id->as_string();
    }
    const auto *model = body.find("model");
    if (model != nullptr && model->is_string()) {
        response.resolved_model = *model->as_string();
    }

    const auto *choices = body.find("choices");
    if (choices == nullptr || !choices->is_array() || choices->as_array()->size() != 1) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "chat completions response must carry exactly one choice");
    }
    const auto &choice = choices->as_array()->front();
    if (!choice.is_object()) {
        return make_model_error(ModelDomainCode::ProtocolViolation, "choice is not an object");
    }
    const auto *finish = choice.find("finish_reason");
    if (finish == nullptr || !finish->is_string()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "choice carries no finish reason");
    }
    const auto &finish_text = *finish->as_string();
    if (finish_text == "stop" || finish_text == "tool_calls") {
        response.status = ModelCompletionStatus::Completed;
    } else if (finish_text == "length") {
        response.status = ModelCompletionStatus::Incomplete;
        response.incomplete_reason = IncompleteReason::MaxOutputTokens;
    } else if (finish_text == "content_filter") {
        response.status = ModelCompletionStatus::ContentFiltered;
    } else {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "choice carries an unknown finish reason");
    }

    const auto *message = choice.find("message");
    if (message == nullptr || !message->is_object()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "choice carries no message object");
    }
    const auto *refusal = message->find("refusal");
    if (refusal != nullptr && refusal->is_string() && !refusal->as_string()->empty()) {
        response.status = ModelCompletionStatus::Refused;
        RefusalOutput refusal_output;
        refusal_output.safe_summary = "provider returned a refusal";
        response.output.emplace_back(std::move(refusal_output));
        response.usage = parse_usage_object(body.find("usage"), false);
        return response;
    }

    MessageOutput output_message;
    output_message.role = ModelRole::Assistant;
    const auto *content = message->find("content");
    if (content != nullptr && content->is_string()) {
        OutputTextPart part;
        part.text = *content->as_string();
        output_message.content.emplace_back(std::move(part));
    }
    if (const auto *reasoning = message->find("reasoning_content");
        reasoning != nullptr && reasoning->is_string() && !reasoning->as_string()->empty()) {
        UnknownOutput unknown;
        unknown.provider_type = "chat.reasoning_content";
        unknown.payload_digest = digest_string(*reasoning->as_string());
        response.output.emplace_back(std::move(unknown));
    }
    if (!output_message.content.empty()) {
        response.output.emplace_back(std::move(output_message));
    }

    const auto *tool_calls = message->find("tool_calls");
    if (tool_calls != nullptr && tool_calls->is_array()) {
        for (const auto &call_json : *tool_calls->as_array()) {
            if (!call_json.is_object()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "tool call entry is not an object");
            }
            ToolCallOutput call;
            const auto *call_id = call_json.find("id");
            if (call_id == nullptr || !call_id->is_string()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "tool call entry carries no id");
            }
            call.provider_call_id = ProviderToolCallId{*call_id->as_string()};
            const auto *function = call_json.find("function");
            if (function == nullptr || !function->is_object()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "tool call entry carries no function object");
            }
            const auto *name = function->find("name");
            if (name == nullptr || !name->is_string()) {
                return make_model_error(ModelDomainCode::ProtocolViolation,
                                        "tool call function carries no name");
            }
            call.provider_name = *name->as_string();
            const auto found = std::find_if(
                request.tools.begin(), request.tools.end(), [&](const ExposedToolSpec &tool) {
                    return tool.wire_name == call.provider_name;
                });
            if (found != request.tools.end()) {
                call.tool_id = found->tool_id;
            }
            if (const auto *arguments = function->find("arguments");
                arguments != nullptr && arguments->is_string()) {
                auto decoded = parse_json(*arguments->as_string());
                if (!decoded || !decoded.value().is_object()) {
                    return make_model_error(ModelDomainCode::ProtocolViolation,
                                            "tool call arguments are not a json object");
                }
                call.arguments = std::move(decoded).value();
            } else {
                call.arguments = JsonValue::Object{};
            }
            call.arguments_digest = digest_string(to_json_string(call.arguments));
            response.output.emplace_back(std::move(call));
        }
    }

    if (response.output.empty()) {
        return make_model_error(ModelDomainCode::ProtocolViolation,
                                "choice message carries neither content nor tool calls");
    }
    response.usage = parse_usage_object(body.find("usage"), false);
    return response;
}

} // namespace mira
