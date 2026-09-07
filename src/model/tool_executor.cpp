#include <mira/tool_executor.hpp>

#include <mira/json.hpp>
#include <mira/model_digest.hpp>
#include <mira/model_schema.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error tool_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_executor";
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::string truncate(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
        text += "...";
    }
    return text;
}

[[nodiscard]] JsonValue spec_to_json(const BuiltinToolSpec &spec) {
    JsonValue::Object version;
    version.emplace_back("major", static_cast<std::int64_t>(spec.version.major));
    version.emplace_back("minor", static_cast<std::int64_t>(spec.version.minor));
    version.emplace_back("patch", static_cast<std::int64_t>(spec.version.patch));
    JsonValue::Object root;
    root.emplace_back("tool_id", spec.tool_id.to_string());
    root.emplace_back("version", JsonValue(std::move(version)));
    root.emplace_back("wire_name", spec.wire_name);
    root.emplace_back("description", spec.description);
    root.emplace_back("parameters_schema", spec.parameters_schema.root);
    root.emplace_back("has_side_effects", spec.has_side_effects);
    return JsonValue(std::move(root));
}

[[nodiscard]] ToolExecutionRecord failed_record(const ToolProposal &proposal,
                                                std::string summary) {
    ToolExecutionRecord record;
    record.provider_call_id = proposal.provider_call_id;
    record.tool_id = proposal.tool_id;
    record.failed = true;
    record.safe_error_summary = std::move(summary);
    return record;
}

} // namespace

Result<void> BuiltinToolRegistry::register_tool(BuiltinToolSpec spec, BuiltinToolHandler handler) {
    if (spec.wire_name.empty()) {
        return tool_error(ErrorCode::InvalidArgument, "tool wire_name must not be empty");
    }
    if (spec.description.empty()) {
        return tool_error(ErrorCode::InvalidArgument, "tool description must not be empty");
    }
    if (spec.tool_id.is_nil()) {
        return tool_error(ErrorCode::InvalidArgument, "tool id must not be nil");
    }
    if (is_known_hosted_tool_name(spec.wire_name)) {
        return tool_error(ErrorCode::InvalidArgument,
                          "wire_name collides with a hosted provider tool: " + spec.wire_name);
    }
    if (!handler) {
        return tool_error(ErrorCode::InvalidArgument, "tool handler must be set");
    }
    if (!spec.parameters_schema.valid()) {
        return tool_error(ErrorCode::InvalidArgument, "tool parameters schema must be an object");
    }
    if (const auto gate = gate_schema_subset(spec.parameters_schema); !gate) {
        return tool_error(ErrorCode::InvalidArgument,
                          "tool parameters schema uses an unsupported subset: " +
                              gate.error().safe_message);
    }

    const std::lock_guard lock(mutex_);
    for (const auto &entry : tools_) {
        if (entry.spec.wire_name == spec.wire_name) {
            return tool_error(ErrorCode::AlreadyExists,
                              "wire_name already registered: " + spec.wire_name);
        }
        if (entry.spec.tool_id == spec.tool_id) {
            return tool_error(ErrorCode::AlreadyExists, "tool id already registered");
        }
    }
    auto position = std::lower_bound(
        tools_.begin(), tools_.end(), spec.wire_name,
        [](const Entry &entry, const std::string &name) { return entry.spec.wire_name < name; });
    tools_.insert(position, Entry{std::move(spec), std::move(handler)});
    return Result<void>{};
}

std::vector<ExposedToolSpec> BuiltinToolRegistry::exposed_tools() const {
    const std::lock_guard lock(mutex_);
    std::vector<ExposedToolSpec> exposed;
    exposed.reserve(tools_.size());
    for (const auto &entry : tools_) {
        ExposedToolSpec tool;
        tool.tool_id = entry.spec.tool_id;
        tool.version = entry.spec.version;
        tool.wire_name = entry.spec.wire_name;
        tool.description = entry.spec.description;
        tool.parameters_schema = entry.spec.parameters_schema;
        tool.has_side_effects = entry.spec.has_side_effects;
        tool.spec_digest = canonical_json_digest(spec_to_json(entry.spec));
        exposed.push_back(std::move(tool));
    }
    return exposed;
}

std::size_t BuiltinToolRegistry::size() const {
    const std::lock_guard lock(mutex_);
    return tools_.size();
}

Result<ToolExecutionRecord> BuiltinToolRegistry::execute(const ToolProposal &proposal,
                                                         const OperationContext &context) {
    BuiltinToolSpec spec;
    BuiltinToolHandler handler;
    {
        const std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            tools_.begin(), tools_.end(),
            [&proposal](const Entry &entry) { return entry.spec.tool_id == proposal.tool_id; });
        if (found == tools_.end()) {
            return tool_error(ErrorCode::NotFound, "tool was not registered");
        }
        // Identity must still match the snapshot the producing request exposed;
        // a registry mutated mid-run fails closed instead of executing a tool
        // the model never saw in this shape.
        if (found->spec.wire_name != proposal.wire_name ||
            found->spec.version != proposal.tool_version ||
            found->spec.has_side_effects != proposal.has_side_effects) {
            return tool_error(ErrorCode::InvalidState,
                              "tool identity no longer matches the exposed snapshot");
        }
        if (!dispatched_.insert(proposal.operation_id).second) {
            return tool_error(ErrorCode::AlreadyExists,
                              "operation was already dispatched; at most once per operation");
        }
        spec = found->spec;
        handler = found->handler;
    }

    const auto violations = validate_instance_against_schema(proposal.arguments,
                                                             spec.parameters_schema);
    if (!violations.empty()) {
        std::ostringstream summary;
        summary << "arguments failed schema validation at " << violations.front().path << " ("
                << violations.front().keyword << "): " << violations.front().message;
        return failed_record(proposal, truncate(summary.str(), 512));
    }

    Result<JsonValue> outcome(Error{});
    try {
        outcome = handler(proposal.arguments, context);
    } catch (...) {
        return failed_record(proposal, "tool handler raised an exception");
    }
    if (!outcome) {
        if (outcome.error().code == ErrorCode::Cancelled) {
            return tool_error(ErrorCode::Cancelled, "tool execution was cancelled");
        }
        return failed_record(proposal, truncate(outcome.error().safe_message, 512));
    }

    ToolExecutionRecord record;
    record.provider_call_id = proposal.provider_call_id;
    record.tool_id = proposal.tool_id;
    const auto serialized = to_json_string(outcome.value());
    if (serialized.size() > kDefaultToolBridgeLimits.max_result_bytes) {
        return failed_record(proposal, "tool result exceeds the size limit");
    }
    record.result = std::move(outcome).value();
    return record;
}

BuiltinToolRegistration make_wait_tool() {
    BuiltinToolRegistration registration;
    registration.spec.wire_name = "wait";
    registration.spec.description =
        "Waits a bounded duration in milliseconds (0..10000) before the next step. Use this "
        "instead of repeated no-op actions when the interface needs time to settle.";
    const auto schema = parse_json(R"json({
        "type": "object",
        "properties": {
            "duration_ms": {"type": "integer", "minimum": 0, "maximum": 10000}
        },
        "required": ["duration_ms"],
        "additionalProperties": false
    })json");
    registration.spec.parameters_schema = JsonSchema{schema.value()};
    registration.spec.has_side_effects = false;
    registration.handler = [](const JsonValue &arguments,
                              const OperationContext &context) -> Result<JsonValue> {
        const auto *field = arguments.find("duration_ms");
        if (field == nullptr || !field->is_number()) {
            return tool_error(ErrorCode::InvalidArgument, "duration_ms must be a number");
        }
        const double requested = field->as_number().value_or(0.0);
        const auto wait_for = std::chrono::milliseconds(
            static_cast<long long>(requested < 0.0 ? 0.0 : requested));
        const auto started = std::chrono::steady_clock::now();
        while (true) {
            if (context.cancelled()) {
                return tool_error(ErrorCode::Cancelled, "wait was cancelled");
            }
            if (context.expired(Timestamp::now())) {
                return tool_error(ErrorCode::DeadlineExceeded, "wait exceeded its deadline");
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (elapsed >= wait_for) {
                JsonValue::Object result;
                result.emplace_back("waited_ms",
                                    static_cast<std::int64_t>(std::min<double>(
                                        requested, static_cast<double>(elapsed.count()))));
                return JsonValue(std::move(result));
            }
            const auto remaining = wait_for - elapsed;
            std::this_thread::sleep_for(
                std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(20)));
        }
    };
    return registration;
}

} // namespace mira
