#include "subagent_tool_session.hpp"
#include "subagent_support.hpp"

#include <mira/model_digest.hpp>

#include <algorithm>

namespace mira::subagent_support {
namespace {
bool reserved(const std::string &name) {
    return name == "spawn_agent" || name == "get_agent" || name == "cancel_agent" ||
           name == "discover_tools" || name == "load_tools";
}
Error interrupted(const OperationContext &context) {
    return error(context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded,
                 "child tool operation was interrupted");
}
} // namespace

Result<void> ToolSession::initialize(const SubagentProfile &profile, SubagentResources &resources,
                                     const AgentLoopSpec &child,
                                     std::shared_ptr<IEventStore> events, RuntimeId runtime) {
    if (profile.tool_access == SubagentToolAccess::StaticReadOnly) {
        if (!resources.tool_catalog.empty() || resources.authorize_tool || resources.search_tools)
            return error(ErrorCode::InvalidArgument, "discovery resources require Discover mode");
        registry = resources.tools;
        const auto tools = registry ? registry->exposed_tools() : std::vector<ExposedToolSpec>{};
        for (const auto &id : profile.allowed_tools) {
            const auto found = std::find_if(tools.begin(), tools.end(),
                                            [&](const auto &tool) { return tool.tool_id == id; });
            if (found == tools.end() || found->has_side_effects || reserved(found->wire_name) ||
                std::find(selected.begin(), selected.end(), id) != selected.end())
                return error(ErrorCode::PermissionDenied, "child read-only tool set rejected");
            selected.push_back(id);
        }
        return {};
    }
    if (resources.tools || !profile.allowed_tools.empty() || resources.tool_catalog.size() > 4096)
        return error(ErrorCode::InvalidArgument, "invalid child discovery catalog");
    catalog_ = std::move(resources.tool_catalog);
    authorize_ = std::move(resources.authorize_tool);
    search_ = std::move(resources.search_tools);
    child_ = child;
    events_ = std::move(events);
    runtime_ = runtime;
    max_active_ = std::min(profile.max_active_tools,
                           profile.model.capabilities.limits.max_tools_per_request - 2);
    // Validate the entire pinned catalog before exposing any entry. The
    // temporary registry checks IDs, aliases, handlers and supported schemas.
    BuiltinToolRegistry validation;
    std::size_t catalog_bytes = 0;
    for (const auto &entry : catalog_) {
        const auto &tool = entry.registration;
        catalog_bytes += to_json_string(tool.spec.parameters_schema.root).size() +
                         tool.spec.description.size() + tool.spec.wire_name.size();
        if (catalog_bytes > 8 * 1024 * 1024 || tool.spec.wire_name.size() > 128 ||
            reserved(tool.spec.wire_name) || tool.spec.description.size() > 2048 ||
            to_json_string(tool.spec.parameters_schema.root).size() > 32 * 1024 ||
            (tool.spec.has_side_effects && !entry.verify))
            return error(ErrorCode::PermissionDenied, "child catalog contract rejected");
        if (auto valid = validation.register_tool(tool.spec, tool.handler); !valid)
            return error(ErrorCode::InvalidArgument, "child catalog registration rejected");
    }
    registry = std::make_shared<BuiltinToolRegistry>();
    for (const bool load_tool : {false, true}) {
        BuiltinToolSpec spec;
        spec.wire_name = load_tool ? "load_tools" : "discover_tools";
        spec.description = load_tool
                               ? "Select the tools needed for your delegated goal. Replaces the "
                                 "active set; native schemas appear next turn. Only catalog names "
                                 "are accepted. Side effects require checking their results."
                               : "Discover tools for your delegated goal without asking the parent "
                                 "for tool names. Browse descriptions with offset (16 per page); "
                                 "query uses host semantic search when available, otherwise browse "
                                 "and select by meaning. Then call load_tools with chosen names.";
        spec.parameters_schema = JsonSchema{parse_json(load_tool ? R"({"type":"object",
            "properties":{"names":{"type":"array","items":{"type":"string"},"maxItems":64}},
            "required":["names"],"additionalProperties":false})"
                                                                 : R"({"type":"object",
            "properties":{"query":{"type":"string","maxLength":1024},
            "offset":{"type":"integer","minimum":0,"maximum":4096}},
            "additionalProperties":false})")
                                                .value()};
        controls_.push_back(spec.tool_id);
        auto added = registry->register_tool(
            std::move(spec), [this, load_tool](const JsonValue &args, const OperationContext &ctx) {
                if (ctx.cancelled_or_expired(Timestamp::now()))
                    return Result<JsonValue>{interrupted(ctx)};
                if (fatal)
                    return Result<JsonValue>{*fatal};
                return load_tool ? load(args, ctx) : discover(args, ctx);
            });
        if (!added)
            return added;
    }
    selected = controls_;
    return {};
}

Result<std::vector<ToolId>> ToolSession::select(const ModelRequest &request,
                                                std::uint64_t max_input) const {
    if (fatal)
        return *fatal;
    auto projected = request;
    std::erase_if(projected.tools, [&](const auto &tool) {
        return std::find(selected.begin(), selected.end(), tool.tool_id) == selected.end();
    });
    if (projected.tools.size() != selected.size())
        return error(ErrorCode::PermissionDenied, "child selected tool identity changed");
    if (estimate_input_tokens(projected) > max_input)
        return error(ErrorCode::ResourceExhausted,
                     "child input estimate exceeds its request quota");
    return selected;
}

Result<JsonValue> ToolSession::discover(const JsonValue &args, const OperationContext &context) {
    std::vector<std::size_t> candidates;
    const auto *query = args.find("query");
    const bool searched = search_ && query && !query->as_string()->empty();
    if (searched) {
        auto ids = search_(*query->as_string(), context);
        if (!ids)
            return error(ErrorCode::Unavailable, "child catalog search failed");
        if (ids.value().size() > 256)
            return error(ErrorCode::ResourceExhausted, "child search result capacity exceeded");
        for (const auto &id : ids.value()) {
            const auto found = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto &t) {
                return t.registration.spec.tool_id == id;
            });
            if (found == catalog_.end())
                return error(ErrorCode::PermissionDenied, "search returned an unknown tool");
            const auto index = static_cast<std::size_t>(found - catalog_.begin());
            if (std::find(candidates.begin(), candidates.end(), index) != candidates.end())
                return error(ErrorCode::InvalidArgument, "search returned duplicate tools");
            candidates.push_back(index);
        }
    } else {
        for (std::size_t i = 0; i < catalog_.size(); ++i)
            candidates.push_back(i);
    }
    const auto *offset_value = args.find("offset");
    const auto offset = offset_value ? static_cast<std::size_t>(*offset_value->as_number()) : 0;
    const auto end = std::min(candidates.size(), std::min(offset, candidates.size()) + 16);
    JsonValue::Array entries;
    for (auto i = offset; i < end; ++i) {
        const auto &spec = catalog_[candidates[i]].registration.spec;
        entries.emplace_back(JsonValue::Object{{"name", spec.wire_name},
                                               {"description", spec.description},
                                               {"has_side_effects", spec.has_side_effects}});
    }
    return JsonValue{JsonValue::Object{
        {"tools", std::move(entries)},
        {"search_applied", searched},
        {"total", static_cast<std::int64_t>(candidates.size())},
        {"next_offset",
         end < candidates.size() ? JsonValue{static_cast<std::int64_t>(end)} : JsonValue{}}}};
}

Result<JsonValue> ToolSession::load(const JsonValue &args, const OperationContext &) {
    std::vector<std::size_t> indices;
    for (const auto &name : *args.find("names")->as_array()) {
        const auto found = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto &tool) {
            return tool.registration.spec.wire_name == *name.as_string();
        });
        if (found == catalog_.end())
            return error(ErrorCode::NotFound, "selected tool is not in the child catalog");
        const auto index = static_cast<std::size_t>(found - catalog_.begin());
        if (std::find(indices.begin(), indices.end(), index) != indices.end())
            return error(ErrorCode::InvalidArgument, "duplicate tool selection");
        indices.push_back(index);
    }
    if (indices.size() > max_active_)
        return error(ErrorCode::ResourceExhausted, "child active tool capacity exceeded");
    auto next = controls_;
    for (const auto index : indices) {
        const auto &spec = catalog_[index].registration.spec;
        if (std::find(loaded_.begin(), loaded_.end(), spec.tool_id) == loaded_.end()) {
            auto added = registry->register_tool(
                spec, [this, index](const JsonValue &arguments, const OperationContext &context) {
                    return execute(index, arguments, context);
                });
            if (!added) {
                fatal = error(ErrorCode::Internal, "child tool activation failed");
                return *fatal;
            }
            loaded_.push_back(spec.tool_id);
        }
        next.push_back(spec.tool_id);
    }
    selected = std::move(next);
    return JsonValue{
        JsonValue::Object{{"active_tools", static_cast<std::int64_t>(indices.size())}}};
}

Result<void> ToolSession::emit(const char *type, const BuiltinToolSpec &tool, const JsonValue &args,
                               const OperationContext &context) {
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = runtime_;
    request.session_id = child_.session_id;
    request.task_id = child_.task_id;
    request.payload = {
        type,
        to_json_string(JsonValue{JsonValue::Object{
            {"schema_version", 1},
            {"operation_id", context.operation.to_string()},
            {"tool_id", tool.tool_id.to_string()},
            {"tool_version", std::to_string(tool.version.major) + "." +
                                 std::to_string(tool.version.minor) + "." +
                                 std::to_string(tool.version.patch)},
            {"arguments_digest", canonical_json_digest(args).to_string()},
            {"spec_digest", canonical_json_digest(JsonValue{JsonValue::Object{
                                                      {"name", tool.wire_name},
                                                      {"schema", tool.parameters_schema.root},
                                                      {"description", tool.description},
                                                      {"has_side_effects", tool.has_side_effects}}})
                                .to_string()}}}),
        EventClass::Critical};
    try {
        auto stored = events_->append(request);
        if (stored && (stored.value().achieved == Durability::ProcessCrash ||
                       stored.value().achieved == Durability::PowerLoss))
            return {};
    } catch (...) {
    }
    fatal = error(ErrorCode::DataLoss, "child tool audit could not be durably stored");
    return *fatal;
}

Result<JsonValue> ToolSession::execute(std::size_t index, const JsonValue &args,
                                       const OperationContext &context) {
    if (fatal)
        return *fatal;
    if (context.cancelled_or_expired(Timestamp::now()))
        return interrupted(context);
    const auto &entry = catalog_[index];
    const auto &spec = entry.registration.spec;
    if (std::find(selected.begin(), selected.end(), spec.tool_id) == selected.end())
        return error(ErrorCode::PermissionDenied, "tool was unloaded before dispatch");
    // One exact identity for authorization, dispatch, Verify and audit. The
    // enclosing ConversationLoop also deduplicates provider proposals.
    auto operation = context;
    operation.operation = OperationId::generate();
    if (authorize_) {
        Result<void> allowed;
        try {
            allowed = authorize_(child_, spec, args, operation);
        } catch (...) {
            allowed = error(ErrorCode::PermissionDenied, "child tool policy raised an exception");
        }
        if (!allowed) {
            if (auto stored = emit("SubagentToolDenied", spec, args, operation); !stored)
                return stored.error();
            return error(ErrorCode::PermissionDenied, "child tool policy denied the invocation");
        }
    }
    if (operation.cancelled_or_expired(Timestamp::now()))
        return interrupted(operation);
    if (spec.has_side_effects) {
        if (auto stored = emit("SubagentEffectDispatchStarted", spec, args, operation); !stored)
            return stored.error();
        if (operation.cancelled_or_expired(Timestamp::now())) {
            // No dispatch occurred, even though the durable intent exists.
            if (auto stored = emit("SubagentEffectNotApplied", spec, args, operation); !stored)
                return stored.error();
            return interrupted(operation);
        }
    }
    Result<JsonValue> receipt =
        error(ErrorCode::Internal, "child tool handler raised an exception");
    try {
        receipt = entry.registration.handler(args, operation);
    } catch (...) {
    }
    if (!spec.has_side_effects)
        return receipt;
    Result<SubagentEffectStatus> verified =
        error(ErrorCode::Internal, "child effect verification failed");
    try {
        verified = entry.verify(args, receipt, operation);
    } catch (...) {
    }
    const auto status = verified ? verified.value() : SubagentEffectStatus::Uncertain;
    const char *event = "SubagentEffectUncertain";
    if (status == SubagentEffectStatus::Applied) {
        ++verified_effects;
        event = "SubagentEffectVerified";
    } else if (status == SubagentEffectStatus::NotApplied)
        event = "SubagentEffectNotApplied";
    if (status != SubagentEffectStatus::Applied && status != SubagentEffectStatus::NotApplied)
        effects_uncertain = true;
    if (auto stored = emit(event, spec, args, operation); !stored) {
        effects_uncertain = true;
        return stored.error();
    }
    if (effects_uncertain || (status == SubagentEffectStatus::Applied && !receipt)) {
        fatal = error(ErrorCode::ExecutionUncertain,
                      "child effect requires reconciliation; do not repeat dispatch");
        return *fatal;
    }
    if (status == SubagentEffectStatus::NotApplied)
        return error(ErrorCode::InvalidState, "child effect was verified not applied");
    return receipt;
}
} // namespace mira::subagent_support
