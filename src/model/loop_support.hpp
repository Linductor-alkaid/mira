#pragma once

// Internal to the model layer: small helpers shared by the device agent loop
// and the conversational loop.

#include <mira/agent_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>

#include <memory>
#include <string>

namespace mira::loop_support {

// Rate limits, overload and transport failures are recoverable within a
// bounded recovery budget; permission, policy and request-shape failures are
// not. Kept in one place so both loops classify identically.
[[nodiscard]] inline bool recoverable_model_failure(const Error &error) {
    if (!error.retryable) {
        return false;
    }
    if (error.domain != "mira.model") {
        return true;
    }
    return error.domain_code == static_cast<std::int32_t>(ModelDomainCode::RateLimited) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::TransportFailed);
}

// Appends one loop event envelope shared by both loop implementations:
// {task_id, task_epoch, detail} bound to the loop's runtime/session identity.
// Best-effort by contract: a missing or failing event store never disturbs
// the decision path.
inline void emit_loop_event(const std::shared_ptr<IEventStore> &events, const RuntimeId &runtime,
                            const SessionId &session, const AgentLoopSpec &spec, std::string type,
                            JsonValue summary, EventClass classification) {
    if (events == nullptr) {
        return;
    }
    JsonValue::Object envelope;
    envelope.emplace_back("task_id", spec.task_id.to_string());
    envelope.emplace_back("task_epoch", static_cast<std::int64_t>(spec.task_epoch));
    envelope.emplace_back("detail", std::move(summary));
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime;
    append.session_id = session;
    append.task_id = spec.task_id;
    append.payload = EventPayload{std::move(type), to_json_string(JsonValue(std::move(envelope))),
                                  classification};
    (void)events->append(append);
}

// Base ModelRequest shared by both loops: contract identity, task binding
// and the tool registry snapshot the model is allowed to see (DEC-015).
template <typename ToolRegistry>
[[nodiscard]] ModelRequest begin_model_request(const AgentLoopSpec &spec, ToolRegistry *tools) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = spec.task_id;
    request.task_epoch = spec.task_epoch;
    request.profile_id = spec.profile_id;
    if (tools != nullptr) {
        request.tools = tools->exposed_tools();
    }
    return request;
}

} // namespace mira::loop_support
