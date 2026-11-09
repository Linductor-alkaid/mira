#include "subagent_support.hpp"

#include <algorithm>
#include <limits>

namespace mira {
using subagent_support::error;

std::string subagent_status_name(SubagentStatus status) {
    switch (status) {
    case SubagentStatus::Starting:
        return "Starting";
    case SubagentStatus::Running:
        return "Running";
    case SubagentStatus::Answered:
        return "Answered";
    case SubagentStatus::Failed:
        return "Failed";
    case SubagentStatus::Cancelled:
        return "Cancelled";
    case SubagentStatus::TimedOut:
        return "TimedOut";
    }
    return "Unknown";
}

Result<void> SubagentCoordinator::State::validate() const {
    if (parent.session_id.is_nil() || parent.task_id.is_nil() || parent.task_epoch == 0 ||
        parent.goal.empty() || runtime.is_nil() || !events || !artifacts ||
        config.max_active == 0 || config.max_active > 16 || config.max_children == 0 ||
        config.max_children > 64 || config.max_active > config.max_children ||
        config.max_pending_results == 0 || config.max_pending_results > 64 ||
        config.max_prompt_bytes == 0 || config.max_prompt_bytes > 64 * 1024 ||
        config.max_result_bytes == 0 || config.max_result_bytes > 16 * 1024 ||
        config.max_model_requests == 0 || config.max_input_tokens == 0 ||
        config.max_output_tokens == 0) {
        return error(ErrorCode::InvalidArgument, "invalid subagent coordinator configuration");
    }
    return {};
}

Result<void> SubagentCoordinator::State::emit(const char *type, const SubagentSnapshot *child,
                                              const char *reason) {
    JsonValue::Object detail{{"schema_version", 1},
                             {"parent_session_id", parent.session_id.to_string()},
                             {"parent_task_id", parent.task_id.to_string()},
                             {"parent_task_epoch", std::to_string(parent.task_epoch)},
                             {"parent_execution_id", parent_execution.to_string()}};
    if (child) {
        detail.emplace_back("child_session_id", child->handle.session_id.to_string());
        detail.emplace_back("child_task_id", child->handle.task_id.to_string());
        detail.emplace_back("execution_id", child->handle.execution_id.to_string());
        detail.emplace_back("status", subagent_status_name(child->status));
        detail.emplace_back("claimed", child->claimed);
        detail.emplace_back("answer_bytes", static_cast<std::int64_t>(child->answer.size()));
        if (child->result_artifact) {
            detail.emplace_back("artifact_id", child->result_artifact->id.to_string());
            detail.emplace_back("artifact_digest", child->result_artifact->digest.to_string());
            detail.emplace_back("artifact_bytes",
                                static_cast<std::int64_t>(child->result_artifact->byte_size));
        }
    }
    if (reason)
        detail.emplace_back("reason", reason);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime;
    append.session_id = parent.session_id;
    append.task_id = parent.task_id;
    append.payload =
        EventPayload{type, to_json_string(JsonValue{std::move(detail)}), EventClass::State};
    try {
        if (!events || !events->append(append)) {
            return error(ErrorCode::DataLoss, "subagent event could not be stored");
        }
    } catch (...) {
        return error(ErrorCode::DataLoss, "subagent event store raised an exception");
    }
    return {};
}

SubagentCoordinator::SubagentCoordinator(kairo::Executor &executor, AgentLoopSpec parent,
                                         std::shared_ptr<IEventStore> events,
                                         std::shared_ptr<IArtifactStore> artifacts,
                                         RuntimeId runtime, SubagentConfig config)
    : state_(std::make_shared<State>(executor, std::move(parent), std::move(events),
                                     std::move(artifacts), runtime, config)) {}

SubagentCoordinator::~SubagentCoordinator() { (void)close(); }

Result<void> SubagentCoordinator::register_profile(SubagentProfile profile) {
    auto &s = *state_;
    if (auto valid = s.validate(); !valid)
        return valid;
    const auto &c = profile.conversation;
    if (profile.name.empty() || profile.name.size() > 64 || profile.description.size() > 1024 ||
        !profile.create_resources || !profile.model.validate() ||
        c.limit_mode != ConversationLimitMode::Finite || c.max_turns == 0 || c.max_turns > 64 ||
        c.max_output_tokens_per_turn == 0 || c.max_output_tokens_per_turn > 16 * 1024 ||
        c.max_recoveries > 8 || c.max_output_continuations > 8 || profile.timeout.count() <= 0 ||
        profile.timeout > std::chrono::minutes(10) || profile.max_input_tokens_per_request == 0 ||
        profile.max_input_tokens_per_request > 1024 * 1024 || profile.allowed_tools.size() > 128) {
        return error(ErrorCode::InvalidArgument, "invalid subagent profile");
    }
    if (profile.output_schema &&
        (!profile.output_schema->valid() || !gate_schema_subset(*profile.output_schema))) {
        return error(ErrorCode::InvalidArgument, "invalid subagent output schema");
    }
    const std::lock_guard lock(s.mutex);
    if (s.started || s.closed || s.profiles_sealed)
        return error(ErrorCode::InvalidState, "subagent profiles are sealed");
    if (s.profiles.size() == 32)
        return error(ErrorCode::ResourceExhausted, "subagent profile capacity exceeded");
    const auto name = profile.name;
    if (!s.profiles.emplace(name, std::make_shared<const SubagentProfile>(std::move(profile)))
             .second) {
        return error(ErrorCode::AlreadyExists, "subagent profile already exists");
    }
    return {};
}

Result<SubagentHandle> SubagentCoordinator::State::spawn(const std::string &name,
                                                         std::string prompt,
                                                         const OperationContext &caller) {
    if (caller.session != parent.session_id || caller.task != parent.task_id ||
        caller.task_epoch != parent.task_epoch) {
        return error(ErrorCode::PermissionDenied, "delegation caller does not own this parent");
    }
    if (caller.cancelled_or_expired(Timestamp::now()) || cancelled.load()) {
        return error(caller.cancelled() || cancelled.load() ? ErrorCode::Cancelled
                                                            : ErrorCode::DeadlineExceeded,
                     "delegation caller is interrupted");
    }
    const std::lock_guard lock(mutex);
    if (!accepting || closed)
        return error(ErrorCode::InvalidState, "subagent admission is closed");
    const auto found = profiles.find(name);
    if (found == profiles.end())
        return error(ErrorCode::NotFound, "subagent profile is not registered");
    if (prompt.empty() || prompt.size() > config.max_prompt_bytes) {
        return error(ErrorCode::ResourceExhausted, "subagent prompt capacity exceeded");
    }
    const auto active =
        std::count_if(children.begin(), children.end(), [](const auto &c) { return !c->done; });
    const auto pending = std::count_if(children.begin(), children.end(),
                                       [](const auto &c) { return !c->snapshot.claimed; });
    if (children.size() >= config.max_children ||
        static_cast<std::size_t>(active) >= config.max_active ||
        static_cast<std::size_t>(pending) >= config.max_pending_results) {
        return error(ErrorCode::ResourceExhausted, "subagent execution/result capacity exceeded");
    }
    const auto &profile = *found->second;
    const auto turns = static_cast<std::uint64_t>(profile.conversation.max_turns);
    const auto inputs = turns * profile.max_input_tokens_per_request;
    const auto outputs = turns * profile.conversation.max_output_tokens_per_turn;
    if (turns > config.max_model_requests - reserved_requests ||
        inputs > config.max_input_tokens - reserved_inputs ||
        outputs > config.max_output_tokens - reserved_outputs) {
        return error(ErrorCode::ResourceExhausted,
                     "subagent cumulative delegation budget exceeded");
    }
    auto child = std::make_shared<Child>();
    child->snapshot.handle = {SessionId::generate(), TaskId::generate(), OperationId::generate()};
    child->snapshot.profile = name;
    child->profile = found->second;
    child->prompt = std::move(prompt);
    child->spec = {child->snapshot.handle.task_id, child->snapshot.handle.session_id, 1,
                   child->prompt, profile.model.id};
    child->context = caller;
    child->context.session = child->spec.session_id;
    child->context.task = child->spec.task_id;
    child->context.task_epoch = child->spec.task_epoch;
    child->context.operation = child->snapshot.handle.execution_id;
    child->context.started_at = Timestamp::now();
    const auto limit = std::chrono::steady_clock::now() + profile.timeout;
    child->context.deadline = caller.deadline ? std::min(*caller.deadline, limit) : limit;
    children.push_back(child);
    reserved_requests += turns;
    reserved_inputs += inputs;
    reserved_outputs += outputs;
    if (auto stored = emit("SubagentSpawned", &child->snapshot); !stored) {
        child->done = true;
        child->snapshot.claimed = true;
        child->snapshot.status = SubagentStatus::Failed;
        child->snapshot.safe_summary = stored.error().safe_message;
        return stored.error();
    }
    kairo::BlockingWorkerSpec worker;
    worker.name = "mira-child-" + child->snapshot.handle.execution_id.to_string();
    worker.config.thread_name = worker.name;
    const auto weak = weak_from_this();
    worker.worker = std::make_unique<subagent_support::Worker>(
        [weak, child](kairo::StopToken stop) {
            if (const auto state = weak.lock())
                state->run_child(child, stop);
        },
        [weak] {
            if (const auto state = weak.lock())
                state->cv.notify_all();
        });
    try {
        child->worker = executor.start_worker(std::move(worker));
    } catch (...) {
        child->worker.reset();
    }
    if (!child->worker || !child->worker->started()) {
        child->done = true;
        child->snapshot.claimed = true;
        child->snapshot.status = SubagentStatus::Failed;
        child->snapshot.safe_summary = "Executor rejected child worker";
        if (auto stored = emit("SubagentSettled", &child->snapshot, "submission-rejected"); !stored)
            audit_error = stored.error();
        cv.notify_all();
        return error(ErrorCode::ResourceExhausted, "Executor rejected child worker");
    }
    return child->snapshot.handle;
}

Result<SubagentSnapshot> SubagentCoordinator::State::get(const SubagentHandle &handle) const {
    const std::lock_guard lock(mutex);
    for (const auto &child : children)
        if (child->snapshot.handle == handle)
            return child->snapshot;
    return error(ErrorCode::NotFound, "subagent execution is not owned by this coordinator");
}

Result<void> SubagentCoordinator::State::cancel(const SubagentHandle &handle) {
    const std::lock_guard lock(mutex);
    for (const auto &child : children) {
        if (child->snapshot.handle != handle)
            continue;
        if (!child->done) {
            child->cancelled.store(true);
            if (child->worker)
                child->worker->request_stop();
            cv.notify_all();
            return emit("SubagentCancellationRequested", &child->snapshot);
        }
        return {};
    }
    return error(ErrorCode::NotFound, "subagent execution is not owned by this coordinator");
}

void SubagentCoordinator::State::stop(const char *reason) {
    const std::lock_guard lock(mutex);
    accepting = false;
    if (!cancelled.exchange(true)) {
        if (auto stored = emit("SubagentParentStopping", nullptr, reason); !stored)
            audit_error = stored.error();
    }
    for (const auto &child : children) {
        child->cancelled.store(true);
        if (child->worker)
            child->worker->request_stop();
    }
    if (parent_worker)
        parent_worker->request_stop();
    cv.notify_all();
}

Result<SubagentHandle> SubagentCoordinator::spawn(const std::string &profile, std::string prompt,
                                                  const OperationContext &caller) {
    return state_->spawn(profile, std::move(prompt), caller);
}
Result<SubagentSnapshot> SubagentCoordinator::get(const SubagentHandle &handle) const {
    return state_->get(handle);
}
Result<void> SubagentCoordinator::cancel(const SubagentHandle &handle) {
    return state_->cancel(handle);
}
void SubagentCoordinator::request_cancel() { state_->stop("cancelled"); }
void SubagentCoordinator::request_human_takeover() { state_->stop("human-takeover"); }

Result<std::shared_future<Result<ConversationLoopResult>>>
SubagentCoordinator::start_parent(std::unique_ptr<ConversationLoop> loop,
                                  OperationContext context) {
    auto s = state_;
    if (auto valid = s->validate(); !valid)
        return valid.error();
    if (!loop)
        return error(ErrorCode::InvalidArgument, "parent loop is required");
    const std::lock_guard lock(s->mutex);
    if (s->started || s->closed || s->cancelled.load())
        return error(ErrorCode::InvalidState, "parent already started or closed");
    s->started = true;
    s->accepting = true;
    context.session = s->parent.session_id;
    context.task = s->parent.task_id;
    context.task_epoch = s->parent.task_epoch;
    context.operation = s->parent_execution;
    context.started_at = Timestamp::now();
    const auto weak = std::weak_ptr<State>(s);
    loop->set_event_store(s->events, s->runtime, s->parent.session_id);
    loop->set_continuation_source(
        [weak](const AgentLoopSpec &spec, const OperationContext &ctx, bool wait) {
            if (const auto state = weak.lock())
                return state->collect(spec, ctx, wait);
            return Result<std::vector<std::string>>{
                error(ErrorCode::InvalidState, "subagent owner expired")};
        });
    auto promise = std::make_shared<std::promise<Result<ConversationLoopResult>>>();
    s->parent_result = promise->get_future().share();
    if (auto stored = s->emit("SubagentParentStarted"); !stored) {
        s->accepting = false;
        s->parent_done = true;
        promise->set_value(stored.error());
        return stored.error();
    }
    kairo::BlockingWorkerSpec worker;
    worker.name = "mira-parent-" + s->parent_execution.to_string();
    worker.config.thread_name = worker.name;
    worker.worker = std::make_unique<subagent_support::Worker>(
        [s, parent = std::shared_ptr<ConversationLoop>(std::move(loop)), context,
         promise](kairo::StopToken stop) mutable {
            {
                const std::lock_guard guard(s->mutex);
                s->parent_worker_id = std::this_thread::get_id();
            }
            const auto inherited = context.cancellation_requested;
            context.cancellation_requested = [weak = std::weak_ptr<State>(s), stop, inherited] {
                const auto state = weak.lock();
                return !state || state->cancelled.load() || stop.stop_requested() ||
                       (inherited && inherited());
            };
            Result<ConversationLoopResult> result{
                error(ErrorCode::Internal, "parent execution failed")};
            try {
                result = parent->run(s->parent, context);
            } catch (...) {
                result = error(ErrorCode::Internal, "parent execution raised an exception");
            }
            s->stop("parent-settled");
            {
                std::unique_lock guard(s->mutex);
                s->cv.wait(guard, [&] {
                    return std::all_of(s->children.begin(), s->children.end(),
                                       [](const auto &c) { return c->done; });
                });
                if (auto stored = s->emit("SubagentParentSettled"); !stored)
                    s->audit_error = stored.error();
                if (s->audit_error)
                    result = *s->audit_error;
                s->parent_done = true;
            }
            promise->set_value(std::move(result));
            s->cv.notify_all();
        },
        [weak] {
            if (const auto state = weak.lock())
                state->cv.notify_all();
        });
    try {
        s->parent_worker = s->executor.start_worker(std::move(worker));
    } catch (...) {
        s->parent_worker.reset();
    }
    if (!s->parent_worker || !s->parent_worker->started()) {
        s->accepting = false;
        s->parent_done = true;
        auto failure = error(ErrorCode::ResourceExhausted, "Executor rejected parent worker");
        if (auto stored = s->emit("SubagentParentRejected", nullptr, "submission-rejected");
            !stored)
            s->audit_error = stored.error();
        promise->set_value(failure);
        return failure;
    }
    return s->parent_result;
}

Result<void> SubagentCoordinator::close(std::chrono::milliseconds timeout) {
    auto s = state_;
    {
        const std::lock_guard lock(s->mutex);
        const auto self = std::this_thread::get_id();
        if ((!s->parent_done && self == s->parent_worker_id) ||
            std::any_of(s->children.begin(), s->children.end(),
                        [&](const auto &c) { return !c->done && c->worker_id == self; })) {
            return error(ErrorCode::InvalidState, "subagent close requires an external owner");
        }
    }
    const std::lock_guard serialize(s->close_mutex);
    s->stop("owner-close");
    {
        std::unique_lock lock(s->mutex);
        if (!s->cv.wait_for(lock, std::max(timeout, std::chrono::milliseconds{0}), [&] {
                return (!s->started || s->parent_done) &&
                       std::all_of(s->children.begin(), s->children.end(),
                                   [](const auto &c) { return c->done; });
            }))
            return error(ErrorCode::DeadlineExceeded,
                         "subagent close is waiting for cooperative execution cleanup");
    }
    if (s->parent_worker && s->parent_worker->started())
        s->parent_worker->stop();
    for (const auto &child : s->children)
        if (child->worker && child->worker->started())
            child->worker->stop();
    const std::lock_guard lock(s->mutex);
    s->closed = true;
    if (s->audit_error)
        return *s->audit_error;
    return {};
}
} // namespace mira
