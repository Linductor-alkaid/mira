#include <mira/context_contracts.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error contract_error(ContextDomainCode code, std::string message) {
    return make_context_error(code, std::move(message));
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

[[nodiscard]] std::string authority_name(ContextAuthority authority) {
    switch (authority) {
    case ContextAuthority::SystemPolicy:
        return "system-policy";
    case ContextAuthority::UserConstraint:
        return "user-constraint";
    case ContextAuthority::VerifiedState:
        return "verified-state";
    case ContextAuthority::RetrievedMemory:
        return "retrieved-memory";
    case ContextAuthority::UntrustedExternalData:
        return "untrusted-external-data";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ContextAuthority> authority_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "system-policy") {
        return ContextAuthority::SystemPolicy;
    }
    if (*text == "user-constraint") {
        return ContextAuthority::UserConstraint;
    }
    if (*text == "verified-state") {
        return ContextAuthority::VerifiedState;
    }
    if (*text == "retrieved-memory") {
        return ContextAuthority::RetrievedMemory;
    }
    if (*text == "untrusted-external-data") {
        return ContextAuthority::UntrustedExternalData;
    }
    return std::nullopt;
}

[[nodiscard]] std::string priority_name(ContextPriority priority) {
    switch (priority) {
    case ContextPriority::Essential:
        return "essential";
    case ContextPriority::High:
        return "high";
    case ContextPriority::Normal:
        return "normal";
    case ContextPriority::Low:
        return "low";
    case ContextPriority::Disposable:
        return "disposable";
    }
    return "unknown";
}

[[nodiscard]] std::optional<ContextPriority> priority_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "essential") {
        return ContextPriority::Essential;
    }
    if (*text == "high") {
        return ContextPriority::High;
    }
    if (*text == "normal") {
        return ContextPriority::Normal;
    }
    if (*text == "low") {
        return ContextPriority::Low;
    }
    if (*text == "disposable") {
        return ContextPriority::Disposable;
    }
    return std::nullopt;
}

[[nodiscard]] std::string image_detail_name(ImageDetail detail) {
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
    return "unknown";
}

[[nodiscard]] std::optional<ImageDetail> image_detail_from(const JsonValue &json) {
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
        return contract_error(ContextDomainCode::InvalidItem,
                              "artifact reference must be an object");
    }
    ArtifactRef reference;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "artifact reference requires an id string");
    }
    const auto parsed_id = ArtifactId::parse(*id->as_string());
    if (!parsed_id) {
        return contract_error(ContextDomainCode::InvalidItem, "artifact reference id is malformed");
    }
    reference.id = *parsed_id;
    const auto *digest = json.find("digest");
    if (digest != nullptr && digest->is_string()) {
        auto parsed = digest_from_hex(*digest->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "artifact reference digest is malformed");
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
            return contract_error(ContextDomainCode::InvalidItem,
                                  "artifact reference has unknown sensitivity");
        }
        reference.sensitivity = *parsed;
    }
    return reference;
}

[[nodiscard]] JsonValue content_part_to_json(const ModelContentPart &part) {
    JsonValue::Object object;
    if (const auto *text = std::get_if<TextPart>(&part)) {
        object.emplace_back("type", "text");
        object.emplace_back("text", text->text);
        object.emplace_back("sensitivity", sensitivity_name(text->sensitivity));
    } else if (const auto *image = std::get_if<ImagePart>(&part)) {
        object.emplace_back("type", "image");
        object.emplace_back("source", artifact_ref_to_json(image->source));
        object.emplace_back("detail", image_detail_name(image->detail));
        object.emplace_back("media_type", image->media_type);
    } else {
        const auto &file = std::get<FilePart>(part);
        object.emplace_back("type", "file");
        object.emplace_back("source", artifact_ref_to_json(file.source));
        object.emplace_back("media_type", file.media_type);
        object.emplace_back("display_name", file.display_name);
    }
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ModelContentPart> content_part_from(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::InvalidItem, "content part must be an object");
    }
    const auto *type = json.find("type");
    if (type == nullptr || !type->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem, "content part requires a type");
    }
    if (*type->as_string() == "text") {
        const auto *text = json.find("text");
        if (text == nullptr || !text->is_string()) {
            return contract_error(ContextDomainCode::InvalidItem, "text part requires text");
        }
        TextPart part;
        part.text = *text->as_string();
        const auto *sensitivity = json.find("sensitivity");
        if (sensitivity != nullptr && sensitivity->is_string()) {
            const auto parsed = sensitivity_from(*sensitivity);
            if (!parsed) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "text part has unknown sensitivity");
            }
            part.sensitivity = *parsed;
        }
        return ModelContentPart(std::move(part));
    }
    const auto *source = json.find("source");
    if (source == nullptr) {
        return contract_error(ContextDomainCode::InvalidItem, "binary part requires a source");
    }
    auto reference = artifact_ref_from(*source);
    if (!reference) {
        return reference.error();
    }
    if (*type->as_string() == "image") {
        ImagePart part;
        part.source = std::move(reference).value();
        const auto *detail = json.find("detail");
        if (detail != nullptr && detail->is_string()) {
            const auto parsed = image_detail_from(*detail);
            if (!parsed) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "image part has unknown detail");
            }
            part.detail = *parsed;
        }
        const auto *media_type = json.find("media_type");
        if (media_type != nullptr && media_type->is_string()) {
            part.media_type = *media_type->as_string();
        }
        return ModelContentPart(std::move(part));
    }
    if (*type->as_string() == "file") {
        FilePart part;
        part.source = std::move(reference).value();
        const auto *media_type = json.find("media_type");
        if (media_type != nullptr && media_type->is_string()) {
            part.media_type = *media_type->as_string();
        }
        const auto *display_name = json.find("display_name");
        if (display_name != nullptr && display_name->is_string()) {
            part.display_name = *display_name->as_string();
        }
        return ModelContentPart(std::move(part));
    }
    return contract_error(ContextDomainCode::InvalidItem, "content part has unknown type");
}

[[nodiscard]] std::uint64_t saturating_sub(std::uint64_t value, std::uint64_t amount) noexcept {
    return value > amount ? value - amount : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ContextLimits
// ---------------------------------------------------------------------------

std::uint64_t ContextLimits::input_budget_tokens() const noexcept {
    return saturating_sub(
        saturating_sub(saturating_sub(context_window_tokens, reserved_output_tokens),
                       safety_margin_tokens),
        provider_overhead_tokens);
}

std::uint64_t ContextLimits::watermark_tokens(double watermark) const noexcept {
    const auto budget = static_cast<double>(input_budget_tokens());
    const auto tokens = budget * watermark;
    if (tokens <= 0.0 || std::isnan(tokens)) {
        return 0;
    }
    if (tokens >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(tokens);
}

Result<void> ContextLimits::validate() const {
    if (context_window_tokens == 0) {
        return contract_error(ContextDomainCode::InvalidLimits, "context window must be positive");
    }
    if (reserved_output_tokens + safety_margin_tokens >= context_window_tokens) {
        return contract_error(ContextDomainCode::InvalidLimits,
                              "reserved output plus safety margin must leave input budget");
    }
    if (provider_overhead_tokens >= context_window_tokens) {
        return contract_error(ContextDomainCode::InvalidLimits,
                              "provider overhead must leave input budget");
    }
    if (!(trim_watermark > 0.0) || !(trim_watermark < checkpoint_watermark) ||
        !(checkpoint_watermark < hard_watermark) || !(hard_watermark < 1.0)) {
        return contract_error(ContextDomainCode::InvalidLimits,
                              "watermarks must satisfy 0 < trim < checkpoint < hard < 1");
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string token_count_quality_name(TokenCountQuality quality) {
    switch (quality) {
    case TokenCountQuality::ExactProviderCount:
        return "ExactProviderCount";
    case TokenCountQuality::ExactLocalTokenizer:
        return "ExactLocalTokenizer";
    case TokenCountQuality::ConservativeEstimate:
        return "ConservativeEstimate";
    case TokenCountQuality::DegradedEstimate:
        return "DegradedEstimate";
    }
    return "Unknown";
}

std::string context_authority_name(ContextAuthority authority) { return authority_name(authority); }

std::string context_item_kind_name(ContextItemKind kind) {
    switch (kind) {
    case ContextItemKind::SystemPolicy:
        return "system-policy";
    case ContextItemKind::Goal:
        return "goal";
    case ContextItemKind::UserConstraint:
        return "user-constraint";
    case ContextItemKind::TaskLimits:
        return "task-limits";
    case ContextItemKind::CurrentObservation:
        return "current-observation";
    case ContextItemKind::UncertainSideEffect:
        return "uncertain-side-effect";
    case ContextItemKind::VerificationResult:
        return "verification-result";
    case ContextItemKind::CheckpointSummary:
        return "checkpoint-summary";
    case ContextItemKind::RecentAction:
        return "recent-action";
    case ContextItemKind::RecentError:
        return "recent-error";
    case ContextItemKind::RetrievedMemory:
        return "retrieved-memory";
    case ContextItemKind::ToolCall:
        return "tool-call";
    case ContextItemKind::ToolResult:
        return "tool-result";
    case ContextItemKind::HistoricalPayload:
        return "historical-payload";
    }
    return "unknown";
}

std::optional<ContextItemKind> context_item_kind_from(std::string_view name) {
    static const std::pair<std::string_view, ContextItemKind> kKinds[] = {
        {"system-policy", ContextItemKind::SystemPolicy},
        {"goal", ContextItemKind::Goal},
        {"user-constraint", ContextItemKind::UserConstraint},
        {"task-limits", ContextItemKind::TaskLimits},
        {"current-observation", ContextItemKind::CurrentObservation},
        {"uncertain-side-effect", ContextItemKind::UncertainSideEffect},
        {"verification-result", ContextItemKind::VerificationResult},
        {"checkpoint-summary", ContextItemKind::CheckpointSummary},
        {"recent-action", ContextItemKind::RecentAction},
        {"recent-error", ContextItemKind::RecentError},
        {"retrieved-memory", ContextItemKind::RetrievedMemory},
        {"tool-call", ContextItemKind::ToolCall},
        {"tool-result", ContextItemKind::ToolResult},
        {"historical-payload", ContextItemKind::HistoricalPayload},
    };
    for (const auto &[kind_name, kind] : kKinds) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

ContextPartition partition_of(ContextItemKind kind) noexcept {
    switch (kind) {
    case ContextItemKind::SystemPolicy:
        return ContextPartition::P0Policy;
    case ContextItemKind::Goal:
    case ContextItemKind::UserConstraint:
    case ContextItemKind::TaskLimits:
        return ContextPartition::P1TaskFrame;
    case ContextItemKind::CurrentObservation:
    case ContextItemKind::UncertainSideEffect:
    case ContextItemKind::VerificationResult:
        return ContextPartition::P2State;
    case ContextItemKind::CheckpointSummary:
    case ContextItemKind::RecentAction:
    case ContextItemKind::RecentError:
        return ContextPartition::P3Progress;
    case ContextItemKind::RetrievedMemory:
        return ContextPartition::P4Memory;
    case ContextItemKind::ToolCall:
    case ContextItemKind::ToolResult:
    case ContextItemKind::HistoricalPayload:
        return ContextPartition::P5History;
    }
    return ContextPartition::P5History;
}

std::string context_item_disposition_name(ContextItemDisposition disposition) {
    switch (disposition) {
    case ContextItemDisposition::Selected:
        return "Selected";
    case ContextItemDisposition::SelectedByReference:
        return "SelectedByReference";
    case ContextItemDisposition::Compressed:
        return "Compressed";
    case ContextItemDisposition::Dropped:
        return "Dropped";
    case ContextItemDisposition::RejectedMinimumSet:
        return "RejectedMinimumSet";
    }
    return "Unknown";
}

std::string context_watermark_name(ContextWatermark watermark) {
    switch (watermark) {
    case ContextWatermark::Normal:
        return "Normal";
    case ContextWatermark::Trim:
        return "Trim";
    case ContextWatermark::Checkpoint:
        return "Checkpoint";
    case ContextWatermark::Hard:
        return "Hard";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void> ContextItem::validate() const {
    if (id.is_nil()) {
        return contract_error(ContextDomainCode::InvalidItem, "context item id must be non-nil");
    }
    if (content.empty()) {
        return contract_error(ContextDomainCode::InvalidItem, "context item requires content");
    }
    // SystemPolicy authority is reserved for policy items produced by trusted
    // runtime code; external text can never enter through this door.
    if (kind == ContextItemKind::SystemPolicy && authority != ContextAuthority::SystemPolicy) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "system policy items require system policy authority");
    }
    if (kind != ContextItemKind::SystemPolicy && authority == ContextAuthority::SystemPolicy) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "system policy authority is reserved for policy items");
    }
    if (kind == ContextItemKind::UserConstraint && authority != ContextAuthority::UserConstraint &&
        authority != ContextAuthority::SystemPolicy) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "user constraints require user or system authority");
    }
    if (kind == ContextItemKind::RetrievedMemory &&
        authority != ContextAuthority::RetrievedMemory) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "retrieved memory items require retrieved memory authority");
    }
    if ((kind == ContextItemKind::ToolCall || kind == ContextItemKind::ToolResult) &&
        (!tool_call_key.has_value() || tool_call_key->empty())) {
        return contract_error(ContextDomainCode::InvalidItem, "tool items require a tool call key");
    }
    if (kind != ContextItemKind::ToolCall && kind != ContextItemKind::ToolResult &&
        tool_call_key.has_value()) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "tool call keys are only valid on tool items");
    }
    for (const auto &part : content) {
        if (const auto *text = std::get_if<TextPart>(&part)) {
            if (text->sensitivity == Sensitivity::Secret) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "secret content must not enter model context");
            }
        }
        if (const auto *image = std::get_if<ImagePart>(&part)) {
            if (image->source.sensitivity == Sensitivity::Secret) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "secret payloads must not enter model context");
            }
        }
        if (const auto *file = std::get_if<FilePart>(&part)) {
            if (file->source.sensitivity == Sensitivity::Secret) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "secret payloads must not enter model context");
            }
        }
    }
    return Result<void>{};
}

Result<void> ContextRequest::validate() const {
    const auto schema = validate_schema_version(contract_version, context_contract_version());
    if (!schema) {
        return schema.error();
    }
    if (task_id.is_nil() || session_id.is_nil()) {
        return contract_error(ContextDomainCode::InvalidItem, "task and session ids are required");
    }
    if (profile_id.is_nil() && !authorized_large_window_profile.has_value()) {
        return contract_error(ContextDomainCode::InvalidItem, "a profile id is required");
    }
    const auto limits_result = limits.validate();
    if (!limits_result) {
        return limits_result.error();
    }
    if (authorized_large_window_profile.has_value()) {
        const auto large = authorized_large_window_limits.validate();
        if (!large) {
            return large.error();
        }
    }
    for (const auto &item : items) {
        const auto item_result = item.validate();
        if (!item_result) {
            return item_result.error();
        }
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Error domain
// ---------------------------------------------------------------------------

std::string context_domain_code_name(ContextDomainCode code) {
    switch (code) {
    case ContextDomainCode::MinimumSetTooLarge:
        return "MinimumSetTooLarge";
    case ContextDomainCode::StaleBuild:
        return "StaleBuild";
    case ContextDomainCode::InvalidLimits:
        return "InvalidLimits";
    case ContextDomainCode::InvalidItem:
        return "InvalidItem";
    case ContextDomainCode::TokenCountUnavailable:
        return "TokenCountUnavailable";
    case ContextDomainCode::ToolPairingBroken:
        return "ToolPairingBroken";
    case ContextDomainCode::SchemaUnsupported:
        return "SchemaUnsupported";
    }
    return "Unknown";
}

Error make_context_error(ContextDomainCode code, std::string safe_message, bool retryable,
                         std::optional<OperationId> operation) {
    Error error;
    error.code = ErrorCode::InvalidState;
    if (code == ContextDomainCode::MinimumSetTooLarge) {
        error.code = ErrorCode::ResourceExhausted;
    } else if (code == ContextDomainCode::StaleBuild) {
        error.code = ErrorCode::InvalidState;
    } else if (code == ContextDomainCode::InvalidLimits || code == ContextDomainCode::InvalidItem ||
               code == ContextDomainCode::ToolPairingBroken ||
               code == ContextDomainCode::SchemaUnsupported) {
        error.code = ErrorCode::InvalidArgument;
    } else if (code == ContextDomainCode::TokenCountUnavailable) {
        error.code = ErrorCode::Unavailable;
    }
    error.domain = "mira.context";
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = retryable;
    error.safe_message = std::move(safe_message);
    error.operation_id = operation;
    return error;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

JsonValue context_limits_to_json(const ContextLimits &limits) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(context_contract_version().major)},
                          {"minor", static_cast<std::int64_t>(context_contract_version().minor)}});
    object.emplace_back("context_window_tokens",
                        static_cast<std::int64_t>(limits.context_window_tokens));
    object.emplace_back("reserved_output_tokens",
                        static_cast<std::int64_t>(limits.reserved_output_tokens));
    object.emplace_back("safety_margin_tokens",
                        static_cast<std::int64_t>(limits.safety_margin_tokens));
    object.emplace_back("provider_overhead_tokens",
                        static_cast<std::int64_t>(limits.provider_overhead_tokens));
    object.emplace_back("max_image_tokens", static_cast<std::int64_t>(limits.max_image_tokens));
    object.emplace_back("max_tool_schema_tokens",
                        static_cast<std::int64_t>(limits.max_tool_schema_tokens));
    object.emplace_back("trim_watermark", limits.trim_watermark);
    object.emplace_back("checkpoint_watermark", limits.checkpoint_watermark);
    object.emplace_back("hard_watermark", limits.hard_watermark);
    return JsonValue(std::move(object));
}

Result<ContextLimits> context_limits_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported, "limits must be an object");
    }
    ContextLimits limits;
    const auto *window = json.find("context_window_tokens");
    if (window != nullptr && window->is_integer()) {
        limits.context_window_tokens = static_cast<std::uint64_t>(window->as_integer().value());
    }
    const auto *reserved = json.find("reserved_output_tokens");
    if (reserved != nullptr && reserved->is_integer()) {
        limits.reserved_output_tokens = static_cast<std::uint64_t>(reserved->as_integer().value());
    }
    const auto *safety = json.find("safety_margin_tokens");
    if (safety != nullptr && safety->is_integer()) {
        limits.safety_margin_tokens = static_cast<std::uint64_t>(safety->as_integer().value());
    }
    const auto *overhead = json.find("provider_overhead_tokens");
    if (overhead != nullptr && overhead->is_integer()) {
        limits.provider_overhead_tokens =
            static_cast<std::uint64_t>(overhead->as_integer().value());
    }
    const auto *image = json.find("max_image_tokens");
    if (image != nullptr && image->is_integer()) {
        limits.max_image_tokens = static_cast<std::uint64_t>(image->as_integer().value());
    }
    const auto *tool = json.find("max_tool_schema_tokens");
    if (tool != nullptr && tool->is_integer()) {
        limits.max_tool_schema_tokens = static_cast<std::uint64_t>(tool->as_integer().value());
    }
    const auto *trim = json.find("trim_watermark");
    if (trim != nullptr && trim->is_number()) {
        limits.trim_watermark = trim->as_number().value();
    }
    const auto *checkpoint = json.find("checkpoint_watermark");
    if (checkpoint != nullptr && checkpoint->is_number()) {
        limits.checkpoint_watermark = checkpoint->as_number().value();
    }
    const auto *hard = json.find("hard_watermark");
    if (hard != nullptr && hard->is_number()) {
        limits.hard_watermark = hard->as_number().value();
    }
    return limits;
}

JsonValue context_item_to_json(const ContextItem &item) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(context_contract_version().major)},
                          {"minor", static_cast<std::int64_t>(context_contract_version().minor)}});
    object.emplace_back("id", item.id.to_string());
    object.emplace_back("kind", context_item_kind_name(item.kind));
    object.emplace_back("authority", authority_name(item.authority));
    object.emplace_back("priority", priority_name(item.priority));
    JsonValue::Array content;
    for (const auto &part : item.content) {
        content.emplace_back(content_part_to_json(part));
    }
    object.emplace_back("content", JsonValue(std::move(content)));
    if (item.payload.has_value()) {
        object.emplace_back("payload", artifact_ref_to_json(*item.payload));
    }
    JsonValue::Array provenance;
    for (const auto &event : item.provenance) {
        provenance.emplace_back(event.to_string());
    }
    object.emplace_back("provenance", JsonValue(std::move(provenance)));
    object.emplace_back("pinned", item.pinned);
    object.emplace_back("replaceable_by_reference", item.replaceable_by_reference);
    object.emplace_back("consumed", item.consumed);
    if (item.tool_call_key.has_value()) {
        object.emplace_back("tool_call_key", *item.tool_call_key);
    }
    object.emplace_back("sequence", static_cast<std::int64_t>(item.sequence));
    if (item.task_epoch.has_value()) {
        object.emplace_back("task_epoch", static_cast<std::int64_t>(*item.task_epoch));
    }
    if (item.environment_epoch.has_value()) {
        object.emplace_back("environment_epoch",
                            static_cast<std::int64_t>(*item.environment_epoch));
    }
    return JsonValue(std::move(object));
}

Result<ContextItem> context_item_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported,
                              "context item must be an object");
    }
    ContextItem item;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem, "context item requires an id");
    }
    const auto parsed_id = ContextItemId::parse(*id->as_string());
    if (!parsed_id) {
        return contract_error(ContextDomainCode::InvalidItem, "context item id is malformed");
    }
    item.id = *parsed_id;
    const auto *kind = json.find("kind");
    if (kind == nullptr || !kind->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem, "context item requires a kind");
    }
    const auto parsed_kind = context_item_kind_from(*kind->as_string());
    if (!parsed_kind) {
        return contract_error(ContextDomainCode::InvalidItem, "context item kind is unknown");
    }
    item.kind = *parsed_kind;
    const auto *authority = json.find("authority");
    if (authority != nullptr && authority->is_string()) {
        const auto parsed = authority_from(*authority);
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "context item authority is unknown");
        }
        item.authority = *parsed;
    }
    const auto *priority = json.find("priority");
    if (priority != nullptr && priority->is_string()) {
        const auto parsed = priority_from(*priority);
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "context item priority is unknown");
        }
        item.priority = *parsed;
    }
    const auto *content = json.find("content");
    if (content == nullptr || !content->is_array()) {
        return contract_error(ContextDomainCode::InvalidItem, "context item requires content");
    }
    for (const auto &part : *content->as_array()) {
        auto parsed = content_part_from(part);
        if (!parsed) {
            return parsed.error();
        }
        item.content.emplace_back(std::move(parsed).value());
    }
    const auto *payload = json.find("payload");
    if (payload != nullptr && !payload->is_null()) {
        auto parsed = artifact_ref_from(*payload);
        if (!parsed) {
            return parsed.error();
        }
        item.payload = std::move(parsed).value();
    }
    const auto *provenance = json.find("provenance");
    if (provenance != nullptr && provenance->is_array()) {
        for (const auto &event : *provenance->as_array()) {
            if (!event.is_string()) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "provenance entries must be event ids");
            }
            const auto parsed = EventId::parse(*event.as_string());
            if (!parsed) {
                return contract_error(ContextDomainCode::InvalidItem,
                                      "provenance event id is malformed");
            }
            item.provenance.emplace_back(*parsed);
        }
    }
    const auto read_flag = [&json](const char *key, bool &field) {
        if (const auto *flag = json.find(key); flag != nullptr && flag->is_boolean()) {
            field = flag->as_boolean().value();
        }
    };
    read_flag("pinned", item.pinned);
    read_flag("replaceable_by_reference", item.replaceable_by_reference);
    read_flag("consumed", item.consumed);
    const auto *tool_key = json.find("tool_call_key");
    if (tool_key != nullptr && tool_key->is_string()) {
        item.tool_call_key = *tool_key->as_string();
    }
    const auto *sequence = json.find("sequence");
    if (sequence != nullptr && sequence->is_integer()) {
        item.sequence = static_cast<std::uint64_t>(sequence->as_integer().value());
    }
    const auto *task_epoch = json.find("task_epoch");
    if (task_epoch != nullptr && task_epoch->is_integer()) {
        item.task_epoch = static_cast<std::uint64_t>(task_epoch->as_integer().value());
    }
    const auto *environment_epoch = json.find("environment_epoch");
    if (environment_epoch != nullptr && environment_epoch->is_integer()) {
        item.environment_epoch =
            static_cast<std::uint64_t>(environment_epoch->as_integer().value());
    }
    return item;
}

namespace {

[[nodiscard]] JsonValue budget_report_to_json(const ContextBudgetReport &report) {
    JsonValue::Object object;
    object.emplace_back("input_budget_tokens",
                        static_cast<std::int64_t>(report.input_budget_tokens));
    object.emplace_back("estimated_tokens", static_cast<std::int64_t>(report.estimated_tokens));
    object.emplace_back("count_quality", token_count_quality_name(report.quality));
    object.emplace_back("utilization", report.utilization);
    object.emplace_back("watermark", context_watermark_name(report.watermark));
    object.emplace_back("routed_to_large_window", report.routed_to_large_window);
    if (report.routed_profile.has_value()) {
        object.emplace_back("routed_profile", report.routed_profile->to_string());
    }
    object.emplace_back("checkpoint_recommended", report.checkpoint_recommended);
    object.emplace_back("selected_items", static_cast<std::int64_t>(report.selected_items));
    object.emplace_back("replaced_items", static_cast<std::int64_t>(report.replaced_items));
    object.emplace_back("compressed_items", static_cast<std::int64_t>(report.compressed_items));
    object.emplace_back("dropped_items", static_cast<std::int64_t>(report.dropped_items));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ContextBudgetReport> budget_report_from(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported,
                              "budget report must be an object");
    }
    ContextBudgetReport report;
    const auto *budget = json.find("input_budget_tokens");
    if (budget != nullptr && budget->is_integer()) {
        report.input_budget_tokens = static_cast<std::uint64_t>(budget->as_integer().value());
    }
    const auto *estimated = json.find("estimated_tokens");
    if (estimated != nullptr && estimated->is_integer()) {
        report.estimated_tokens = static_cast<std::uint64_t>(estimated->as_integer().value());
    }
    const auto *quality = json.find("count_quality");
    if (quality != nullptr && quality->is_string()) {
        const std::string_view name = *quality->as_string();
        if (name == "ExactProviderCount") {
            report.quality = TokenCountQuality::ExactProviderCount;
        } else if (name == "ExactLocalTokenizer") {
            report.quality = TokenCountQuality::ExactLocalTokenizer;
        } else if (name == "DegradedEstimate") {
            report.quality = TokenCountQuality::DegradedEstimate;
        }
    }
    const auto *utilization = json.find("utilization");
    if (utilization != nullptr && utilization->is_number()) {
        report.utilization = utilization->as_number().value();
    }
    const auto *watermark = json.find("watermark");
    if (watermark != nullptr && watermark->is_string()) {
        const std::string_view name = *watermark->as_string();
        if (name == "Trim") {
            report.watermark = ContextWatermark::Trim;
        } else if (name == "Checkpoint") {
            report.watermark = ContextWatermark::Checkpoint;
        } else if (name == "Hard") {
            report.watermark = ContextWatermark::Hard;
        }
    }
    const auto *routed = json.find("routed_to_large_window");
    if (routed != nullptr && routed->is_boolean()) {
        report.routed_to_large_window = routed->as_boolean().value();
    }
    const auto *routed_profile = json.find("routed_profile");
    if (routed_profile != nullptr && routed_profile->is_string()) {
        const auto parsed = ModelProfileId::parse(*routed_profile->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem, "routed profile id is malformed");
        }
        report.routed_profile = *parsed;
    }
    const auto *recommended = json.find("checkpoint_recommended");
    if (recommended != nullptr && recommended->is_boolean()) {
        report.checkpoint_recommended = recommended->as_boolean().value();
    }
    const auto read_size = [&json](const char *key, std::size_t &field) {
        if (const auto *value = json.find(key); value != nullptr && value->is_integer()) {
            const auto number = value->as_integer().value();
            field = number > 0 ? static_cast<std::size_t>(number) : 0;
        }
    };
    read_size("selected_items", report.selected_items);
    read_size("replaced_items", report.replaced_items);
    read_size("compressed_items", report.compressed_items);
    read_size("dropped_items", report.dropped_items);
    return report;
}

[[nodiscard]] JsonValue item_audit_to_json(const ContextItemAudit &audit) {
    JsonValue::Object object;
    object.emplace_back("id", audit.id.to_string());
    object.emplace_back("kind", context_item_kind_name(audit.kind));
    object.emplace_back("disposition", context_item_disposition_name(audit.disposition));
    object.emplace_back("reason", audit.reason);
    object.emplace_back("estimated_tokens", static_cast<std::int64_t>(audit.estimated_tokens));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ContextItemAudit> item_audit_from(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported, "item audit must be an object");
    }
    ContextItemAudit audit;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem, "item audit requires an id");
    }
    const auto parsed_id = ContextItemId::parse(*id->as_string());
    if (!parsed_id) {
        return contract_error(ContextDomainCode::InvalidItem, "item audit id is malformed");
    }
    audit.id = *parsed_id;
    const auto *kind = json.find("kind");
    if (kind != nullptr && kind->is_string()) {
        const auto parsed = context_item_kind_from(*kind->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem, "item audit kind is unknown");
        }
        audit.kind = *parsed;
    }
    const auto *disposition = json.find("disposition");
    if (disposition != nullptr && disposition->is_string()) {
        const std::string_view name = *disposition->as_string();
        if (name == "SelectedByReference") {
            audit.disposition = ContextItemDisposition::SelectedByReference;
        } else if (name == "Compressed") {
            audit.disposition = ContextItemDisposition::Compressed;
        } else if (name == "Dropped") {
            audit.disposition = ContextItemDisposition::Dropped;
        } else if (name == "RejectedMinimumSet") {
            audit.disposition = ContextItemDisposition::RejectedMinimumSet;
        }
    }
    const auto *reason = json.find("reason");
    if (reason != nullptr && reason->is_string()) {
        audit.reason = *reason->as_string();
    }
    const auto *tokens = json.find("estimated_tokens");
    if (tokens != nullptr && tokens->is_integer()) {
        audit.estimated_tokens = static_cast<std::uint64_t>(tokens->as_integer().value());
    }
    return audit;
}

[[nodiscard]] JsonValue tool_audit_to_json(const ContextToolAudit &audit) {
    JsonValue::Object object;
    object.emplace_back("tool_id", audit.tool_id.to_string());
    object.emplace_back("selected", audit.selected);
    object.emplace_back("reason", audit.reason);
    object.emplace_back("estimated_tokens", static_cast<std::int64_t>(audit.estimated_tokens));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ContextToolAudit> tool_audit_from(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported, "tool audit must be an object");
    }
    ContextToolAudit audit;
    const auto *id = json.find("tool_id");
    if (id == nullptr || !id->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem, "tool audit requires a tool id");
    }
    const auto parsed = ToolId::parse(*id->as_string());
    if (!parsed) {
        return contract_error(ContextDomainCode::InvalidItem, "tool audit tool id is malformed");
    }
    audit.tool_id = *parsed;
    const auto *selected = json.find("selected");
    if (selected != nullptr && selected->is_boolean()) {
        audit.selected = selected->as_boolean().value();
    }
    const auto *reason = json.find("reason");
    if (reason != nullptr && reason->is_string()) {
        audit.reason = *reason->as_string();
    }
    const auto *tokens = json.find("estimated_tokens");
    if (tokens != nullptr && tokens->is_integer()) {
        audit.estimated_tokens = static_cast<std::uint64_t>(tokens->as_integer().value());
    }
    return audit;
}

} // namespace

JsonValue prepared_context_to_json(const PreparedModelContext &prepared) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(context_contract_version().major)},
                          {"minor", static_cast<std::int64_t>(context_contract_version().minor)}});
    object.emplace_back("task_id", prepared.task_id.to_string());
    object.emplace_back("session_id", prepared.session_id.to_string());
    object.emplace_back("task_epoch", static_cast<std::int64_t>(prepared.task_epoch));
    object.emplace_back("environment_epoch", static_cast<std::int64_t>(prepared.environment_epoch));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(prepared.through_event_sequence));
    object.emplace_back("profile_id", prepared.profile_id.to_string());
    object.emplace_back("input_items", static_cast<std::int64_t>(prepared.input.size()));
    object.emplace_back("budget", budget_report_to_json(prepared.budget));
    JsonValue::Array item_audit;
    for (const auto &audit : prepared.item_audit) {
        item_audit.emplace_back(item_audit_to_json(audit));
    }
    object.emplace_back("item_audit", JsonValue(std::move(item_audit)));
    JsonValue::Array tool_audit;
    for (const auto &audit : prepared.tool_audit) {
        tool_audit.emplace_back(tool_audit_to_json(audit));
    }
    object.emplace_back("tool_audit", JsonValue(std::move(tool_audit)));
    object.emplace_back("total_estimate_upper",
                        static_cast<std::int64_t>(prepared.total_estimate.upper_bound));
    object.emplace_back("total_estimate_quality",
                        token_count_quality_name(prepared.total_estimate.quality));
    object.emplace_back("selection_digest", prepared.selection_digest.to_string());
    return JsonValue(std::move(object));
}

Result<PreparedModelContext> prepared_context_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return contract_error(ContextDomainCode::SchemaUnsupported,
                              "prepared context must be an object");
    }
    PreparedModelContext prepared;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto *major = schema->find("major");
        const auto *minor = schema->find("minor");
        SchemaVersion version;
        if (major != nullptr && major->is_integer() && minor != nullptr && minor->is_integer()) {
            version.major = static_cast<std::uint16_t>(major->as_integer().value());
            version.minor = static_cast<std::uint16_t>(minor->as_integer().value());
        }
        const auto supported = validate_schema_version(version, context_contract_version());
        if (!supported) {
            return supported.error();
        }
    }
    const auto *task = json.find("task_id");
    if (task == nullptr || !task->is_string()) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "prepared context requires a task id");
    }
    const auto parsed_task = TaskId::parse(*task->as_string());
    if (!parsed_task) {
        return contract_error(ContextDomainCode::InvalidItem,
                              "prepared context task id is malformed");
    }
    prepared.task_id = *parsed_task;
    const auto *session = json.find("session_id");
    if (session != nullptr && session->is_string()) {
        const auto parsed = SessionId::parse(*session->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "prepared context session id is malformed");
        }
        prepared.session_id = *parsed;
    }
    const auto read_u64 = [&json](const char *key, std::uint64_t &field) {
        if (const auto *value = json.find(key); value != nullptr && value->is_integer()) {
            field = static_cast<std::uint64_t>(value->as_integer().value());
        }
    };
    read_u64("task_epoch", prepared.task_epoch);
    read_u64("environment_epoch", prepared.environment_epoch);
    read_u64("through_event_sequence", prepared.through_event_sequence);
    const auto *profile = json.find("profile_id");
    if (profile != nullptr && profile->is_string()) {
        const auto parsed = ModelProfileId::parse(*profile->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "prepared context profile id is malformed");
        }
        prepared.profile_id = *parsed;
    }
    const auto *budget = json.find("budget");
    if (budget != nullptr) {
        auto parsed = budget_report_from(*budget);
        if (!parsed) {
            return parsed.error();
        }
        prepared.budget = std::move(parsed).value();
    }
    const auto *item_audit = json.find("item_audit");
    if (item_audit != nullptr && item_audit->is_array()) {
        for (const auto &entry : *item_audit->as_array()) {
            auto parsed = item_audit_from(entry);
            if (!parsed) {
                return parsed.error();
            }
            prepared.item_audit.emplace_back(std::move(parsed).value());
        }
    }
    const auto *tool_audit = json.find("tool_audit");
    if (tool_audit != nullptr && tool_audit->is_array()) {
        for (const auto &entry : *tool_audit->as_array()) {
            auto parsed = tool_audit_from(entry);
            if (!parsed) {
                return parsed.error();
            }
            prepared.tool_audit.emplace_back(std::move(parsed).value());
        }
    }
    read_u64("total_estimate_upper", prepared.total_estimate.upper_bound);
    const auto *quality = json.find("total_estimate_quality");
    if (quality != nullptr && quality->is_string()) {
        const std::string_view name = *quality->as_string();
        if (name == "ExactProviderCount") {
            prepared.total_estimate.quality = TokenCountQuality::ExactProviderCount;
        } else if (name == "ExactLocalTokenizer") {
            prepared.total_estimate.quality = TokenCountQuality::ExactLocalTokenizer;
        } else if (name == "DegradedEstimate") {
            prepared.total_estimate.quality = TokenCountQuality::DegradedEstimate;
        }
    }
    const auto *digest = json.find("selection_digest");
    if (digest != nullptr && digest->is_string()) {
        auto parsed = digest_from_hex(*digest->as_string());
        if (!parsed) {
            return contract_error(ContextDomainCode::InvalidItem,
                                  "prepared context digest is malformed");
        }
        prepared.selection_digest = *parsed;
    }
    return prepared;
}

} // namespace mira
