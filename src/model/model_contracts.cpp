#include <mira/model_contracts.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

constexpr std::size_t kMaxInputItems = 256;
constexpr std::size_t kMaxContentPartsPerItem = 64;
constexpr std::size_t kMaxToolsPerRequest = 128;
constexpr std::size_t kMaxTextPartBytes = 1024 * 1024;
constexpr std::size_t kMaxOutputItems = 256;
constexpr std::size_t kMaxSafeMessageBytes = 2048;

[[nodiscard]] bool is_nil_id(const Id128 &id) noexcept { return id.is_nil(); }

[[nodiscard]] Error contract_error(std::string message) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.model";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::string sensitivity_name(Sensitivity sensitivity) {
    switch (sensitivity) {
    case Sensitivity::Public:
        return "public";
    case Sensitivity::Internal:
        return "internal";
    case Sensitivity::Sensitive:
        return "sensitive";
    case Sensitivity::Secret:
        return "secret";
    }
    return "unknown";
}

[[nodiscard]] std::optional<Sensitivity> sensitivity_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "public") {
        return Sensitivity::Public;
    }
    if (*text == "internal") {
        return Sensitivity::Internal;
    }
    if (*text == "sensitive") {
        return Sensitivity::Sensitive;
    }
    if (*text == "secret") {
        return Sensitivity::Secret;
    }
    return std::nullopt;
}

[[nodiscard]] std::string role_name(ModelRole role) {
    switch (role) {
    case ModelRole::System:
        return "system";
    case ModelRole::Developer:
        return "developer";
    case ModelRole::User:
        return "user";
    case ModelRole::Assistant:
        return "assistant";
    case ModelRole::Unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ModelRole> role_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "system") {
        return ModelRole::System;
    }
    if (*text == "developer") {
        return ModelRole::Developer;
    }
    if (*text == "user") {
        return ModelRole::User;
    }
    if (*text == "assistant") {
        return ModelRole::Assistant;
    }
    // Unknown enum values fail closed instead of being coerced to a known
    // role; "unknown" itself is the only accepted spelling.
    if (*text == "unknown") {
        return ModelRole::Unknown;
    }
    return std::nullopt;
}

[[nodiscard]] std::string detail_name(ImageDetail detail) {
    switch (detail) {
    case ImageDetail::Auto:
        return "auto";
    case ImageDetail::Low:
        return "low";
    case ImageDetail::High:
        return "high";
    case ImageDetail::Original:
        return "original";
    }
    return "auto";
}

[[nodiscard]] std::optional<ImageDetail> detail_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "auto") {
        return ImageDetail::Auto;
    }
    if (*text == "low") {
        return ImageDetail::Low;
    }
    if (*text == "high") {
        return ImageDetail::High;
    }
    if (*text == "original") {
        return ImageDetail::Original;
    }
    return std::nullopt;
}

[[nodiscard]] std::string output_mode_name(OutputMode mode) {
    switch (mode) {
    case OutputMode::StrictJsonSchema:
        return "strict_json_schema";
    case OutputMode::StrictFunctionTool:
        return "strict_function_tool";
    case OutputMode::JsonObject:
        return "json_object";
    case OutputMode::Text:
        return "text";
    }
    return "text";
}

[[nodiscard]] std::optional<OutputMode> output_mode_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "strict_json_schema") {
        return OutputMode::StrictJsonSchema;
    }
    if (*text == "strict_function_tool") {
        return OutputMode::StrictFunctionTool;
    }
    if (*text == "json_object") {
        return OutputMode::JsonObject;
    }
    if (*text == "text") {
        return OutputMode::Text;
    }
    return std::nullopt;
}

[[nodiscard]] std::string reasoning_effort_name(ReasoningEffort effort) {
    switch (effort) {
    case ReasoningEffort::Minimal:
        return "minimal";
    case ReasoningEffort::Low:
        return "low";
    case ReasoningEffort::Medium:
        return "medium";
    case ReasoningEffort::High:
        return "high";
    }
    return "medium";
}

[[nodiscard]] std::optional<ReasoningEffort> reasoning_effort_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "minimal") {
        return ReasoningEffort::Minimal;
    }
    if (*text == "low") {
        return ReasoningEffort::Low;
    }
    if (*text == "medium") {
        return ReasoningEffort::Medium;
    }
    if (*text == "high") {
        return ReasoningEffort::High;
    }
    return std::nullopt;
}

[[nodiscard]] std::string service_tier_name(ServiceTier tier) {
    switch (tier) {
    case ServiceTier::Auto:
        return "auto";
    case ServiceTier::Default:
        return "default";
    case ServiceTier::Flex:
        return "flex";
    case ServiceTier::Priority:
        return "priority";
    }
    return "auto";
}

[[nodiscard]] std::optional<ServiceTier> service_tier_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "auto") {
        return ServiceTier::Auto;
    }
    if (*text == "default") {
        return ServiceTier::Default;
    }
    if (*text == "flex") {
        return ServiceTier::Flex;
    }
    if (*text == "priority") {
        return ServiceTier::Priority;
    }
    return std::nullopt;
}

[[nodiscard]] std::string tool_choice_mode_name(ToolChoiceMode mode) {
    switch (mode) {
    case ToolChoiceMode::Auto:
        return "auto";
    case ToolChoiceMode::None:
        return "none";
    case ToolChoiceMode::Required:
        return "required";
    case ToolChoiceMode::Named:
        return "named";
    }
    return "auto";
}

[[nodiscard]] std::optional<ToolChoiceMode> tool_choice_mode_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "auto") {
        return ToolChoiceMode::Auto;
    }
    if (*text == "none") {
        return ToolChoiceMode::None;
    }
    if (*text == "required") {
        return ToolChoiceMode::Required;
    }
    if (*text == "named") {
        return ToolChoiceMode::Named;
    }
    return std::nullopt;
}

[[nodiscard]] std::string status_name(ModelCompletionStatus status) {
    switch (status) {
    case ModelCompletionStatus::Completed:
        return "completed";
    case ModelCompletionStatus::Incomplete:
        return "incomplete";
    case ModelCompletionStatus::Refused:
        return "refused";
    case ModelCompletionStatus::ContentFiltered:
        return "content_filtered";
    case ModelCompletionStatus::Failed:
        return "failed";
    case ModelCompletionStatus::Cancelled:
        return "cancelled";
    case ModelCompletionStatus::Unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ModelCompletionStatus> status_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "completed") {
        return ModelCompletionStatus::Completed;
    }
    if (*text == "incomplete") {
        return ModelCompletionStatus::Incomplete;
    }
    if (*text == "refused") {
        return ModelCompletionStatus::Refused;
    }
    if (*text == "content_filtered") {
        return ModelCompletionStatus::ContentFiltered;
    }
    if (*text == "failed") {
        return ModelCompletionStatus::Failed;
    }
    if (*text == "cancelled") {
        return ModelCompletionStatus::Cancelled;
    }
    if (*text == "unknown") {
        return ModelCompletionStatus::Unknown;
    }
    return std::nullopt;
}

[[nodiscard]] std::string incomplete_reason_name(IncompleteReason reason) {
    switch (reason) {
    case IncompleteReason::MaxOutputTokens:
        return "max_output_tokens";
    case IncompleteReason::ContextWindow:
        return "context_window";
    case IncompleteReason::Other:
        return "other";
    }
    return "other";
}

[[nodiscard]] std::optional<IncompleteReason> incomplete_reason_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "max_output_tokens") {
        return IncompleteReason::MaxOutputTokens;
    }
    if (*text == "context_window") {
        return IncompleteReason::ContextWindow;
    }
    if (*text == "other") {
        return IncompleteReason::Other;
    }
    return std::nullopt;
}

[[nodiscard]] std::string usage_quality_name(UsageQuality quality) {
    switch (quality) {
    case UsageQuality::Exact:
        return "exact";
    case UsageQuality::ProviderReported:
        return "provider_reported";
    case UsageQuality::Estimated:
        return "estimated";
    case UsageQuality::Partial:
        return "partial";
    case UsageQuality::Missing:
        return "missing";
    }
    return "missing";
}

[[nodiscard]] std::optional<UsageQuality> usage_quality_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "exact") {
        return UsageQuality::Exact;
    }
    if (*text == "provider_reported") {
        return UsageQuality::ProviderReported;
    }
    if (*text == "estimated") {
        return UsageQuality::Estimated;
    }
    if (*text == "partial") {
        return UsageQuality::Partial;
    }
    if (*text == "missing") {
        return UsageQuality::Missing;
    }
    return std::nullopt;
}

[[nodiscard]] std::string provenance_source(const Provenance &provenance) {
    return provenance.source.empty() ? "unknown" : provenance.source;
}

[[nodiscard]] JsonValue artifact_ref_to_json(const ArtifactRef &reference) {
    JsonValue::Object object;
    object.emplace_back("id", reference.id.to_string());
    object.emplace_back("digest", reference.digest.to_string());
    object.emplace_back("byte_size", static_cast<std::int64_t>(reference.byte_size));
    object.emplace_back("media_type", reference.media_type);
    object.emplace_back("sensitivity", sensitivity_name(reference.sensitivity));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ArtifactRef> artifact_ref_from(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error("artifact reference must be an object");
    }
    ArtifactRef reference;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return contract_error("artifact reference requires an id string");
    }
    const auto parsed_id = ArtifactId::parse(*id->as_string());
    if (!parsed_id) {
        return contract_error("artifact reference id is malformed");
    }
    reference.id = *parsed_id;
    const auto *digest = json.find("digest");
    if (digest != nullptr && digest->is_string()) {
        auto parsed = digest_from_hex(*digest->as_string());
        if (!parsed) {
            return contract_error("artifact reference digest is malformed");
        }
        reference.digest = *parsed;
    }
    const auto *byte_size = json.find("byte_size");
    if (byte_size != nullptr && byte_size->is_integer()) {
        reference.byte_size = static_cast<std::uint64_t>(byte_size->as_integer().value());
    }
    const auto *media_type = json.find("media_type");
    if (media_type != nullptr && media_type->is_string()) {
        reference.media_type = *media_type->as_string();
    }
    const auto *sensitivity = json.find("sensitivity");
    if (sensitivity != nullptr && sensitivity->is_string()) {
        const auto parsed = sensitivity_from(*sensitivity);
        if (!parsed) {
            return contract_error("artifact reference has unknown sensitivity");
        }
        reference.sensitivity = *parsed;
    }
    return reference;
}

} // namespace

std::string model_domain_code_name(ModelDomainCode code) {
    switch (code) {
    case ModelDomainCode::EndpointPolicyDenied:
        return "EndpointPolicyDenied";
    case ModelDomainCode::AuthenticationFailed:
        return "AuthenticationFailed";
    case ModelDomainCode::ProviderPermissionDenied:
        return "ProviderPermissionDenied";
    case ModelDomainCode::InvalidModelRequest:
        return "InvalidModelRequest";
    case ModelDomainCode::CapabilityMismatch:
        return "CapabilityMismatch";
    case ModelDomainCode::ContextLimitExceeded:
        return "ContextLimitExceeded";
    case ModelDomainCode::RateLimited:
        return "RateLimited";
    case ModelDomainCode::ProviderOverloaded:
        return "ProviderOverloaded";
    case ModelDomainCode::TransportFailed:
        return "TransportFailed";
    case ModelDomainCode::ProtocolViolation:
        return "ProtocolViolation";
    case ModelDomainCode::ResponseTooLarge:
        return "ResponseTooLarge";
    case ModelDomainCode::ModelRefused:
        return "ModelRefused";
    case ModelDomainCode::ContentFiltered:
        return "ContentFiltered";
    case ModelDomainCode::IncompleteModelOutput:
        return "IncompleteModelOutput";
    case ModelDomainCode::MalformedStructuredOutput:
        return "MalformedStructuredOutput";
    case ModelDomainCode::AmbiguousModelOutput:
        return "AmbiguousModelOutput";
    case ModelDomainCode::AmbiguousCompletion:
        return "AmbiguousCompletion";
    case ModelDomainCode::ModelCancelled:
        return "ModelCancelled";
    case ModelDomainCode::ModelDeadlineExceeded:
        return "ModelDeadlineExceeded";
    case ModelDomainCode::ModelResourceExhausted:
        return "ModelResourceExhausted";
    }
    return "Unknown";
}

Error make_model_error(ModelDomainCode code, std::string safe_message, bool retryable,
                       std::optional<OperationId> operation) {
    Error error;
    switch (code) {
    case ModelDomainCode::EndpointPolicyDenied:
    case ModelDomainCode::AuthenticationFailed:
    case ModelDomainCode::ProviderPermissionDenied:
        error.code = ErrorCode::PermissionDenied;
        break;
    case ModelDomainCode::InvalidModelRequest:
        error.code = ErrorCode::InvalidArgument;
        break;
    case ModelDomainCode::CapabilityMismatch:
        error.code = ErrorCode::UnsupportedCapability;
        break;
    case ModelDomainCode::ContextLimitExceeded:
        error.code = ErrorCode::ContextOverflow;
        break;
    case ModelDomainCode::RateLimited:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case ModelDomainCode::ProviderOverloaded:
    case ModelDomainCode::TransportFailed:
        error.code = ErrorCode::Unavailable;
        break;
    case ModelDomainCode::ProtocolViolation:
        error.code = ErrorCode::DataLoss;
        break;
    case ModelDomainCode::ResponseTooLarge:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case ModelDomainCode::ModelRefused:
    case ModelDomainCode::IncompleteModelOutput:
    case ModelDomainCode::MalformedStructuredOutput:
    case ModelDomainCode::AmbiguousModelOutput:
        error.code = ErrorCode::InvalidModelOutput;
        break;
    case ModelDomainCode::ContentFiltered:
        error.code = ErrorCode::SafetyRejected;
        break;
    case ModelDomainCode::AmbiguousCompletion:
        error.code = ErrorCode::ExecutionUncertain;
        break;
    case ModelDomainCode::ModelCancelled:
        error.code = ErrorCode::Cancelled;
        break;
    case ModelDomainCode::ModelDeadlineExceeded:
        error.code = ErrorCode::DeadlineExceeded;
        break;
    case ModelDomainCode::ModelResourceExhausted:
        error.code = ErrorCode::ResourceExhausted;
        break;
    }
    error.domain = "mira.model";
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = retryable;
    if (safe_message.size() > kMaxSafeMessageBytes) {
        safe_message.resize(kMaxSafeMessageBytes);
    }
    error.safe_message = std::move(safe_message);
    error.operation_id = operation;
    return error;
}

Result<void> validate_model_request(const ModelRequest &request) {
    if (const auto version = validate_schema_version(request.contract_version); !version) {
        return contract_error("model request contract version is not supported");
    }
    if (is_nil_id(request.request_id.value) || is_nil_id(request.operation_id.value) ||
        is_nil_id(request.task_id.value) || is_nil_id(request.profile_id.value)) {
        return contract_error("model request identifiers must be set");
    }
    if (request.input.empty()) {
        return contract_error("model request requires input items");
    }
    if (request.input.size() > kMaxInputItems) {
        return contract_error("model request exceeds the input item limit");
    }
    bool has_authoritative_prompt = false;
    for (const auto &item : request.input) {
        if (item.role == ModelRole::System || item.role == ModelRole::Developer) {
            has_authoritative_prompt = true;
        }
        if (item.role == ModelRole::Unknown) {
            return contract_error("model request input must use a known role");
        }
        if (item.content.empty()) {
            return contract_error("model request input item requires content parts");
        }
        if (item.content.size() > kMaxContentPartsPerItem) {
            return contract_error("model request input item exceeds the content part limit");
        }
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                if (text->text.size() > kMaxTextPartBytes) {
                    return contract_error("text part exceeds the size limit");
                }
                if (text->sensitivity == Sensitivity::Secret) {
                    return contract_error("secret text must not enter a model request");
                }
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                if (image->source.id.is_nil() || image->media_type.empty()) {
                    return contract_error("image part requires an artifact reference and media type");
                }
                if (image->source.sensitivity == Sensitivity::Secret) {
                    return contract_error("secret artifacts must not enter a model request");
                }
            } else if (const auto *file = std::get_if<FilePart>(&part)) {
                if (file->source.id.is_nil() || file->media_type.empty()) {
                    return contract_error("file part requires an artifact reference and media type");
                }
            }
        }
    }
    if (!has_authoritative_prompt) {
        return contract_error("model request requires a system or developer prompt item");
    }
    const auto &contract = request.output_contract;
    if (contract.mode == OutputMode::StrictJsonSchema || contract.mode == OutputMode::JsonObject) {
        if (contract.schema_id.value.is_nil() || !contract.schema.valid()) {
            return contract_error("structured output requires a schema id and object schema");
        }
        if (canonical_json_digest(contract.schema.root) != contract.canonical_schema_digest) {
            return contract_error("schema digest does not match the schema payload");
        }
    }
    if (request.tools.size() > kMaxToolsPerRequest) {
        return contract_error("model request exceeds the tool limit");
    }
    for (const auto &tool : request.tools) {
        if (tool.tool_id.value.is_nil() || tool.wire_name.empty() ||
            !tool.parameters_schema.valid()) {
            return contract_error("exposed tool requires an id, wire name and parameter schema");
        }
    }
    for (std::size_t outer = 0; outer < request.tools.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < request.tools.size(); ++inner) {
            if (request.tools[outer].wire_name == request.tools[inner].wire_name) {
                return contract_error("exposed tool wire names must be unique per request");
            }
        }
    }
    if (request.tool_choice.mode == ToolChoiceMode::Named &&
        std::none_of(request.tools.begin(), request.tools.end(), [&](const ExposedToolSpec &tool) {
            return tool.tool_id == request.tool_choice.required_tool;
        })) {
        return contract_error("named tool choice must reference an exposed tool");
    }
    if (request.tool_choice.mode != ToolChoiceMode::Named &&
        !request.tool_choice.required_tool.value.is_nil()) {
        return contract_error("tool choice id is only valid for the named mode");
    }
    if (request.generation.temperature.has_value() &&
        (*request.generation.temperature < 0.0 || *request.generation.temperature > 2.0)) {
        return contract_error("temperature is out of range");
    }
    if (request.generation.top_p.has_value() &&
        (*request.generation.top_p < 0.0 || *request.generation.top_p > 1.0)) {
        return contract_error("top_p is out of range");
    }
    if (request.continuation.has_value()) {
        const auto &continuation = *request.continuation;
        if (continuation.profile_id != request.profile_id || continuation.task_id != request.task_id ||
            continuation.task_epoch != request.task_epoch ||
            continuation.provider_state.empty()) {
            return contract_error("continuation binding does not match the request");
        }
    }
    if (request.data_policy.store.has_value() && *request.data_policy.store &&
        request.data_policy.remote_retention.count() == 0) {
        return contract_error("remote storage requires an explicit retention period");
    }
    return Result<void>{};
}

Result<void> validate_model_response(const ModelResponse &response) {
    if (const auto version = validate_schema_version(response.contract_version); !version) {
        return contract_error("model response contract version is not supported");
    }
    if (is_nil_id(response.request_id.value) || is_nil_id(response.operation_id.value) ||
        is_nil_id(response.profile_id.value)) {
        return contract_error("model response identifiers must be set");
    }
    if (response.requested_model.empty()) {
        return contract_error("model response must name the requested model");
    }
    if (response.status == ModelCompletionStatus::Unknown) {
        return contract_error("model response must carry a known completion status");
    }
    if (response.incomplete_reason.has_value() &&
        response.status != ModelCompletionStatus::Incomplete) {
        return contract_error("incomplete reason requires the incomplete status");
    }
    if (response.status == ModelCompletionStatus::Incomplete && !response.incomplete_reason.has_value()) {
        return contract_error("incomplete status requires an incomplete reason");
    }
    if (response.output.size() > kMaxOutputItems) {
        return contract_error("model response exceeds the output item limit");
    }
    const auto &usage = response.usage;
    const bool any_usage = usage.input_tokens.has_value() || usage.output_tokens.has_value() ||
                           usage.cached_input_tokens.has_value() || usage.reasoning_tokens.has_value();
    if (usage.quality == UsageQuality::Missing && any_usage) {
        return contract_error("missing usage quality must not carry token counts");
    }
    if (usage.quality != UsageQuality::Missing && usage.quality != UsageQuality::Partial &&
        !any_usage) {
        return contract_error("present usage quality requires token counts");
    }
    for (const auto &item : response.output) {
        if (const auto *call = std::get_if<ToolCallOutput>(&item)) {
            if (call->provider_call_id.value.empty() || call->tool_id.value.is_nil() ||
                call->provider_name.empty() || !call->arguments.is_object()) {
                return contract_error("tool call output is incomplete");
            }
        } else if (const auto *unknown = std::get_if<UnknownOutput>(&item)) {
            if (unknown->provider_type.empty()) {
                return contract_error("unknown output requires a provider type");
            }
        }
    }
    return Result<void>{};
}

JsonValue model_request_to_json(const ModelRequest &request) {
    JsonValue::Object root;
    root.emplace_back("contract_version",
                      JsonValue::Object{{"major", static_cast<std::int64_t>(request.contract_version.major)},
                                        {"minor", static_cast<std::int64_t>(request.contract_version.minor)}});
    root.emplace_back("request_id", request.request_id.to_string());
    root.emplace_back("operation_id", request.operation_id.to_string());
    root.emplace_back("task_id", request.task_id.to_string());
    root.emplace_back("task_epoch", static_cast<std::int64_t>(request.task_epoch));
    root.emplace_back("profile_id", request.profile_id.to_string());

    JsonValue::Array input;
    for (const auto &item : request.input) {
        JsonValue::Object item_json;
        item_json.emplace_back("role", role_name(item.role));
        item_json.emplace_back("provenance", provenance_source(item.provenance));
        item_json.emplace_back("authority", sensitivity_name(item.authority));
        JsonValue::Array parts;
        for (const auto &part : item.content) {
            JsonValue::Object part_json;
            if (const auto *text = std::get_if<TextPart>(&part)) {
                part_json.emplace_back("kind", "text");
                part_json.emplace_back("text", text->text);
                part_json.emplace_back("sensitivity", sensitivity_name(text->sensitivity));
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                part_json.emplace_back("kind", "image");
                part_json.emplace_back("source", artifact_ref_to_json(image->source));
                part_json.emplace_back("detail", detail_name(image->detail));
                part_json.emplace_back("media_type", image->media_type);
            } else if (const auto *file = std::get_if<FilePart>(&part)) {
                part_json.emplace_back("kind", "file");
                part_json.emplace_back("source", artifact_ref_to_json(file->source));
                part_json.emplace_back("media_type", file->media_type);
                part_json.emplace_back("display_name", file->display_name);
            }
            parts.emplace_back(std::move(part_json));
        }
        item_json.emplace_back("content", std::move(parts));
        input.emplace_back(std::move(item_json));
    }
    root.emplace_back("input", std::move(input));

    JsonValue::Object output_contract;
    output_contract.emplace_back("mode", output_mode_name(request.output_contract.mode));
    output_contract.emplace_back("schema_id", request.output_contract.schema_id.to_string());
    output_contract.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(request.output_contract.schema_version.major)},
                          {"minor", static_cast<std::int64_t>(request.output_contract.schema_version.minor)},
                          {"patch", static_cast<std::int64_t>(request.output_contract.schema_version.patch)}});
    if (request.output_contract.schema.valid()) {
        output_contract.emplace_back("schema", request.output_contract.schema.root);
    }
    output_contract.emplace_back("canonical_schema_digest",
                                 request.output_contract.canonical_schema_digest.to_string());
    root.emplace_back("output_contract", std::move(output_contract));

    JsonValue::Array tools;
    for (const auto &tool : request.tools) {
        JsonValue::Object tool_json;
        tool_json.emplace_back("tool_id", tool.tool_id.to_string());
        tool_json.emplace_back(
            "version",
            JsonValue::Object{{"major", static_cast<std::int64_t>(tool.version.major)},
                              {"minor", static_cast<std::int64_t>(tool.version.minor)},
                              {"patch", static_cast<std::int64_t>(tool.version.patch)}});
        tool_json.emplace_back("wire_name", tool.wire_name);
        tool_json.emplace_back("description", tool.description);
        tool_json.emplace_back("parameters", tool.parameters_schema.root);
        tool_json.emplace_back("spec_digest", tool.spec_digest.to_string());
        tool_json.emplace_back("has_side_effects", tool.has_side_effects);
        tools.emplace_back(std::move(tool_json));
    }
    root.emplace_back("tools", std::move(tools));

    JsonValue::Object tool_choice;
    tool_choice.emplace_back("mode", tool_choice_mode_name(request.tool_choice.mode));
    if (!request.tool_choice.required_tool.value.is_nil()) {
        tool_choice.emplace_back("required_tool", request.tool_choice.required_tool.to_string());
    }
    root.emplace_back("tool_choice", std::move(tool_choice));

    JsonValue::Object generation;
    if (request.generation.max_output_tokens.has_value()) {
        generation.emplace_back("max_output_tokens",
                                static_cast<std::int64_t>(*request.generation.max_output_tokens));
    }
    if (request.generation.temperature.has_value()) {
        generation.emplace_back("temperature", *request.generation.temperature);
    }
    if (request.generation.top_p.has_value()) {
        generation.emplace_back("top_p", *request.generation.top_p);
    }
    if (request.generation.seed.has_value()) {
        generation.emplace_back("seed", static_cast<std::int64_t>(*request.generation.seed));
    }
    if (request.generation.reasoning_effort.has_value()) {
        generation.emplace_back(
            "reasoning_effort", reasoning_effort_name(*request.generation.reasoning_effort));
    }
    if (request.generation.service_tier.has_value()) {
        generation.emplace_back("service_tier", service_tier_name(*request.generation.service_tier));
    }
    root.emplace_back("generation", std::move(generation));

    if (request.continuation.has_value()) {
        JsonValue::Object continuation;
        continuation.emplace_back("provider_state", request.continuation->provider_state);
        if (request.continuation->previous_response_id.has_value()) {
            continuation.emplace_back("previous_response_id",
                                      *request.continuation->previous_response_id);
        }
        continuation.emplace_back("profile_id", request.continuation->profile_id.to_string());
        continuation.emplace_back("task_id", request.continuation->task_id.to_string());
        continuation.emplace_back("task_epoch",
                                  static_cast<std::int64_t>(request.continuation->task_epoch));
        continuation.emplace_back("prompt_digest", request.continuation->prompt_digest.to_string());
        continuation.emplace_back("schema_digest", request.continuation->schema_digest.to_string());
        continuation.emplace_back("tool_snapshot_digest",
                                  request.continuation->tool_snapshot_digest.to_string());
        continuation.emplace_back("data_policy_digest",
                                  request.continuation->data_policy_digest);
        continuation.emplace_back("remote_store_enabled",
                                  request.continuation->remote_store_enabled);
        root.emplace_back("continuation", std::move(continuation));
    }

    JsonValue::Object budget;
    budget.emplace_back("max_input_tokens", static_cast<std::int64_t>(request.budget.max_input_tokens));
    budget.emplace_back("max_output_tokens",
                        static_cast<std::int64_t>(request.budget.max_output_tokens));
    budget.emplace_back("max_total_cost_micros",
                        static_cast<std::int64_t>(request.budget.max_total_cost_micros));
    budget.emplace_back("currency", request.budget.currency);
    budget.emplace_back("max_requests", static_cast<std::int64_t>(request.budget.max_requests));
    budget.emplace_back("max_image_bytes", static_cast<std::int64_t>(request.budget.max_image_bytes));
    root.emplace_back("budget", std::move(budget));

    JsonValue::Object data_policy;
    if (request.data_policy.store.has_value()) {
        data_policy.emplace_back("store", *request.data_policy.store);
    }
    data_policy.emplace_back("allow_uploads", request.data_policy.allow_uploads);
    if (request.data_policy.region.has_value()) {
        data_policy.emplace_back("region", *request.data_policy.region);
    }
    if (request.data_policy.organization.has_value()) {
        data_policy.emplace_back("organization", *request.data_policy.organization);
    }
    if (request.data_policy.project.has_value()) {
        data_policy.emplace_back("project", *request.data_policy.project);
    }
    data_policy.emplace_back("local_raw_retention",
                             static_cast<std::int64_t>(request.data_policy.local_raw_retention.count()));
    data_policy.emplace_back("remote_retention",
                             static_cast<std::int64_t>(request.data_policy.remote_retention.count()));
    root.emplace_back("data_policy", std::move(data_policy));

    JsonValue::Object provenance;
    provenance.emplace_back(
        "system_template_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(request.prompt_provenance.system_template_version.major)},
                          {"minor", static_cast<std::int64_t>(request.prompt_provenance.system_template_version.minor)},
                          {"patch", static_cast<std::int64_t>(request.prompt_provenance.system_template_version.patch)}});
    provenance.emplace_back("system_template_digest",
                            request.prompt_provenance.system_template_digest.to_string());
    provenance.emplace_back("decision_schema_digest",
                            request.prompt_provenance.decision_schema_digest.to_string());
    provenance.emplace_back("tool_snapshot_digest",
                            request.prompt_provenance.tool_snapshot_digest.to_string());
    provenance.emplace_back("context_selection_digest",
                            request.prompt_provenance.context_selection_digest.to_string());
    provenance.emplace_back(
        "redaction_policy_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(request.prompt_provenance.redaction_policy_version.major)},
                          {"minor", static_cast<std::int64_t>(request.prompt_provenance.redaction_policy_version.minor)},
                          {"patch", static_cast<std::int64_t>(request.prompt_provenance.redaction_policy_version.patch)}});
    root.emplace_back("prompt_provenance", std::move(provenance));

    return JsonValue(std::move(root));
}

Result<ModelRequest> model_request_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error("model request json must be an object");
    }
    ModelRequest request;
    const auto *contract_version = json.find("contract_version");
    if (contract_version == nullptr || !contract_version->is_object()) {
        return contract_error("model request json requires a contract version");
    }
    const auto *major = contract_version->find("major");
    const auto *minor = contract_version->find("minor");
    if (major == nullptr || minor == nullptr || !major->is_integer() || !minor->is_integer()) {
        return contract_error("model request contract version is malformed");
    }
    request.contract_version = SchemaVersion{static_cast<std::uint16_t>(major->as_integer().value()),
                                             static_cast<std::uint16_t>(minor->as_integer().value())};

    const auto parse_id_field = [](const JsonValue &parent, const std::string &key,
                                   auto &&target) -> bool {
        const auto *field = parent.find(key);
        if (field == nullptr || !field->is_string()) {
            return false;
        }
        auto parsed = std::decay_t<decltype(target)>::parse(*field->as_string());
        if (!parsed) {
            return false;
        }
        target = *parsed;
        return true;
    };

    if (!parse_id_field(json, "request_id", request.request_id) ||
        !parse_id_field(json, "operation_id", request.operation_id) ||
        !parse_id_field(json, "task_id", request.task_id) ||
        !parse_id_field(json, "profile_id", request.profile_id)) {
        return contract_error("model request identifiers are malformed");
    }
    const auto *task_epoch = json.find("task_epoch");
    if (task_epoch == nullptr || !task_epoch->is_integer()) {
        return contract_error("model request task epoch is malformed");
    }
    request.task_epoch = static_cast<std::uint64_t>(task_epoch->as_integer().value());

    const auto *input = json.find("input");
    if (input == nullptr || !input->is_array()) {
        return contract_error("model request json requires an input array");
    }
    for (const auto &item_json : *input->as_array()) {
        if (!item_json.is_object()) {
            return contract_error("model request input item must be an object");
        }
        ModelInputItem item;
        const auto *role = item_json.find("role");
        if (role == nullptr) {
            return contract_error("model request input item requires a role");
        }
        const auto parsed_role = role_from(*role);
        if (!parsed_role) {
            return contract_error("model request input item has an unknown role");
        }
        item.role = *parsed_role;
        const auto *provenance = item_json.find("provenance");
        if (provenance != nullptr && provenance->is_string()) {
            item.provenance.source = *provenance->as_string();
        }
        const auto *authority = item_json.find("authority");
        if (authority != nullptr && authority->is_string()) {
            const auto parsed = sensitivity_from(*authority);
            if (!parsed) {
                return contract_error("model request input item has unknown authority");
            }
            item.authority = *parsed;
        }
        const auto *content = item_json.find("content");
        if (content == nullptr || !content->is_array()) {
            return contract_error("model request input item requires content");
        }
        for (const auto &part_json : *content->as_array()) {
            if (!part_json.is_object()) {
                return contract_error("model request content part must be an object");
            }
            const auto *kind = part_json.find("kind");
            if (kind == nullptr || !kind->is_string()) {
                return contract_error("model request content part requires a kind");
            }
            const auto &kind_text = *kind->as_string();
            if (kind_text == "text") {
                TextPart part;
                const auto *text = part_json.find("text");
                if (text == nullptr || !text->is_string()) {
                    return contract_error("text part requires text");
                }
                part.text = *text->as_string();
                const auto *sensitivity = part_json.find("sensitivity");
                if (sensitivity != nullptr && sensitivity->is_string()) {
                    const auto parsed = sensitivity_from(*sensitivity);
                    if (!parsed) {
                        return contract_error("text part has unknown sensitivity");
                    }
                    part.sensitivity = *parsed;
                }
                item.content.emplace_back(std::move(part));
            } else if (kind_text == "image") {
                ImagePart part;
                const auto *source = part_json.find("source");
                if (source == nullptr) {
                    return contract_error("image part requires a source");
                }
                auto reference = artifact_ref_from(*source);
                if (!reference) {
                    return reference.error();
                }
                part.source = std::move(reference).value();
                const auto *detail = part_json.find("detail");
                if (detail != nullptr) {
                    const auto parsed = detail_from(*detail);
                    if (!parsed) {
                        return contract_error("image part has unknown detail");
                    }
                    part.detail = *parsed;
                }
                const auto *media_type = part_json.find("media_type");
                if (media_type == nullptr || !media_type->is_string()) {
                    return contract_error("image part requires a media type");
                }
                part.media_type = *media_type->as_string();
                item.content.emplace_back(std::move(part));
            } else if (kind_text == "file") {
                FilePart part;
                const auto *source = part_json.find("source");
                if (source == nullptr) {
                    return contract_error("file part requires a source");
                }
                auto reference = artifact_ref_from(*source);
                if (!reference) {
                    return reference.error();
                }
                part.source = std::move(reference).value();
                const auto *media_type = part_json.find("media_type");
                if (media_type == nullptr || !media_type->is_string()) {
                    return contract_error("file part requires a media type");
                }
                part.media_type = *media_type->as_string();
                const auto *display_name = part_json.find("display_name");
                if (display_name != nullptr && display_name->is_string()) {
                    part.display_name = *display_name->as_string();
                }
                item.content.emplace_back(std::move(part));
            } else {
                return contract_error("model request content part has an unknown kind");
            }
        }
        request.input.emplace_back(std::move(item));
    }

    const auto *output_contract = json.find("output_contract");
    if (output_contract == nullptr || !output_contract->is_object()) {
        return contract_error("model request json requires an output contract");
    }
    const auto *mode = output_contract->find("mode");
    if (mode == nullptr) {
        return contract_error("model request output contract requires a mode");
    }
    const auto parsed_mode = output_mode_from(*mode);
    if (!parsed_mode) {
        return contract_error("model request output contract has an unknown mode");
    }
    request.output_contract.mode = *parsed_mode;
    const auto *schema_id = output_contract->find("schema_id");
    if (schema_id != nullptr && schema_id->is_string()) {
        auto parsed = SchemaId::parse(*schema_id->as_string());
        if (!parsed) {
            return contract_error("model request schema id is malformed");
        }
        request.output_contract.schema_id = *parsed;
    }
    const auto *schema_version = output_contract->find("schema_version");
    if (schema_version != nullptr && schema_version->is_object()) {
        const auto *sv_major = schema_version->find("major");
        const auto *sv_minor = schema_version->find("minor");
        const auto *sv_patch = schema_version->find("patch");
        if (sv_major != nullptr && sv_major->is_integer() && sv_minor != nullptr &&
            sv_minor->is_integer() && sv_patch != nullptr && sv_patch->is_integer()) {
            request.output_contract.schema_version =
                SemanticVersion{static_cast<std::uint16_t>(sv_major->as_integer().value()),
                                static_cast<std::uint16_t>(sv_minor->as_integer().value()),
                                static_cast<std::uint16_t>(sv_patch->as_integer().value())};
        }
    }
    const auto *schema = output_contract->find("schema");
    if (schema != nullptr && schema->is_object()) {
        request.output_contract.schema.root = *schema;
    }
    const auto *schema_digest = output_contract->find("canonical_schema_digest");
    if (schema_digest != nullptr && schema_digest->is_string()) {
        auto parsed = digest_from_hex(*schema_digest->as_string());
        if (!parsed) {
            return contract_error("model request schema digest is malformed");
        }
        request.output_contract.canonical_schema_digest = *parsed;
    }

    const auto *tools = json.find("tools");
    if (tools != nullptr && tools->is_array()) {
        for (const auto &tool_json : *tools->as_array()) {
            if (!tool_json.is_object()) {
                return contract_error("exposed tool json must be an object");
            }
            ExposedToolSpec tool;
            const auto *tool_id = tool_json.find("tool_id");
            if (tool_id != nullptr && tool_id->is_string()) {
                auto parsed = ToolId::parse(*tool_id->as_string());
                if (!parsed) {
                    return contract_error("exposed tool id is malformed");
                }
                tool.tool_id = *parsed;
            }
            const auto *version = tool_json.find("version");
            if (version != nullptr && version->is_object()) {
                const auto *v_major = version->find("major");
                const auto *v_minor = version->find("minor");
                const auto *v_patch = version->find("patch");
                if (v_major != nullptr && v_major->is_integer()) {
                    tool.version.major =
                        static_cast<std::uint16_t>(v_major->as_integer().value());
                }
                if (v_minor != nullptr && v_minor->is_integer()) {
                    tool.version.minor =
                        static_cast<std::uint16_t>(v_minor->as_integer().value());
                }
                if (v_patch != nullptr && v_patch->is_integer()) {
                    tool.version.patch =
                        static_cast<std::uint16_t>(v_patch->as_integer().value());
                }
            }
            const auto *wire_name = tool_json.find("wire_name");
            if (wire_name != nullptr && wire_name->is_string()) {
                tool.wire_name = *wire_name->as_string();
            }
            const auto *description = tool_json.find("description");
            if (description != nullptr && description->is_string()) {
                tool.description = *description->as_string();
            }
            const auto *parameters = tool_json.find("parameters");
            if (parameters != nullptr && parameters->is_object()) {
                tool.parameters_schema.root = *parameters;
            }
            const auto *spec_digest = tool_json.find("spec_digest");
            if (spec_digest != nullptr && spec_digest->is_string()) {
                auto parsed = digest_from_hex(*spec_digest->as_string());
                if (!parsed) {
                    return contract_error("exposed tool digest is malformed");
                }
                tool.spec_digest = *parsed;
            }
            const auto *side_effects = tool_json.find("has_side_effects");
            if (side_effects != nullptr && side_effects->is_boolean()) {
                tool.has_side_effects = *side_effects->as_boolean();
            }
            request.tools.emplace_back(std::move(tool));
        }
    }

    const auto *tool_choice = json.find("tool_choice");
    if (tool_choice != nullptr && tool_choice->is_object()) {
        const auto *choice_mode = tool_choice->find("mode");
        if (choice_mode != nullptr) {
            const auto parsed = tool_choice_mode_from(*choice_mode);
            if (!parsed) {
                return contract_error("tool choice has an unknown mode");
            }
            request.tool_choice.mode = *parsed;
        }
        const auto *required_tool = tool_choice->find("required_tool");
        if (required_tool != nullptr && required_tool->is_string()) {
            auto parsed = ToolId::parse(*required_tool->as_string());
            if (!parsed) {
                return contract_error("tool choice id is malformed");
            }
            request.tool_choice.required_tool = *parsed;
        }
    }

    const auto *generation = json.find("generation");
    if (generation != nullptr && generation->is_object()) {
        const auto read_u64 = [&generation](const char *key, std::optional<std::uint64_t> &out) {
            if (const auto *field = generation->find(key); field != nullptr && field->is_integer()) {
                out = static_cast<std::uint64_t>(field->as_integer().value());
            }
        };
        read_u64("max_output_tokens", request.generation.max_output_tokens);
        read_u64("seed", request.generation.seed);
        if (const auto *field = generation->find("temperature");
            field != nullptr && field->is_number()) {
            request.generation.temperature = field->as_number();
        }
        if (const auto *field = generation->find("top_p"); field != nullptr && field->is_number()) {
            request.generation.top_p = field->as_number();
        }
        if (const auto *field = generation->find("reasoning_effort"); field != nullptr) {
            const auto parsed = reasoning_effort_from(*field);
            if (!parsed) {
                return contract_error("generation has an unknown reasoning effort");
            }
            request.generation.reasoning_effort = parsed;
        }
        if (const auto *field = generation->find("service_tier"); field != nullptr) {
            const auto parsed = service_tier_from(*field);
            if (!parsed) {
                return contract_error("generation has an unknown service tier");
            }
            request.generation.service_tier = parsed;
        }
    }

    const auto *continuation = json.find("continuation");
    if (continuation != nullptr && continuation->is_object()) {
        ProviderContinuation value;
        const auto *provider_state = continuation->find("provider_state");
        if (provider_state == nullptr || !provider_state->is_string()) {
            return contract_error("continuation requires provider state");
        }
        value.provider_state = *provider_state->as_string();
        const auto *previous = continuation->find("previous_response_id");
        if (previous != nullptr && previous->is_string()) {
            value.previous_response_id = *previous->as_string();
        }
        const auto *profile = continuation->find("profile_id");
        if (profile != nullptr && profile->is_string()) {
            auto parsed = ModelProfileId::parse(*profile->as_string());
            if (!parsed) {
                return contract_error("continuation profile id is malformed");
            }
            value.profile_id = *parsed;
        }
        const auto *task = continuation->find("task_id");
        if (task != nullptr && task->is_string()) {
            auto parsed = TaskId::parse(*task->as_string());
            if (!parsed) {
                return contract_error("continuation task id is malformed");
            }
            value.task_id = *parsed;
        }
        const auto *epoch = continuation->find("task_epoch");
        if (epoch != nullptr && epoch->is_integer()) {
            value.task_epoch = static_cast<std::uint64_t>(epoch->as_integer().value());
        }
        request.continuation = std::move(value);
    }

    const auto *budget = json.find("budget");
    if (budget != nullptr && budget->is_object()) {
        const auto read_u64_field = [&budget](const char *key, auto &out) {
            using Target = std::decay_t<decltype(out)>;
            if (const auto *field = budget->find(key); field != nullptr && field->is_integer()) {
                out = static_cast<Target>(field->as_integer().value());
            }
        };
        read_u64_field("max_input_tokens", request.budget.max_input_tokens);
        read_u64_field("max_output_tokens", request.budget.max_output_tokens);
        read_u64_field("max_total_cost_micros", request.budget.max_total_cost_micros);
        read_u64_field("max_requests", request.budget.max_requests);
        read_u64_field("max_image_bytes", request.budget.max_image_bytes);
        if (const auto *field = budget->find("currency"); field != nullptr && field->is_string()) {
            request.budget.currency = *field->as_string();
        }
    }

    const auto *data_policy = json.find("data_policy");
    if (data_policy != nullptr && data_policy->is_object()) {
        if (const auto *field = data_policy->find("store"); field != nullptr && field->is_boolean()) {
            request.data_policy.store = field->as_boolean();
        }
        if (const auto *field = data_policy->find("allow_uploads");
            field != nullptr && field->is_boolean()) {
            request.data_policy.allow_uploads = *field->as_boolean();
        }
        if (const auto *field = data_policy->find("region"); field != nullptr && field->is_string()) {
            request.data_policy.region = *field->as_string();
        }
        if (const auto *field = data_policy->find("organization");
            field != nullptr && field->is_string()) {
            request.data_policy.organization = *field->as_string();
        }
        if (const auto *field = data_policy->find("project"); field != nullptr && field->is_string()) {
            request.data_policy.project = *field->as_string();
        }
        if (const auto *field = data_policy->find("local_raw_retention");
            field != nullptr && field->is_integer()) {
            request.data_policy.local_raw_retention =
                std::chrono::seconds(static_cast<std::int64_t>(field->as_integer().value()));
        }
        if (const auto *field = data_policy->find("remote_retention");
            field != nullptr && field->is_integer()) {
            request.data_policy.remote_retention =
                std::chrono::seconds(static_cast<std::int64_t>(field->as_integer().value()));
        }
    }

    const auto *provenance = json.find("prompt_provenance");
    if (provenance != nullptr && provenance->is_object()) {
        if (const auto *field = provenance->find("system_template_digest");
            field != nullptr && field->is_string()) {
            auto parsed = digest_from_hex(*field->as_string());
            if (!parsed) {
                return contract_error("prompt provenance digest is malformed");
            }
            request.prompt_provenance.system_template_digest = *parsed;
        }
    }
    return request;
}

JsonValue model_response_to_json(const ModelResponse &response) {
    JsonValue::Object root;
    root.emplace_back("contract_version",
                      JsonValue::Object{{"major", static_cast<std::int64_t>(response.contract_version.major)},
                                        {"minor", static_cast<std::int64_t>(response.contract_version.minor)}});
    root.emplace_back("request_id", response.request_id.to_string());
    root.emplace_back("operation_id", response.operation_id.to_string());
    root.emplace_back("profile_id", response.profile_id.to_string());
    root.emplace_back("requested_model", response.requested_model);
    if (response.resolved_model.has_value()) {
        root.emplace_back("resolved_model", *response.resolved_model);
    }
    if (response.provider_response_id.has_value()) {
        root.emplace_back("provider_response_id", *response.provider_response_id);
    }
    if (response.provider_request_id.has_value()) {
        root.emplace_back("provider_request_id", *response.provider_request_id);
    }
    root.emplace_back("status", status_name(response.status));
    if (response.incomplete_reason.has_value()) {
        root.emplace_back("incomplete_reason", incomplete_reason_name(*response.incomplete_reason));
    }

    JsonValue::Array output;
    for (const auto &item : response.output) {
        JsonValue::Object item_json;
        if (const auto *message = std::get_if<MessageOutput>(&item)) {
            item_json.emplace_back("type", "message");
            item_json.emplace_back("role", role_name(message->role));
            JsonValue::Array parts;
            for (const auto &part : message->content) {
                JsonValue::Object part_json;
                if (const auto *text = std::get_if<OutputTextPart>(&part)) {
                    part_json.emplace_back("kind", "text");
                    part_json.emplace_back("text", text->text);
                    if (text->annotations_digest.has_value()) {
                        part_json.emplace_back("annotations_digest", *text->annotations_digest);
                    }
                } else if (const auto *refusal = std::get_if<OutputRefusalPart>(&part)) {
                    part_json.emplace_back("kind", "refusal");
                    part_json.emplace_back("safe_summary", refusal->safe_summary);
                }
                parts.emplace_back(std::move(part_json));
            }
            item_json.emplace_back("content", std::move(parts));
        } else if (const auto *call = std::get_if<ToolCallOutput>(&item)) {
            item_json.emplace_back("type", "tool_call");
            item_json.emplace_back("provider_call_id", call->provider_call_id.value);
            item_json.emplace_back("tool_id", call->tool_id.to_string());
            item_json.emplace_back("provider_name", call->provider_name);
            item_json.emplace_back("arguments", call->arguments);
            item_json.emplace_back("arguments_digest", call->arguments_digest.to_string());
        } else if (const auto *refusal = std::get_if<RefusalOutput>(&item)) {
            item_json.emplace_back("type", "refusal");
            item_json.emplace_back("safe_summary", refusal->safe_summary);
            if (refusal->provider_code.has_value()) {
                item_json.emplace_back("provider_code", *refusal->provider_code);
            }
        } else if (const auto *unknown = std::get_if<UnknownOutput>(&item)) {
            item_json.emplace_back("type", "unknown_output");
            item_json.emplace_back("provider_type", unknown->provider_type);
            item_json.emplace_back("payload_digest", unknown->payload_digest.to_string());
            if (unknown->protected_payload.has_value()) {
                item_json.emplace_back("protected_payload",
                                       artifact_ref_to_json(*unknown->protected_payload));
            }
        }
        output.emplace_back(std::move(item_json));
    }
    root.emplace_back("output", std::move(output));

    JsonValue::Object usage;
    if (response.usage.input_tokens.has_value()) {
        usage.emplace_back("input_tokens", static_cast<std::int64_t>(*response.usage.input_tokens));
    }
    if (response.usage.output_tokens.has_value()) {
        usage.emplace_back("output_tokens", static_cast<std::int64_t>(*response.usage.output_tokens));
    }
    if (response.usage.cached_input_tokens.has_value()) {
        usage.emplace_back("cached_input_tokens",
                           static_cast<std::int64_t>(*response.usage.cached_input_tokens));
    }
    if (response.usage.reasoning_tokens.has_value()) {
        usage.emplace_back("reasoning_tokens",
                           static_cast<std::int64_t>(*response.usage.reasoning_tokens));
    }
    usage.emplace_back("quality", usage_quality_name(response.usage.quality));
    root.emplace_back("usage", std::move(usage));

    JsonValue::Object rate_limit;
    if (response.rate_limit.remaining_requests.has_value()) {
        rate_limit.emplace_back("remaining_requests",
                                static_cast<std::int64_t>(*response.rate_limit.remaining_requests));
    }
    if (response.rate_limit.remaining_tokens.has_value()) {
        rate_limit.emplace_back("remaining_tokens",
                                static_cast<std::int64_t>(*response.rate_limit.remaining_tokens));
    }
    if (response.rate_limit.reset_after.has_value()) {
        rate_limit.emplace_back("reset_after",
                                static_cast<std::int64_t>(response.rate_limit.reset_after->count()));
    }
    root.emplace_back("rate_limit", std::move(rate_limit));

    if (response.protected_raw_response.has_value()) {
        root.emplace_back("protected_raw_response",
                          artifact_ref_to_json(*response.protected_raw_response));
    }
    return JsonValue(std::move(root));
}

Result<ModelResponse> model_response_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error("model response json must be an object");
    }
    ModelResponse response;
    const auto *contract_version = json.find("contract_version");
    if (contract_version == nullptr || !contract_version->is_object()) {
        return contract_error("model response json requires a contract version");
    }
    const auto *major = contract_version->find("major");
    const auto *minor = contract_version->find("minor");
    if (major == nullptr || minor == nullptr || !major->is_integer() || !minor->is_integer()) {
        return contract_error("model response contract version is malformed");
    }
    response.contract_version = SchemaVersion{static_cast<std::uint16_t>(major->as_integer().value()),
                                              static_cast<std::uint16_t>(minor->as_integer().value())};

    const auto parse_id = [](const JsonValue &parent, const std::string &key,
                             auto &&target) -> bool {
        const auto *field = parent.find(key);
        if (field == nullptr || !field->is_string()) {
            return false;
        }
        auto parsed = std::decay_t<decltype(target)>::parse(*field->as_string());
        if (!parsed) {
            return false;
        }
        target = *parsed;
        return true;
    };
    if (!parse_id(json, "request_id", response.request_id) ||
        !parse_id(json, "operation_id", response.operation_id) ||
        !parse_id(json, "profile_id", response.profile_id)) {
        return contract_error("model response identifiers are malformed");
    }
    const auto *requested_model = json.find("requested_model");
    if (requested_model == nullptr || !requested_model->is_string()) {
        return contract_error("model response requires a requested model");
    }
    response.requested_model = *requested_model->as_string();
    const auto read_string = [&json](const char *key) -> std::optional<std::string> {
        if (const auto *field = json.find(key); field != nullptr && field->is_string()) {
            return *field->as_string();
        }
        return std::nullopt;
    };
    response.resolved_model = read_string("resolved_model");
    response.provider_response_id = read_string("provider_response_id");
    response.provider_request_id = read_string("provider_request_id");

    const auto *status = json.find("status");
    if (status == nullptr) {
        return contract_error("model response requires a status");
    }
    const auto parsed_status = status_from(*status);
    if (!parsed_status) {
        return contract_error("model response has an unknown status");
    }
    response.status = *parsed_status;
    const auto *incomplete = json.find("incomplete_reason");
    if (incomplete != nullptr) {
        const auto parsed = incomplete_reason_from(*incomplete);
        if (!parsed) {
            return contract_error("model response has an unknown incomplete reason");
        }
        response.incomplete_reason = parsed;
    }

    const auto *output = json.find("output");
    if (output != nullptr && output->is_array()) {
        for (const auto &item_json : *output->as_array()) {
            if (!item_json.is_object()) {
                return contract_error("model response output item must be an object");
            }
            const auto *type = item_json.find("type");
            if (type == nullptr || !type->is_string()) {
                return contract_error("model response output item requires a type");
            }
            const auto &type_text = *type->as_string();
            if (type_text == "message") {
                MessageOutput message;
                const auto *role = item_json.find("role");
                if (role != nullptr) {
                    const auto parsed = role_from(*role);
                    if (!parsed) {
                        return contract_error("message output has an unknown role");
                    }
                    message.role = *parsed;
                }
                const auto *content = item_json.find("content");
                if (content != nullptr && content->is_array()) {
                    for (const auto &part_json : *content->as_array()) {
                        if (!part_json.is_object()) {
                            return contract_error("message content part must be an object");
                        }
                        const auto *kind = part_json.find("kind");
                        if (kind == nullptr || !kind->is_string()) {
                            return contract_error("message content part requires a kind");
                        }
                        const auto &kind_text = *kind->as_string();
                        if (kind_text == "text") {
                            OutputTextPart part;
                            const auto *text = part_json.find("text");
                            if (text == nullptr || !text->is_string()) {
                                return contract_error("output text part requires text");
                            }
                            part.text = *text->as_string();
                            const auto *annotations = part_json.find("annotations_digest");
                            if (annotations != nullptr && annotations->is_string()) {
                                part.annotations_digest = *annotations->as_string();
                            }
                            message.content.emplace_back(std::move(part));
                        } else if (kind_text == "refusal") {
                            OutputRefusalPart part;
                            const auto *summary = part_json.find("safe_summary");
                            if (summary != nullptr && summary->is_string()) {
                                part.safe_summary = *summary->as_string();
                            }
                            message.content.emplace_back(std::move(part));
                        } else {
                            return contract_error("message content part has an unknown kind");
                        }
                    }
                }
                response.output.emplace_back(std::move(message));
            } else if (type_text == "tool_call") {
                ToolCallOutput call;
                const auto *call_id = item_json.find("provider_call_id");
                if (call_id == nullptr || !call_id->is_string()) {
                    return contract_error("tool call output requires a provider call id");
                }
                call.provider_call_id = ProviderToolCallId{*call_id->as_string()};
                const auto *tool_id = item_json.find("tool_id");
                if (tool_id != nullptr && tool_id->is_string()) {
                    auto parsed = ToolId::parse(*tool_id->as_string());
                    if (!parsed) {
                        return contract_error("tool call output tool id is malformed");
                    }
                    call.tool_id = *parsed;
                }
                const auto *provider_name = item_json.find("provider_name");
                if (provider_name != nullptr && provider_name->is_string()) {
                    call.provider_name = *provider_name->as_string();
                }
                const auto *arguments = item_json.find("arguments");
                if (arguments != nullptr && arguments->is_object()) {
                    call.arguments = *arguments;
                }
                const auto *arguments_digest = item_json.find("arguments_digest");
                if (arguments_digest != nullptr && arguments_digest->is_string()) {
                    auto parsed = digest_from_hex(*arguments_digest->as_string());
                    if (!parsed) {
                        return contract_error("tool call arguments digest is malformed");
                    }
                    call.arguments_digest = *parsed;
                }
                response.output.emplace_back(std::move(call));
            } else if (type_text == "refusal") {
                RefusalOutput refusal;
                const auto *summary = item_json.find("safe_summary");
                if (summary != nullptr && summary->is_string()) {
                    refusal.safe_summary = *summary->as_string();
                }
                const auto *code = item_json.find("provider_code");
                if (code != nullptr && code->is_string()) {
                    refusal.provider_code = *code->as_string();
                }
                response.output.emplace_back(std::move(refusal));
            } else if (type_text == "unknown_output") {
                UnknownOutput unknown;
                const auto *provider_type = item_json.find("provider_type");
                if (provider_type == nullptr || !provider_type->is_string()) {
                    return contract_error("unknown output requires a provider type");
                }
                unknown.provider_type = *provider_type->as_string();
                const auto *payload_digest = item_json.find("payload_digest");
                if (payload_digest != nullptr && payload_digest->is_string()) {
                    auto parsed = digest_from_hex(*payload_digest->as_string());
                    if (!parsed) {
                        return contract_error("unknown output digest is malformed");
                    }
                    unknown.payload_digest = *parsed;
                }
                const auto *protected_payload = item_json.find("protected_payload");
                if (protected_payload != nullptr && protected_payload->is_object()) {
                    auto reference = artifact_ref_from(*protected_payload);
                    if (!reference) {
                        return reference.error();
                    }
                    unknown.protected_payload = std::move(reference).value();
                }
                response.output.emplace_back(std::move(unknown));
            } else {
                // Unknown output item types fail closed instead of decaying to
                // a message or being dropped.
                return contract_error("model response output item has an unknown type");
            }
        }
    }

    const auto *usage = json.find("usage");
    if (usage != nullptr && usage->is_object()) {
        const auto read_count = [&usage](const char *key, std::optional<std::uint64_t> &out) {
            if (const auto *field = usage->find(key); field != nullptr && field->is_integer()) {
                out = static_cast<std::uint64_t>(field->as_integer().value());
            }
        };
        read_count("input_tokens", response.usage.input_tokens);
        read_count("output_tokens", response.usage.output_tokens);
        read_count("cached_input_tokens", response.usage.cached_input_tokens);
        read_count("reasoning_tokens", response.usage.reasoning_tokens);
        const auto *quality = usage->find("quality");
        if (quality != nullptr) {
            const auto parsed = usage_quality_from(*quality);
            if (!parsed) {
                return contract_error("model response has an unknown usage quality");
            }
            response.usage.quality = *parsed;
        }
    }

    const auto *rate_limit = json.find("rate_limit");
    if (rate_limit != nullptr && rate_limit->is_object()) {
        if (const auto *field = rate_limit->find("remaining_requests");
            field != nullptr && field->is_integer()) {
            response.rate_limit.remaining_requests =
                static_cast<std::uint64_t>(field->as_integer().value());
        }
        if (const auto *field = rate_limit->find("remaining_tokens");
            field != nullptr && field->is_integer()) {
            response.rate_limit.remaining_tokens =
                static_cast<std::uint64_t>(field->as_integer().value());
        }
        if (const auto *field = rate_limit->find("reset_after");
            field != nullptr && field->is_integer()) {
            response.rate_limit.reset_after =
                std::chrono::seconds(static_cast<std::int64_t>(field->as_integer().value()));
        }
    }

    const auto *protected_raw = json.find("protected_raw_response");
    if (protected_raw != nullptr && protected_raw->is_object()) {
        auto reference = artifact_ref_from(*protected_raw);
        if (!reference) {
            return reference.error();
        }
        response.protected_raw_response = std::move(reference).value();
    }
    return response;
}

Hash model_request_canonical_digest(const ModelRequest &request) {
    // The canonical form carries artifact digests instead of payload bytes and
    // never contains secrets, so the canonical serialization is directly
    // digestible. Object key order does not affect the digest.
    return canonical_json_digest(model_request_to_json(request));
}

} // namespace mira
