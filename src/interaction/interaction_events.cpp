#include <mira/interaction_coordinator.hpp>

#include <array>
#include <string_view>

namespace mira {
namespace {

// The interaction event family (mira.interaction.*.v1). Closed set: the type
// name and the wire schema name are listed once, in table order, and both
// views are derived from this table (DEC-068 §7).
struct InteractionEventEntry final {
    std::string_view type;
    std::string_view schema;
};

constexpr InteractionEventEntry kInteractionEvents[] = {
    {"InputAccepted", "mira.interaction.input-accepted.v1"},
    {"InputRouted", "mira.interaction.input-routed.v1"},
    {"InputClarified", "mira.interaction.input-clarified.v1"},
    {"InputRejected", "mira.interaction.input-rejected.v1"},
    {"InputAnswered", "mira.interaction.input-answered.v1"},
    {"InputTaskSettled", "mira.interaction.input-task-settled.v1"},
};

constexpr std::size_t kInteractionEventCount =
    sizeof(kInteractionEvents) / sizeof(kInteractionEvents[0]);

} // namespace

std::string_view input_stage_name(InputStage stage) {
    switch (stage) {
    case InputStage::Accepted:
        return "Accepted";
    case InputStage::Routed:
        return "Routed";
    case InputStage::Answered:
        return "Answered";
    case InputStage::TaskSettled:
        return "TaskSettled";
    case InputStage::Rejected:
        return "Rejected";
    }
    return "Accepted";
}

std::string_view route_intent_name(RouteIntent intent) {
    switch (intent) {
    case RouteIntent::ContinueCurrent:
        return "ContinueCurrent";
    case RouteIntent::SupplementTask:
        return "SupplementTask";
    case RouteIntent::NewTask:
        return "NewTask";
    case RouteIntent::ResumeTask:
        return "ResumeTask";
    case RouteIntent::PauseTask:
        return "PauseTask";
    case RouteIntent::CancelTask:
        return "CancelTask";
    case RouteIntent::Clarify:
        return "Clarify";
    case RouteIntent::Reject:
        return "Reject";
    }
    return "Reject";
}

std::string_view interaction_event_schema(InteractionEventType type) {
    const auto index = static_cast<std::size_t>(type);
    if (index >= kInteractionEventCount) {
        return kInteractionEvents[0].schema;
    }
    return kInteractionEvents[index].schema;
}

std::optional<InteractionEventType> parse_interaction_event_schema(std::string_view schema) {
    for (std::size_t index = 0; index < kInteractionEventCount; ++index) {
        if (kInteractionEvents[index].schema == schema) {
            return static_cast<InteractionEventType>(index);
        }
    }
    return std::nullopt;
}

bool is_interaction_event_type(std::string_view type) {
    for (const auto &entry : kInteractionEvents) {
        if (entry.type == type) {
            return true;
        }
    }
    return false;
}

Error interaction_error(ErrorCode code, InteractionDomainCode domain_code, std::string message,
                        bool retryable) {
    Error error;
    error.code = code;
    error.domain = std::string(kInteractionErrorDomain);
    error.domain_code = static_cast<std::int32_t>(domain_code);
    error.safe_message = std::move(message);
    error.retryable = retryable;
    return error;
}

} // namespace mira
