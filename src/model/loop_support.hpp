#pragma once

// Internal to the model layer: small helpers shared by the device agent loop
// and the conversational loop.

#include <mira/agent_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <memory>
#include <string>

namespace mira::loop_support {

// Rate limits, overload, transport failures and network-stage deadline hints
// are recoverable within a bounded recovery budget; permission, policy and
// request-shape failures are not. The deadline code only recovers when the
// transport marked the stall retryable — whole-operation and total-budget
// deadlines stay non-retryable. Kept in one place so both loops classify
// identically.
[[nodiscard]] inline bool recoverable_model_failure(const Error &error) {
    if (!error.retryable) {
        return false;
    }
    if (error.domain != "mira.model") {
        return true;
    }
    return error.domain_code == static_cast<std::int32_t>(ModelDomainCode::RateLimited) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::ProviderOverloaded) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::TransportFailed) ||
           error.domain_code == static_cast<std::int32_t>(ModelDomainCode::ModelDeadlineExceeded);
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

// Resolves IDs against one immutable registry snapshot. Never degrade to full
// exposure on failure: that would silently broaden the model's authority.
[[nodiscard]] inline Result<void> select_tools(const AgentLoopSpec &spec, ModelRequest &request,
                                               const ToolSelector &selector) {
    if (!selector) {
        return {};
    }
    const auto fail = [](std::string message) -> Result<void> {
        Error error;
        error.code = ErrorCode::InvalidArgument;
        error.domain = "mira.tool_selection";
        error.safe_message = std::move(message);
        return error;
    };
    try {
        auto selected = selector(spec, request);
        if (!selected) {
            return fail("tool selector returned an error");
        }
        if (selected.value().size() > kModelMaxToolsPerRequest) {
            return fail("tool selection count=" + std::to_string(selected.value().size()) +
                        " exceeds limit=" + std::to_string(kModelMaxToolsPerRequest));
        }
        std::vector<ExposedToolSpec> exposed;
        exposed.reserve(selected.value().size());
        for (const auto &id : selected.value()) {
            const auto found = std::find_if(request.tools.begin(), request.tools.end(),
                                            [&](const auto &tool) { return tool.tool_id == id; });
            if (found == request.tools.end()) {
                return fail("tool selector returned an unregistered id");
            }
            if (std::any_of(exposed.begin(), exposed.end(),
                            [&](const auto &tool) { return tool.tool_id == id; })) {
                return fail("tool selector returned a duplicate id");
            }
            exposed.push_back(*found);
        }
        request.tools = std::move(exposed);
        return {};
    } catch (...) {
        return fail("tool selector threw an exception");
    }
}

// IDs and a digest permit offline attribution without copying schemas or input.
[[nodiscard]] inline JsonValue selection_detail(const ModelRequest &request, std::size_t available,
                                                const Result<void> &selected) {
    JsonValue::Object detail{
        {"request_id", request.request_id.to_string()},
        {"available_count", static_cast<std::int64_t>(available)},
        {"selected_count", selected ? static_cast<std::int64_t>(request.tools.size()) : 0}};
    if (selected) {
        JsonValue::Array ids;
        for (const auto &tool : request.tools) {
            ids.emplace_back(tool.tool_id.to_string());
        }
        detail.emplace_back("selected_tool_ids", std::move(ids));
        detail.emplace_back("tool_snapshot_digest",
                            tool_snapshot_digest(request.tools).to_string());
    } else {
        detail.emplace_back("reason", selected.error().safe_message);
    }
    return JsonValue{std::move(detail)};
}

} // namespace mira::loop_support
