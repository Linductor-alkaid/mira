#include "subagent_support.hpp"

#include <mira/model_digest.hpp>

#include <algorithm>

namespace mira {
using subagent_support::error;

void SubagentCoordinator::State::settle(const std::shared_ptr<Child> &child, SubagentStatus status,
                                        std::string answer, std::string summary) {
    std::optional<ArtifactDescriptor> artifact;
    std::optional<JsonValue> structured;
    if (status != SubagentStatus::Answered)
        answer.clear();
    if (status == SubagentStatus::Answered && child->profile->output_schema) {
        auto parsed = parse_json(answer);
        if (!parsed ||
            !validate_instance_against_schema(parsed.value(), *child->profile->output_schema)
                 .empty()) {
            status = SubagentStatus::Failed;
            answer.clear();
            summary = "child structured result does not match its output schema";
        } else
            structured = std::move(parsed).value();
    }
    if (structured && to_json_string(*structured).size() > 32 * 1024) {
        status = SubagentStatus::Failed;
        answer.clear();
        structured.reset();
        summary = "child structured result encoding capacity exceeded";
    }
    if (status == SubagentStatus::Answered) {
        if (answer.size() > config.max_result_bytes ||
            to_json_string(JsonValue{answer}).size() > 32 * 1024) {
            status = SubagentStatus::Failed;
            answer.clear();
            summary = "child result capacity exceeded";
        } else {
            try {
                ArtifactWriteSpec spec;
                spec.media_type = "text/plain; charset=utf-8";
                spec.encoding = ArtifactEncoding::Utf8;
                spec.sensitivity = Sensitivity::Sensitive;
                spec.content_schema = {1, 0};
                spec.max_bytes = config.max_result_bytes;
                auto writer = artifacts->begin(spec);
                if (writer && writer.value().write(answer.data(), answer.size())) {
                    auto committed = artifacts->commit(writer.value());
                    if (committed)
                        artifact = committed.value();
                }
            } catch (...) { /* A failed artifact is explicit below; no raw error text. */
            }
            if (!artifact) {
                status = SubagentStatus::Failed;
                answer.clear();
                summary = "child result artifact could not be stored";
            }
        }
    }
    const std::lock_guard lock(mutex);
    // Late model success cannot override cancellation, deadline or takeover.
    if (cancelled.load() || child->cancelled.load()) {
        status = SubagentStatus::Cancelled;
        answer.clear();
        summary = "child execution was cancelled";
    } else if (child->context.expired(Timestamp::now())) {
        status = SubagentStatus::TimedOut;
        answer.clear();
        summary = "child deadline expired";
    }
    child->snapshot.status = status;
    child->snapshot.answer = std::move(answer);
    child->snapshot.safe_summary = std::move(summary);
    child->snapshot.result_artifact = std::move(artifact);
    if (status == SubagentStatus::Answered)
        child->snapshot.structured = std::move(structured);
    child->prompt.clear();
    child->spec.goal.clear();
    if (auto stored = emit("SubagentSettled", &child->snapshot); !stored) {
        child->snapshot.status = SubagentStatus::Failed;
        child->snapshot.answer.clear();
        child->snapshot.structured.reset();
        child->snapshot.safe_summary = stored.error().safe_message;
        audit_error = stored.error();
    }
    child->done = true;
    cv.notify_all();
}

void SubagentCoordinator::State::run_child(const std::shared_ptr<Child> &child,
                                           kairo::StopToken stop_token) {
    {
        const std::lock_guard lock(mutex);
        child->worker_id = std::this_thread::get_id();
        child->snapshot.status = SubagentStatus::Running;
    }
    auto context = child->context;
    const auto inherited = context.cancellation_requested;
    context.cancellation_requested = [this, child, inherited, stop_token] {
        return cancelled.load() || child->cancelled.load() || stop_token.stop_requested() ||
               (inherited && inherited());
    };
    SubagentStatus status = SubagentStatus::Failed;
    std::string answer;
    std::string summary = "child execution failed";
    try {
        // All setup and resource destruction happen before settlement becomes visible.
        if (context.cancelled_or_expired(Timestamp::now())) {
            status = context.cancelled() ? SubagentStatus::Cancelled : SubagentStatus::TimedOut;
        } else {
            auto made = child->profile->create_resources(child->spec, context);
            if (!made) {
                summary = "child resource factory failed";
            } else if (context.cancelled_or_expired(Timestamp::now())) {
                status = context.cancelled() ? SubagentStatus::Cancelled : SubagentStatus::TimedOut;
            } else {
                auto resources = std::move(made).value();
                const auto &profile = *child->profile;
                if (!resources.provider || resources.provider->profile().profile_digest() !=
                                               profile.model.profile_digest()) {
                    summary = "child provider does not match the registered profile";
                } else {
                    const auto catalog = resources.tools ? resources.tools->exposed_tools()
                                                         : std::vector<ExposedToolSpec>{};
                    bool allowed = true;
                    std::vector<ToolId> seen;
                    for (const auto &id : profile.allowed_tools) {
                        const auto found =
                            std::find_if(catalog.begin(), catalog.end(),
                                         [&](const auto &tool) { return tool.tool_id == id; });
                        if (found == catalog.end() || found->has_side_effects ||
                            found->wire_name == "spawn_agent" || found->wire_name == "get_agent" ||
                            found->wire_name == "cancel_agent" ||
                            std::find(seen.begin(), seen.end(), id) != seen.end()) {
                            allowed = false;
                            break;
                        }
                        seen.push_back(id);
                    }
                    if (!allowed) {
                        summary = "child tool authorization rejected the requested tool set";
                    } else {
                        ModelRouter router;
                        router.register_profile(
                            std::make_shared<const ModelProfile>(profile.model));
                        ModelGatewayConfig gateway_config;
                        gateway_config.retry_budget.max_attempts = 1;
                        ModelGateway gateway(executor, std::move(router), resources.artifacts,
                                             PriceTable{}, gateway_config);
                        gateway.register_provider(
                            std::shared_ptr<IModelProvider>(std::move(resources.provider)));
                        gateway.set_event_store(events, runtime, child->spec.session_id);
                        auto admission = std::make_shared<SimpleAdmissionGate>();
                        admission->activate(child->spec.task_id, child->spec.task_epoch);
                        gateway.set_admission_gate(admission);
                        ConversationLoop loop(gateway, profile.conversation);
                        loop.set_event_store(events, runtime, child->spec.session_id);
                        loop.set_tool_registry(resources.tools);
                        loop.set_model_input_supplier(std::move(resources.input_supplier));
                        loop.set_tool_selector(
                            [&profile](const AgentLoopSpec &,
                                       const ModelRequest &request) -> Result<std::vector<ToolId>> {
                                if (estimate_input_tokens(request) >
                                    profile.max_input_tokens_per_request) {
                                    return error(ErrorCode::ResourceExhausted,
                                                 "child input estimate exceeds its request quota");
                                }
                                for (const auto &id : profile.allowed_tools) {
                                    const auto found = std::find_if(
                                        request.tools.begin(), request.tools.end(),
                                        [&](const auto &tool) { return tool.tool_id == id; });
                                    if (found == request.tools.end() || found->has_side_effects) {
                                        return error(ErrorCode::PermissionDenied,
                                                     "child tool authorization changed");
                                    }
                                }
                                return profile.allowed_tools;
                            });
                        auto result = loop.run(child->spec, context);
                        if (result) {
                            switch (result.value().outcome) {
                            case ConversationOutcome::Answered:
                                status = SubagentStatus::Answered;
                                answer = std::move(result.value().answer);
                                summary = "child answered";
                                break;
                            case ConversationOutcome::Cancelled:
                                status = SubagentStatus::Cancelled;
                                summary = "child execution was cancelled";
                                break;
                            default:
                                summary = "child conversation did not complete";
                                break;
                            }
                        } else {
                            summary = "child conversation failed";
                        }
                    }
                }
            }
        }
    } catch (...) {
        summary = "child execution raised an exception";
    }
    // The provider/factory may finish late after an Executor stop request.
    if (context.cancelled())
        status = SubagentStatus::Cancelled;
    else if (context.expired(Timestamp::now()))
        status = SubagentStatus::TimedOut;
    settle(child, status, std::move(answer), std::move(summary));
}

Result<std::vector<std::string>>
SubagentCoordinator::State::collect(const AgentLoopSpec &spec, const OperationContext &context,
                                    bool wait) {
    if (spec.session_id != parent.session_id || spec.task_id != parent.task_id ||
        spec.task_epoch != parent.task_epoch) {
        return error(ErrorCode::PermissionDenied, "continuation belongs to another parent");
    }
    std::unique_lock lock(mutex);
    const auto ready = [&] {
        return std::any_of(children.begin(), children.end(),
                           [](const auto &c) { return c->done && !c->snapshot.claimed; });
    };
    const auto active = [&] {
        return std::any_of(children.begin(), children.end(),
                           [](const auto &c) { return !c->done; });
    };
    if (wait && !ready() && active()) {
        if (auto stored = emit("SubagentParentWaiting"); !stored) {
            audit_error = stored.error();
            return stored.error();
        }
    }
    while (wait && !ready() && active() && !context.cancelled_or_expired(Timestamp::now())) {
        // Bounded cancellation observation even if an external probe has no wake hook.
        cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    if (context.cancelled_or_expired(Timestamp::now())) {
        return error(context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded,
                     "parent continuation was interrupted");
    }
    if (audit_error)
        return *audit_error;
    std::vector<std::string> input;
    std::size_t bytes = 0;
    for (const auto &child : children) {
        if (!child->done || child->snapshot.claimed)
            continue;
        const auto &snapshot = child->snapshot;
        const auto text = to_json_string(JsonValue{
            JsonValue::Object{{"kind", "subagent-result"},
                              {"schema_version", 1},
                              {"child_session_id", snapshot.handle.session_id.to_string()},
                              {"child_task_id", snapshot.handle.task_id.to_string()},
                              {"execution_id", snapshot.handle.execution_id.to_string()},
                              {"status", subagent_status_name(snapshot.status)},
                              {"answer", snapshot.structured ? std::string{} : snapshot.answer},
                              {"safe_summary", snapshot.safe_summary},
                              {"structured", snapshot.structured.value_or(JsonValue{})}}});
        if (text.size() > 48 * 1024) {
            return error(ErrorCode::ResourceExhausted,
                         "child result encoding exceeds continuation capacity");
        }
        if (text.size() > 48 * 1024 - bytes)
            break;
        child->snapshot.claimed = true;
        if (auto stored = emit("SubagentResultClaimed", &child->snapshot); !stored) {
            child->snapshot.claimed = false;
            audit_error = stored.error();
            return stored.error();
        }
        input.push_back(text);
        bytes += text.size();
        if (input.size() == 16)
            break;
    }
    if (wait && input.empty() && !active())
        accepting = false;
    return input;
}
} // namespace mira
