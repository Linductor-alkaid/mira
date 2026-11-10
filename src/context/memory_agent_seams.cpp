#include "memory_agent_support.hpp"
#include <mira/interaction_coordinator.hpp>

#include <algorithm>

namespace mira {
using memory_agent_support::error;
namespace {
JsonValue report_json(const MemoryAgentResult &result) {
    JsonValue::Array entries;
    for (const auto &entry : result.consolidation.entries)
        entries.emplace_back(
            JsonValue::Object{{"disposition", candidate_disposition_name(entry.disposition)},
                              {"reason", entry.reason_code}});
    return JsonValue::Object{
        {"operation", result.operation.to_string()},
        {"state", "completed"},
        {"candidate_count", static_cast<std::int64_t>(result.candidates.size())},
        {"degraded", result.degraded},
        {"more_pending", result.more_pending},
        {"pending_approval",
         static_cast<std::int64_t>(result.consolidation.pending_approval.size())},
        {"writes", std::move(entries)},
        {"compressed_items", static_cast<std::int64_t>(result.compressed_items)}};
}
} // namespace

ModelContextSupplier MemoryAgentService::context_supplier() const {
    const std::weak_ptr<State> weak = state_;
    return [weak](const ModelRequest &request,
                  const OperationContext &context) -> Result<ModelContextSnapshot> {
        const auto s = weak.lock();
        if (!s)
            return error(ErrorCode::InvalidState, "memory service owner no longer exists");
        ModelContextSnapshot snapshot;
        std::uint64_t revision = 0;
        std::chrono::system_clock::time_point expires{};
        {
            const std::lock_guard lock(s->mutex);
            const auto found = s->works.find(request.task_id);
            if (s->closing || found == s->works.end() || !found->second.active ||
                found->second.revoked || found->second.input_pending ||
                !memory_agent_support::request_current(found->second.binding) ||
                !memory_agent_support::same_owner(context, found->second.binding.work) ||
                request.task_epoch != found->second.binding.work.task_epoch)
                return error(ErrorCode::InvalidState, "memory request identity is not current");
            if (std::chrono::system_clock::now() < found->second.cache_until)
                snapshot.candidates = found->second.candidates;
            if (!snapshot.candidates.empty())
                expires = found->second.cache_until;
            snapshot.environment_epoch = found->second.binding.environment_epoch;
            revision = found->second.revision;
        }
        snapshot.current = [weak, task = request.task_id, revision, expires] {
            const auto state = weak.lock();
            if (!state)
                return false;
            const std::lock_guard lock(state->mutex);
            const auto found = state->works.find(task);
            return !state->closing && found != state->works.end() && found->second.active &&
                   !found->second.revoked && !found->second.input_pending &&
                   found->second.revision == revision &&
                   memory_agent_support::request_current(found->second.binding) &&
                   (expires == std::chrono::system_clock::time_point{} ||
                    std::chrono::system_clock::now() < expires);
        };
        snapshot.refreshable = [weak, task = request.task_id, epoch = request.task_epoch] {
            const auto state = weak.lock();
            if (!state)
                return false;
            const std::lock_guard lock(state->mutex);
            const auto found = state->works.find(task);
            return !state->closing && found != state->works.end() && found->second.active &&
                   !found->second.revoked && !found->second.input_pending &&
                   found->second.binding.work.task_epoch == epoch &&
                   memory_agent_support::request_current(found->second.binding);
        };
        return snapshot;
    };
}

std::shared_ptr<IUserInputSink>
MemoryAgentService::input_sink(TaskId task, std::shared_ptr<IUserInputSink> downstream) const {
    class Sink final : public IUserInputSink {
      public:
        explicit Sink(std::function<Result<void>(const InputEnvelope &, TaskId)> deliver)
            : deliver_(std::move(deliver)) {}
        Result<void> deliver(const InputEnvelope &input, TaskId target) override {
            const std::lock_guard serialize(mutex_);
            return deliver_(input, target);
        }

      private:
        std::function<Result<void>(const InputEnvelope &, TaskId)> deliver_;
        std::mutex mutex_;
    };
    const std::weak_ptr<State> weak = state_;
    return std::make_shared<Sink>([weak, task, downstream = std::move(downstream)](
                                      const InputEnvelope &input, TaskId target) -> Result<void> {
        const auto s = weak.lock();
        if (!s || !downstream || target != task)
            return error(ErrorCode::PermissionDenied, "memory input sink has a foreign target");
        std::uint64_t revision = 0;
        {
            const std::lock_guard serialize(s->commit_mutex);
            const std::lock_guard lock(s->mutex);
            const auto found = s->works.find(task);
            if (s->closing || found == s->works.end() || !found->second.active ||
                found->second.revoked)
                return error(ErrorCode::InvalidState, "memory input work is not active");
            const auto &principal = found->second.binding.principal;
            if (input.principal.tenant_id != principal.tenant_id ||
                input.principal.user_id != principal.user_id ||
                input.principal.identity_version != principal.identity_version)
                return error(ErrorCode::PermissionDenied,
                             "memory input principal does not own this work");
            s->invalidate(found->second);
            found->second.input_pending = true;
            revision = found->second.revision;
        }
        Result<void> delivered;
        try {
            delivered = downstream->deliver(input, target);
        } catch (...) {
            return error(ErrorCode::Unavailable, "memory input delivery raised an exception");
        }
        if (delivered) {
            const std::lock_guard lock(s->mutex);
            const auto found = s->works.find(task);
            if (found != s->works.end() && found->second.revision == revision)
                found->second.input_pending = false;
        }
        return delivered;
    });
}

HistoryTransformer MemoryAgentService::history_transformer(TaskId task) const {
    const std::weak_ptr<State> weak = state_;
    return [weak, task](
               const std::vector<ModelInputItem> &history) -> Result<std::vector<ModelInputItem>> {
        const auto s = weak.lock();
        if (!s)
            return error(ErrorCode::InvalidState, "memory service owner no longer exists");
        if (history.size() > s->config.max_history_items ||
            to_json_string(memory_agent_support::history_json(history)).size() >
                s->config.max_source_bytes)
            return error(ErrorCode::ResourceExhausted, "memory history capture exceeds capacity");
        const std::lock_guard lock(s->mutex);
        const auto found = s->works.find(task);
        if (s->closing || found == s->works.end() || !found->second.active || found->second.revoked)
            return error(ErrorCode::InvalidState, "memory history work is not active");
        auto &work = found->second;
        work.captured = history;
        if (work.prefix.empty() || history.size() < work.prefix.size())
            return history;
        const std::vector<ModelInputItem> prefix(
            history.begin(), history.begin() + static_cast<std::ptrdiff_t>(work.prefix.size()));
        if (memory_agent_support::history_json(prefix) !=
            memory_agent_support::history_json(work.prefix))
            return history;
        auto transformed = work.compacted;
        transformed.insert(transformed.end(),
                           history.begin() + static_cast<std::ptrdiff_t>(work.prefix.size()),
                           history.end());
        if (!memory_agent_support::complete_history(transformed))
            return error(ErrorCode::InvalidArgument,
                         "memory history transform broke native protocol");
        // The Loop independently checks its actual protected tail. A mismatch
        // degrades to raw replay; this seam never authorizes dropping it.
        return transformed;
    };
}

ConversationContinuationSource MemoryAgentService::continuation_source(TaskId task) const {
    const std::weak_ptr<State> weak = state_;
    return [weak, task](const AgentLoopSpec &spec, const OperationContext &context,
                        bool wait) -> Result<std::vector<std::string>> {
        const auto s = weak.lock();
        if (!s)
            return error(ErrorCode::InvalidState, "memory service owner no longer exists");
        if (spec.task_id != task || !memory_agent_support::same_owner(context, spec))
            return error(ErrorCode::PermissionDenied, "memory continuation has a foreign owner");
        std::unique_lock lock(s->mutex);
        const auto pending = [&] {
            return std::any_of(s->jobs.begin(), s->jobs.end(), [&](const auto &entry) {
                const auto &job = *entry.second;
                return job.request.task == task && job.request.foreground && !job.claimed &&
                       !job.done && s->current(job);
            });
        };
        while (wait && pending()) {
            if (context.cancelled_or_expired(Timestamp::now()))
                return error(context.cancelled() ? ErrorCode::Cancelled
                                                 : ErrorCode::DeadlineExceeded,
                             "memory continuation was interrupted");
            s->cv.wait_for(lock, std::chrono::milliseconds{5});
        }
        std::vector<std::string> messages;
        for (const auto &[id, job] : s->jobs) {
            (void)id;
            if (messages.size() == 16)
                break;
            if (job->request.task != task || !job->request.foreground || job->claimed ||
                !job->done || !s->current(*job))
                continue;
            const auto &result = job->future.get();
            messages.push_back(to_json_string(
                result ? report_json(result.value())
                       : JsonValue{JsonValue::Object{
                             {"operation", job->id.to_string()},
                             {"state", "failed"},
                             {"code", static_cast<std::int64_t>(result.error().code)}}}));
            job->claimed = true;
        }
        return messages;
    };
}

std::vector<BuiltinToolRegistration> MemoryAgentService::tools() const {
    const std::weak_ptr<State> weak = state_;
    std::vector<BuiltinToolRegistration> registrations;
    BuiltinToolSpec start;
    start.wire_name = "memory_start";
    start.description = "Delegate scoped recall, event recording, or captured-history compression. "
                        "Returns a job id, not completion. Record can require human approval.";
    start.has_side_effects = true;
    start.parameters_schema = JsonSchema{
        parse_json(
            R"({"type":"object","properties":{"mode":{"type":"string","enum":["recall","record","compress"]},"query":{"type":"string","maxLength":4096},"scope_index":{"type":"integer","minimum":0,"maximum":15}},"required":["mode"],"additionalProperties":false})")
            .value()};
    registrations.push_back(
        {start,
         [weak](const JsonValue &args, const OperationContext &context) -> Result<JsonValue> {
             const auto s = weak.lock();
             if (!s)
                 return error(ErrorCode::InvalidState, "memory service owner no longer exists");
             const auto *mode = args.find("mode");
             if (!mode || !mode->is_string())
                 return error(ErrorCode::InvalidArgument, "memory mode is missing");
             MemoryAgentRequest request;
             request.task = context.task;
             request.foreground = true;
             if (*mode->as_string() == "record")
                 request.mode = MemoryAgentMode::Record;
             else if (*mode->as_string() == "compress")
                 request.mode = MemoryAgentMode::Compress;
             else if (*mode->as_string() != "recall")
                 return error(ErrorCode::InvalidArgument, "memory mode is unknown");
             if (const auto *query = args.find("query"); query && query->is_string())
                 request.query = *query->as_string();
             {
                 const std::lock_guard lock(s->mutex);
                 const auto found = s->works.find(context.task);
                 if (found == s->works.end() ||
                     !memory_agent_support::same_owner(context, found->second.binding.work))
                     return error(ErrorCode::PermissionDenied, "memory tool caller is not bound");
                 request.expected_revision = found->second.revision;
                 if (request.mode == MemoryAgentMode::Record) {
                     std::size_t index = 0;
                     if (const auto *value = args.find("scope_index"); value) {
                         if (!value->is_integer() || *value->as_integer() < 0)
                             return error(ErrorCode::InvalidArgument, "invalid memory scope index");
                         index = static_cast<std::size_t>(*value->as_integer());
                     }
                     if (index >= found->second.binding.scopes.size())
                         return error(ErrorCode::PermissionDenied,
                                      "memory scope index is not authorized");
                     request.write_scope = found->second.binding.scopes[index];
                 } else if (request.mode == MemoryAgentMode::Compress) {
                     request.history = found->second.captured;
                     if (request.history.size() < 3)
                         return error(ErrorCode::InvalidState,
                                      "no compressible history was captured");
                     request.protected_items =
                         std::min<std::size_t>(16, request.history.size() - 1);
                     while (request.protected_items < request.history.size()) {
                         const std::vector<ModelInputItem> prefix(
                             request.history.begin(),
                             request.history.end() -
                                 static_cast<std::ptrdiff_t>(request.protected_items));
                         if (memory_agent_support::complete_history(prefix))
                             break;
                         ++request.protected_items;
                     }
                 }
             }
             auto submitted = s->submit(std::move(request), context);
             if (!submitted)
                 return submitted.error();
             return JsonValue{JsonValue::Object{
                 {"operation", submitted.value().operation.to_string()}, {"state", "accepted"}}};
         }});
    for (const bool cancel : {false, true}) {
        BuiltinToolSpec spec;
        spec.wire_name = cancel ? "memory_cancel" : "memory_result";
        spec.description = cancel ? "Request cooperative cancellation of an owned memory job."
                                  : "Inspect an owned memory job; pending is not completion.";
        spec.has_side_effects = cancel;
        spec.parameters_schema = JsonSchema{
            parse_json(
                R"({"type":"object","properties":{"operation":{"type":"string","maxLength":32}},"required":["operation"],"additionalProperties":false})")
                .value()};
        registrations.push_back(
            {spec,
             [weak, cancel](const JsonValue &args,
                            const OperationContext &context) -> Result<JsonValue> {
                 const auto s = weak.lock();
                 const auto *value = args.find("operation");
                 const auto id = value && value->is_string()
                                     ? OperationId::parse(*value->as_string())
                                     : std::nullopt;
                 if (!s || !id)
                     return error(ErrorCode::InvalidArgument, "invalid memory job reference");
                 {
                     const std::lock_guard lock(s->mutex);
                     const auto found = s->jobs.find(*id);
                     if (found == s->jobs.end() ||
                         !memory_agent_support::same_owner(context, found->second->binding.work))
                         return error(ErrorCode::PermissionDenied,
                                      "memory job is not owned by this caller");
                     if (cancel) {
                         found->second->cancelled.store(true);
                         return JsonValue{JsonValue::Object{{"state", "cancellation-requested"}}};
                     }
                 }
                 auto result = s->result(*id);
                 if (!result)
                     return result.error();
                 return result.value() ? report_json(*result.value())
                                       : JsonValue{JsonValue::Object{{"state", "pending"}}};
             }});
    }
    return registrations;
}
} // namespace mira
