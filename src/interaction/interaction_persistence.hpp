#pragma once

#include <mira/interaction_coordinator.hpp>
#include <mira/json.hpp>

namespace mira::interaction_support {

inline Error
recovery_error(ErrorCode code, std::string message,
               InteractionDomainCode domain = InteractionDomainCode::RecoveryUnavailable) {
    return interaction_error(code, domain, std::move(message));
}

inline std::optional<RouteIntent> parse_intent(std::string_view name) {
    for (const auto intent : {RouteIntent::ContinueCurrent, RouteIntent::SupplementTask,
                              RouteIntent::NewTask, RouteIntent::ResumeTask, RouteIntent::PauseTask,
                              RouteIntent::CancelTask, RouteIntent::Clarify, RouteIntent::Reject})
        if (route_intent_name(intent) == name)
            return intent;
    return std::nullopt;
}

inline JsonValue artifact_json(const ArtifactDescriptor &ref) {
    return JsonValue::Object{
        {"id", ref.id.to_string()}, {"digest", ref.digest.to_string()}, {"bytes", ref.byte_size}};
}

inline Result<ArtifactDescriptor> artifact_from_json(const JsonValue &json) {
    const auto *id = json.find("id");
    const auto *digest = json.find("digest");
    const auto *bytes = json.find("bytes");
    if (!id || !id->is_string() || !digest || !digest->is_string() || !bytes ||
        !bytes->is_integer() || bytes->as_integer().value() <= 0)
        return recovery_error(ErrorCode::DataLoss, "invalid interaction artifact reference");
    const auto parsed_id = ArtifactId::parse(*id->as_string());
    const auto parsed_digest = digest_from_hex(*digest->as_string());
    if (!parsed_id || parsed_id->is_nil() || !parsed_digest)
        return recovery_error(ErrorCode::DataLoss, "invalid interaction artifact identity");
    ArtifactDescriptor result;
    result.id = *parsed_id;
    result.digest = *parsed_digest;
    result.byte_size = static_cast<std::uint64_t>(bytes->as_integer().value());
    result.encoding = ArtifactEncoding::Utf8;
    result.media_type = "application/json";
    result.sensitivity = Sensitivity::Sensitive;
    result.content_schema = {1, 0};
    return result;
}

inline void recovery_fields(JsonValue::Object &json, const LedgerEntry &entry) {
    if (entry.input_ref)
        json.emplace_back("input_ref", artifact_json(*entry.input_ref));
    if (entry.context_ref)
        json.emplace_back("context_ref", artifact_json(*entry.context_ref));
    if (entry.clarification_for)
        json.emplace_back("clarification_for", *entry.clarification_for);
    if (entry.resolved_by)
        json.emplace_back("resolved_by", *entry.resolved_by);
    json.emplace_back("delivery", std::string(input_delivery_state_name(entry.delivery)));
}

inline Result<void> restore_recovery_fields(const JsonValue &json, LedgerEntry &entry) {
    for (const auto &field : {"input_ref", "context_ref"}) {
        if (const auto *value = json.find(field)) {
            const auto ref = artifact_from_json(*value);
            if (!ref)
                return ref.error();
            if (std::string_view(field) == "input_ref")
                entry.input_ref = ref.value();
            else
                entry.context_ref = ref.value();
        }
    }
    for (const auto &field : {"clarification_for", "resolved_by"}) {
        if (const auto *value = json.find(field)) {
            if (!value->is_string() || value->as_string()->empty())
                return recovery_error(ErrorCode::DataLoss, "invalid clarification reference");
            if (std::string_view(field) == "clarification_for")
                entry.clarification_for = *value->as_string();
            else {
                if (entry.resolved_by && entry.resolved_by != *value->as_string())
                    return recovery_error(ErrorCode::DataLoss,
                                          "clarification history changes its unique answer");
                entry.resolved_by = *value->as_string();
            }
        }
    }
    if (const auto *value = json.find("delivery")) {
        if (!value->is_string())
            return recovery_error(ErrorCode::DataLoss, "invalid delivery state");
        bool matched = false;
        for (const auto state : {InputDeliveryState::NotDispatched, InputDeliveryState::Dispatching,
                                 InputDeliveryState::Delivered, InputDeliveryState::Uncertain,
                                 InputDeliveryState::NotRequired}) {
            if (*value->as_string() == input_delivery_state_name(state)) {
                entry.delivery = state;
                matched = true;
                break;
            }
        }
        if (!matched)
            return recovery_error(ErrorCode::DataLoss, "unsupported delivery state");
    }
    if (const auto *value = json.find("intent");
        value && value->is_string() && !value->as_string()->empty()) {
        entry.intent = parse_intent(*value->as_string());
        if (!entry.intent)
            return recovery_error(ErrorCode::DataLoss, "unsupported route intent");
    }
    return {};
}
} // namespace mira::interaction_support
