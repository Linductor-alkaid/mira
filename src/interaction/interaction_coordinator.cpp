#include <mira/interaction_coordinator.hpp>

#include <mira/agent_loop.hpp>
#include <mira/conversation_loop.hpp>
#include <mira/environment.hpp>

#include <kairo/executor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mira {
namespace {

// The bounds of the per-task user constraint list. The list is the "latest
// user constraints" of one work item; it is bounded so a long conversation
// cannot grow one task's context without limit (RULE-08).
constexpr std::size_t kMaxConstraintsPerTask = 8;
constexpr std::size_t kMaxConstraintBytes = 1024;
constexpr std::size_t kMaxGoalBytes = 4096;
constexpr std::size_t kMaxClarificationBytes = 1024;

[[nodiscard]] std::string bound_text(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, limit));
}

[[nodiscard]] Error make_error(ErrorCode code, InteractionDomainCode domain_code,
                               std::string message, bool retryable = false) {
    return interaction_error(code, domain_code, std::move(message), retryable);
}

// The task state names of the frozen task state machine.
[[nodiscard]] std::string_view task_state_name(TaskState state) {
    switch (state) {
    case TaskState::Idle:
        return "Idle";
    case TaskState::Observing:
        return "Observing";
    case TaskState::Reasoning:
        return "Reasoning";
    case TaskState::Planning:
        return "Planning";
    case TaskState::Acting:
        return "Acting";
    case TaskState::Verifying:
        return "Verifying";
    case TaskState::Recovering:
        return "Recovering";
    case TaskState::Pausing:
        return "Pausing";
    case TaskState::Paused:
        return "Paused";
    case TaskState::TakeoverSettling:
        return "TakeoverSettling";
    case TaskState::SuspendedForTakeover:
        return "SuspendedForTakeover";
    case TaskState::Cancelling:
        return "Cancelling";
    case TaskState::Completed:
        return "Completed";
    case TaskState::Failed:
        return "Failed";
    case TaskState::Cancelled:
        return "Cancelled";
    }
    return "Idle";
}

// The fail-closed capability check of the coordinator (DEC-068 R9). A missing
// capability, an expired grant or a mismatched identity version rejects the
// input; the check never defaults to allow.
[[nodiscard]] bool permitted(const PrincipalContext &principal, const CoordinatorConfig &config) {
    if (principal.tenant_id.is_nil() || principal.user_id.is_nil()) {
        return false;
    }
    if (config.required_identity_version.has_value() &&
        principal.identity_version != *config.required_identity_version) {
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    return std::any_of(principal.grants.begin(), principal.grants.end(), [&](const auto &grant) {
        if (grant.capability != config.required_capability) {
            return false;
        }
        // The resource selector of a grant scopes environment effects, not the
        // admission of the principal's own conversation, so it does not
        // narrow this check.
        return grant.expires_at == std::chrono::system_clock::time_point{} ||
               grant.expires_at > now;
    });
}

[[nodiscard]] const PrincipalTaskEntry *find_task(const PrincipalState &state, TaskId task_id) {
    for (const auto &task : state.tasks) {
        if (task.task_id == task_id) {
            return &task;
        }
    }
    return nullptr;
}

[[nodiscard]] const PrincipalTaskEntry *task_of_input(const PrincipalState &state,
                                                      std::string_view input_id) {
    for (const auto &task : state.tasks) {
        for (const auto &candidate : task.input_ids) {
            if (candidate == input_id) {
                return &task;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] std::vector<const PrincipalTaskEntry *> live_tasks(const PrincipalState &state) {
    std::vector<const PrincipalTaskEntry *> live;
    for (const auto &task : state.tasks) {
        if (!is_terminal(task.state)) {
            live.push_back(&task);
        }
    }
    return live;
}

// The deterministic routing decision (DEC-068 §5). The inputs are the
// principal, the envelope and the principal state; a host suggestion is only
// adopted after it is validated against that state.
[[nodiscard]] RoutingDecision decide_route(const InputEnvelope &envelope,
                                           const PrincipalState &state,
                                           const std::optional<RouteSuggestion> &suggestion) {
    RoutingDecision decision;

    if (suggestion.has_value()) {
        const auto &proposal = *suggestion;
        switch (proposal.intent) {
        case RouteIntent::ContinueCurrent:
        case RouteIntent::SupplementTask:
        case RouteIntent::PauseTask:
        case RouteIntent::CancelTask: {
            if (!proposal.task_id.has_value()) {
                break;
            }
            const auto *target = find_task(state, *proposal.task_id);
            if (target == nullptr) {
                break;
            }
            if (is_terminal(target->state)) {
                // A terminal task is never revived. Continuing or correcting
                // it becomes a new task bound to its summary; pausing or
                // cancelling it is impossible (DEC-068 R2).
                if (proposal.intent == RouteIntent::ContinueCurrent ||
                    proposal.intent == RouteIntent::SupplementTask) {
                    decision.intent = RouteIntent::ResumeTask;
                    decision.session_id = target->session_id;
                    decision.task_id = target->task_id;
                    decision.rationale =
                        "the suggested task is terminal; the input resumes it through a new task";
                    return decision;
                }
                break;
            }
            decision.intent = proposal.intent;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                proposal.rationale.empty()
                    ? std::string("the suggested target is live and owned by the principal")
                    : proposal.rationale;
            return decision;
        }
        case RouteIntent::ResumeTask: {
            if (!proposal.task_id.has_value()) {
                break;
            }
            const auto *target = find_task(state, *proposal.task_id);
            if (target == nullptr || !is_terminal(target->state)) {
                break;
            }
            decision.intent = RouteIntent::ResumeTask;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                proposal.rationale.empty()
                    ? std::string("the suggested task is settled; the input resumes it "
                                  "through a new task")
                    : proposal.rationale;
            return decision;
        }
        case RouteIntent::NewTask:
            decision.intent = RouteIntent::NewTask;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input starts a new task")
                                     : proposal.rationale;
            return decision;
        case RouteIntent::Clarify:
            decision.intent = RouteIntent::Clarify;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input reference is ambiguous")
                                     : proposal.rationale;
            decision.clarification =
                proposal.clarification.value_or(std::string("Which item should this apply to?"));
            return decision;
        case RouteIntent::Reject:
            decision.intent = RouteIntent::Reject;
            decision.rationale = proposal.rationale.empty()
                                     ? std::string("the input was rejected by the host")
                                     : proposal.rationale;
            return decision;
        }
        // The suggestion did not survive validation: fall through to the
        // structural rules and say so in the rationale.
    }

    // Structural rule 1: an explicit reply reference names the work item.
    if (envelope.reply_to.has_value()) {
        const auto *target = task_of_input(state, *envelope.reply_to);
        if (target != nullptr && !is_terminal(target->state)) {
            decision.intent = RouteIntent::ContinueCurrent;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale = "the input replies to a known live task";
            return decision;
        }
        if (target != nullptr) {
            decision.intent = RouteIntent::ResumeTask;
            decision.session_id = target->session_id;
            decision.task_id = target->task_id;
            decision.rationale =
                "the input replies to a settled task; it resumes it through a new task";
            return decision;
        }
    }

    // Structural rule 2: the live tasks of the principal.
    const auto live = live_tasks(state);
    if (live.empty()) {
        decision.intent = RouteIntent::NewTask;
        decision.rationale = "the principal has no live task; the input starts a new one";
        return decision;
    }
    if (live.size() == 1) {
        decision.intent = RouteIntent::ContinueCurrent;
        decision.session_id = live.front()->session_id;
        decision.task_id = live.front()->task_id;
        decision.rationale = "the principal has exactly one live task; the input continues it";
        return decision;
    }
    // Several live tasks: the reference is ambiguous. A clarification is a
    // normal reply, not an error code (DEC-068 R4).
    decision.intent = RouteIntent::Clarify;
    decision.rationale = "the principal has several live tasks; the reference is ambiguous";
    std::string question = "Which item should this apply to?";
    for (const auto *task : live) {
        question += "\n- ";
        question += bound_text(task->goal, 120);
    }
    decision.clarification = bound_text(question, kMaxClarificationBytes);
    return decision;
}

} // namespace

std::size_t PrincipalKeyHash::operator()(const PrincipalKey &key) const noexcept {
    const auto tenant = Id128Hash{}(key.tenant_id.value);
    const auto user = Id128Hash{}(key.user_id.value);
    return tenant ^ (user + 0x9e3779b97f4a7c15ULL + (tenant << 6U) + (tenant >> 2U));
}

class InteractionCoordinator::Impl final {
  public:
    struct PrincipalEntry final {
        std::vector<SessionId> sessions;
        std::vector<PrincipalTaskEntry> tasks;
    };

    Impl(CoordinatorConfig coordinator_config, MiraRuntime &runtime_ref,
         kairo::Executor &executor_ref)
        : config(std::move(coordinator_config)), runtime(runtime_ref), executor(executor_ref) {}

    [[nodiscard]] PrincipalKey key_of(const PrincipalContext &principal) const {
        return PrincipalKey{principal.tenant_id, principal.user_id};
    }

    [[nodiscard]] PrincipalState snapshot(const PrincipalContext &principal) const {
        PrincipalState state;
        const auto found = registry.find(key_of(principal));
        if (found == registry.end()) {
            return state;
        }
        state.sessions = found->second.sessions;
        state.tasks = found->second.tasks;
        return state;
    }

    // The runtime owns the task state; the registry only caches it. The
    // cached state is refreshed from the runtime before it is read, so a
    // terminal task is never routed to again (DEC-068 R2).
    void refresh_task_states() {
        std::vector<TaskId> ids;
        {
            std::lock_guard lock(mutex);
            for (const auto &entry : registry) {
                for (const auto &task : entry.second.tasks) {
                    ids.push_back(task.task_id);
                }
            }
        }
        std::vector<std::pair<TaskId, TaskState>> states;
        states.reserve(ids.size());
        for (const auto &id : ids) {
            const auto snapshot = runtime.task_snapshot(id);
            if (snapshot) {
                states.emplace_back(id, snapshot.value().state);
            }
        }
        std::lock_guard lock(mutex);
        for (auto &entry : registry) {
            for (auto &task : entry.second.tasks) {
                for (const auto &item : states) {
                    if (task.task_id == item.first) {
                        task.state = item.second;
                    }
                }
            }
        }
    }

    [[nodiscard]] PrincipalTaskEntry *task_of(PrincipalEntry &entry, TaskId task_id) {
        for (auto &task : entry.tasks) {
            if (task.task_id == task_id) {
                return &task;
            }
        }
        return nullptr;
    }

    // Appends the input to the task's constraint list. The list is the
    // latest user constraints of this work item only: an unrelated input
    // never touches another task's constraints (DEC-068 §6).
    static void append_constraint(PrincipalTaskEntry &task, std::string constraint) {
        task.constraints.push_back(std::move(constraint));
        while (task.constraints.size() > kMaxConstraintsPerTask) {
            task.constraints.erase(task.constraints.begin());
        }
    }

    // The bounded context preparation of one work item. It runs as a finite
    // task on the shared executor; the coordinator retains and consumes the
    // future. The preparation never blocks on anything else, so it cannot
    // form a cycle with the coordinator.
    [[nodiscard]] Result<void> prepare_context(TaskId task_id) {
        if (stopping.load(std::memory_order_acquire)) {
            return make_error(ErrorCode::Cancelled, InteractionDomainCode::ExecutorRejected,
                              "the interaction coordinator is shutting down");
        }
        std::future<void> future;
        try {
            future = executor.submit_auto([this, task_id] {
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                std::lock_guard lock(mutex);
                for (auto &entry : registry) {
                    for (auto &task : entry.second.tasks) {
                        if (task.task_id != task_id) {
                            continue;
                        }
                        task.context_block = assemble_context(task);
                        return;
                    }
                }
            });
        } catch (const kairo::CapacityExhaustedException &exception) {
            return make_error(ErrorCode::ResourceExhausted, InteractionDomainCode::ExecutorRejected,
                              std::string("the executor refused the context preparation: ") +
                                  exception.what(),
                              true);
        } catch (const kairo::ExecutorStopping &exception) {
            return make_error(ErrorCode::Unavailable, InteractionDomainCode::ExecutorRejected,
                              std::string("the executor is stopping: ") + exception.what(), true);
        } catch (const std::runtime_error &exception) {
            // The executor refuses the submission when it is not running.
            // The refusal is retryable: the host may start a new executor.
            return make_error(ErrorCode::Unavailable, InteractionDomainCode::ExecutorRejected,
                              std::string("the executor refused the context preparation: ") +
                                  exception.what(),
                              true);
        } catch (const std::exception &exception) {
            return make_error(ErrorCode::Internal, InteractionDomainCode::ExecutorRejected,
                              std::string("the context preparation could not be submitted: ") +
                                  exception.what());
        }
        if (future.wait_for(config.command_timeout) != std::future_status::ready) {
            // The future is retired, not dropped: it is still consumed by the
            // drain below so no task result is lost.
            retired_futures.push_back(std::move(future));
            drain_retired();
            return make_error(ErrorCode::DeadlineExceeded,
                              InteractionDomainCode::RuntimeCommandTimeout,
                              "the context preparation did not settle in time", true);
        }
        try {
            future.get();
        } catch (const std::exception &exception) {
            return make_error(ErrorCode::Internal, InteractionDomainCode::ExecutorRejected,
                              std::string("the context preparation failed: ") + exception.what());
        }
        return Result<void>{};
    }

    // The context block of one work item: the goal, the latest user
    // constraints and the task state. It is bounded and belongs to this work
    // item only.
    [[nodiscard]] static std::string assemble_context(const PrincipalTaskEntry &task) {
        std::string block = "goal: ";
        block += bound_text(task.goal, kMaxGoalBytes);
        block += "\nstate: ";
        block += task_state_name(task.state);
        if (!task.constraints.empty()) {
            block += "\nconstraints:";
            for (const auto &constraint : task.constraints) {
                block += "\n- ";
                block += constraint;
            }
        }
        return block;
    }

    // Consumes the futures of the preparations that timed out on the wait
    // side. A future that never settles is reported, never silently dropped.
    void drain_retired() {
        auto it = retired_futures.begin();
        while (it != retired_futures.end()) {
            if (it->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            try {
                it->get();
            } catch (...) {
            }
            it = retired_futures.erase(it);
        }
    }

    CoordinatorConfig config;
    MiraRuntime &runtime;
    kairo::Executor &executor;
    std::shared_ptr<IEventStore> events;
    RuntimeId runtime_id;
    std::shared_ptr<IInputLedger> ledger;
    RouteResolver resolver;
    EnvironmentProvider environment_provider;

    mutable std::mutex mutex;
    // Serializes the coordinator's own submissions: the registry, the pending
    // deliveries and the retired futures have one writer.
    std::mutex submit_mutex;
    std::unordered_map<PrincipalKey, PrincipalEntry, PrincipalKeyHash> registry;
    std::unordered_map<TaskId, std::shared_ptr<IUserInputSink>, StrongIdHash<TaskId>> sinks;
    std::unordered_map<TaskId, std::vector<InputEnvelope>, StrongIdHash<TaskId>> pending;
    std::vector<std::future<void>> retired_futures;
    std::atomic<bool> stopping{false};
    bool initialized = false;
};

InteractionCoordinator::InteractionCoordinator(CoordinatorConfig config, MiraRuntime &runtime,
                                               kairo::Executor &executor)
    : impl_(std::make_unique<Impl>(std::move(config), runtime, executor)) {}

InteractionCoordinator::~InteractionCoordinator() { shutdown(); }

void InteractionCoordinator::set_event_store(std::shared_ptr<IEventStore> events,
                                             RuntimeId runtime) {
    impl_->events = std::move(events);
    impl_->runtime_id = runtime;
}

void InteractionCoordinator::set_input_ledger(std::shared_ptr<IInputLedger> ledger) {
    impl_->ledger = std::move(ledger);
}

void InteractionCoordinator::set_environment_provider(EnvironmentProvider provider) {
    impl_->environment_provider = std::move(provider);
}

void InteractionCoordinator::set_route_resolver(RouteResolver resolver) {
    impl_->resolver = std::move(resolver);
}

Result<void> InteractionCoordinator::initialize() {
    if (impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was already initialized");
    }
    if (impl_->ledger == nullptr) {
        if (impl_->events == nullptr) {
            return make_error(
                ErrorCode::InvalidState, InteractionDomainCode::LedgerUnavailable,
                "the interaction coordinator needs an event store for its durable ledger");
        }
        InputLedgerOptions options;
        options.idempotency_window = impl_->config.idempotency_window;
        options.max_entries = impl_->config.max_ledger_entries;
        impl_->ledger =
            std::make_shared<EventStoreInputLedger>(impl_->events, impl_->runtime_id, options);
    }
    // The registry is a projection of the durable ledger stream: rebuilding
    // it here restores the ownership map after a restart (DEC-068 §8).
    if (auto *ledger = dynamic_cast<EventStoreInputLedger *>(impl_->ledger.get());
        ledger != nullptr) {
        const auto rebuilt = ledger->rebuild();
        if (!rebuilt) {
            return rebuilt.error();
        }
    }
    impl_->initialized = true;
    return Result<void>{};
}

Result<InputDisposition> InteractionCoordinator::submit_input(const InputEnvelope &envelope) {
    if (impl_->stopping.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::Unavailable, InteractionDomainCode::RouteRejected,
                          "the interaction coordinator is shutting down");
    }
    if (!impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    }
    if (envelope.input_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                          "the input id must not be empty");
    }
    // The coordinator serializes its own submissions: the registry, the
    // pending deliveries and the retired futures have one writer.
    std::lock_guard submit_lock(impl_->submit_mutex);

    // Admission is atomic: a repeat inside the idempotency window returns the
    // existing record and produces no second dispatch.
    auto admitted = impl_->ledger->admit(envelope);
    if (!admitted) {
        // An expired repeat is an explicit rejection and is never executed as
        // a new input.
        InputDisposition disposition;
        disposition.input_id = envelope.input_id;
        disposition.stage = InputStage::Rejected;
        disposition.error = admitted.error();
        return disposition;
    }
    const auto entry = std::move(admitted).value();
    if (entry.duplicate) {
        InputDisposition disposition;
        disposition.ingress_id = entry.ingress_id;
        disposition.input_id = entry.input_id;
        disposition.stage = entry.stage;
        disposition.duplicate = true;
        disposition.session_id = entry.session_id;
        disposition.task_id = entry.task_id;
        disposition.clarification = entry.clarification;
        disposition.error = entry.error;
        return disposition;
    }

    // The fail-closed capability check gates the routing commit
    // (DEC-068 §4.7). A denied input is recorded as rejected.
    if (!permitted(envelope.principal, impl_->config)) {
        const auto reason =
            make_error(ErrorCode::PermissionDenied, InteractionDomainCode::PermissionDenied,
                       "the principal lacks the required capability");
        LedgerTransition transition;
        transition.stage = InputStage::Rejected;
        transition.note = reason.safe_message;
        transition.error = reason;
        const auto rejected = impl_->ledger->advance(entry.input_id, transition);
        InputDisposition disposition;
        disposition.ingress_id = entry.ingress_id;
        disposition.input_id = entry.input_id;
        disposition.stage = InputStage::Rejected;
        disposition.error = reason;
        if (rejected) {
            disposition.error = rejected.value().error;
        }
        return disposition;
    }

    PrincipalState state;
    impl_->refresh_task_states();
    {
        std::lock_guard lock(impl_->mutex);
        state = impl_->snapshot(envelope.principal);
    }
    std::optional<RouteSuggestion> suggestion;
    if (impl_->resolver) {
        suggestion = impl_->resolver(envelope, state);
    }
    const auto decision = decide_route(envelope, state, suggestion);

    InputDisposition disposition;
    disposition.ingress_id = entry.ingress_id;
    disposition.input_id = entry.input_id;
    // The routing rationale is host-provided text; it is bounded before it
    // reaches an error message or the durable event stream.
    const auto rationale = bound_text(decision.rationale, 512);

    switch (decision.intent) {
    case RouteIntent::Reject: {
        const auto reason =
            make_error(ErrorCode::SafetyRejected, InteractionDomainCode::RouteRejected, rationale);
        LedgerTransition transition;
        transition.stage = InputStage::Rejected;
        transition.note = rationale;
        transition.error = reason;
        const auto rejected = impl_->ledger->advance(entry.input_id, transition);
        disposition.stage = InputStage::Rejected;
        disposition.error = reason;
        if (rejected) {
            disposition.error = rejected.value().error;
        }
        return disposition;
    }
    case RouteIntent::Clarify: {
        LedgerTransition transition;
        transition.stage = InputStage::Accepted;
        transition.clarification = decision.clarification;
        transition.note = rationale;
        const auto clarified = impl_->ledger->advance(entry.input_id, transition);
        disposition.stage = InputStage::Accepted;
        disposition.clarification = decision.clarification;
        if (!clarified) {
            disposition.stage = InputStage::Rejected;
            disposition.error = clarified.error();
        }
        return disposition;
    }
    case RouteIntent::PauseTask:
    case RouteIntent::CancelTask: {
        // The deterministic control path: the emergency stop never depends on
        // a model routing round (DEC-068 R6).
        if (!decision.task_id.has_value()) {
            const auto reason =
                make_error(ErrorCode::InvalidState, InteractionDomainCode::RouteRejected,
                           "the routing decision has no task to act on");
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        const auto command = decision.intent == RouteIntent::PauseTask
                                 ? impl_->runtime.pause_task(*decision.task_id)
                                 : impl_->runtime.cancel_task(*decision.task_id);
        if (!command) {
            const auto reason =
                make_error(ErrorCode::InvalidState, InteractionDomainCode::RuntimeCommandFailed,
                           command.error().safe_message);
            LedgerTransition transition;
            transition.stage = InputStage::Rejected;
            transition.note = reason.safe_message;
            transition.error = reason;
            (void)impl_->ledger->advance(entry.input_id, transition);
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        const auto outcome = command.value().outcome(impl_->config.command_timeout);
        if (!outcome) {
            const auto reason = make_error(ErrorCode::DeadlineExceeded,
                                           InteractionDomainCode::RuntimeCommandTimeout,
                                           outcome.error().safe_message, true);
            LedgerTransition transition;
            transition.stage = InputStage::Rejected;
            transition.note = reason.safe_message;
            transition.error = reason;
            (void)impl_->ledger->advance(entry.input_id, transition);
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        LedgerTransition transition;
        transition.stage = InputStage::Routed;
        transition.session_id = decision.session_id;
        transition.task_id = decision.task_id;
        transition.intent = decision.intent;
        transition.note = rationale;
        const auto routed = impl_->ledger->advance(entry.input_id, transition);
        disposition.stage = InputStage::Routed;
        disposition.session_id = decision.session_id;
        disposition.task_id = decision.task_id;
        if (!routed) {
            disposition.stage = InputStage::Rejected;
            disposition.error = routed.error();
        }
        return disposition;
    }
    case RouteIntent::ContinueCurrent:
    case RouteIntent::SupplementTask:
    case RouteIntent::NewTask:
    case RouteIntent::ResumeTask:
        break;
    }

    // The routed work item: an existing live task, a new task, or a new task
    // bound to the summary of a settled one.
    TaskId target_task;
    SessionId target_session;
    std::string goal;
    std::vector<std::string> inherited;
    if (decision.intent == RouteIntent::ContinueCurrent ||
        decision.intent == RouteIntent::SupplementTask) {
        if (!decision.task_id.has_value() || !decision.session_id.has_value()) {
            const auto reason =
                make_error(ErrorCode::InvalidState, InteractionDomainCode::RouteRejected,
                           "the routing decision has no live target");
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        target_task = *decision.task_id;
        target_session = *decision.session_id;
    } else {
        std::optional<SessionId> session_to_use = decision.session_id;
        if (!session_to_use.has_value()) {
            // A new work item needs a session; the host owns the environment.
            {
                // The bounded session count is checked before the session is
                // created: a full principal is rejected without leaving an
                // empty session behind.
                std::lock_guard lock(impl_->mutex);
                const auto found = impl_->registry.find(impl_->key_of(envelope.principal));
                if (found != impl_->registry.end() &&
                    found->second.sessions.size() >= impl_->config.max_sessions_per_principal) {
                    const auto reason = make_error(ErrorCode::ResourceExhausted,
                                                   InteractionDomainCode::RouteRejected,
                                                   "the principal has too many tracked sessions");
                    LedgerTransition transition;
                    transition.stage = InputStage::Rejected;
                    transition.note = reason.safe_message;
                    transition.error = reason;
                    (void)impl_->ledger->advance(entry.input_id, transition);
                    disposition.stage = InputStage::Rejected;
                    disposition.error = reason;
                    return disposition;
                }
            }
            if (!impl_->environment_provider) {
                const auto reason =
                    make_error(ErrorCode::InvalidState, InteractionDomainCode::NoSessionAvailable,
                               "no environment provider is bound to the coordinator");
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            const auto environment = impl_->environment_provider();
            if (!environment) {
                const auto reason =
                    make_error(ErrorCode::Unavailable, InteractionDomainCode::NoSessionAvailable,
                               environment.error().safe_message, true);
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            // The derived principal is a trim, never an elevation: the
            // coordinator creates the session with the caller's own grants
            // (DEC-068 §2).
            SessionConfig session_config;
            session_config.principal = envelope.principal;
            const auto opened = impl_->runtime.open_session(environment.value(), session_config);
            if (!opened) {
                const auto reason =
                    make_error(ErrorCode::InvalidState, InteractionDomainCode::RuntimeCommandFailed,
                               opened.error().safe_message);
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            const auto settled = opened.value().command.outcome(impl_->config.command_timeout);
            if (!settled) {
                const auto reason = make_error(ErrorCode::DeadlineExceeded,
                                               InteractionDomainCode::RuntimeCommandTimeout,
                                               settled.error().safe_message, true);
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            session_to_use = opened.value().id;
            {
                std::lock_guard lock(impl_->mutex);
                auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
                principal.sessions.push_back(opened.value().id);
            }
        }
        target_session = *session_to_use;
        if (decision.intent == RouteIntent::ResumeTask && decision.task_id.has_value()) {
            const auto *previous = find_task(state, *decision.task_id);
            if (previous != nullptr) {
                goal = previous->goal;
                inherited = previous->constraints;
            }
        }
        if (goal.empty()) {
            goal = bound_text(envelope.content, kMaxGoalBytes);
        }
        TaskSpec spec;
        spec.goal = goal;
        spec.user_constraints = inherited;
        const auto submitted = impl_->runtime.submit_task(target_session, spec);
        if (!submitted) {
            const auto reason =
                make_error(ErrorCode::InvalidState, InteractionDomainCode::RuntimeCommandFailed,
                           submitted.error().safe_message);
            LedgerTransition transition;
            transition.stage = InputStage::Rejected;
            transition.note = reason.safe_message;
            transition.error = reason;
            (void)impl_->ledger->advance(entry.input_id, transition);
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        const auto settled = submitted.value().command.outcome(impl_->config.command_timeout);
        if (!settled) {
            const auto reason = make_error(ErrorCode::DeadlineExceeded,
                                           InteractionDomainCode::RuntimeCommandTimeout,
                                           settled.error().safe_message, true);
            LedgerTransition transition;
            transition.stage = InputStage::Rejected;
            transition.note = reason.safe_message;
            transition.error = reason;
            (void)impl_->ledger->advance(entry.input_id, transition);
            disposition.stage = InputStage::Rejected;
            disposition.error = reason;
            return disposition;
        }
        target_task = submitted.value().id;
        {
            std::lock_guard lock(impl_->mutex);
            auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
            if (principal.tasks.size() >= impl_->config.max_tasks_per_principal) {
                const auto reason =
                    make_error(ErrorCode::ResourceExhausted, InteractionDomainCode::RouteRejected,
                               "the principal has too many tracked tasks");
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            PrincipalTaskEntry task_entry;
            task_entry.task_id = target_task;
            task_entry.session_id = target_session;
            task_entry.state = TaskState::Idle;
            task_entry.goal = goal;
            task_entry.constraints = inherited;
            if (decision.task_id.has_value()) {
                task_entry.resumed_from = decision.task_id;
            }
            principal.tasks.push_back(std::move(task_entry));
        }
    }

    // The routed input updates the constraints of its own work item only.
    {
        std::lock_guard lock(impl_->mutex);
        auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
        if (auto *task = impl_->task_of(principal, target_task); task != nullptr) {
            task->input_ids.push_back(entry.input_id);
            if (decision.intent == RouteIntent::SupplementTask ||
                decision.intent == RouteIntent::ContinueCurrent) {
                Impl::append_constraint(*task, bound_text(envelope.content, kMaxConstraintBytes));
            }
        }
    }

    // The context preparation of the work item runs on the shared executor.
    const auto prepared = impl_->prepare_context(target_task);
    if (!prepared) {
        const auto &reason = prepared.error();
        LedgerTransition transition;
        transition.stage = InputStage::Rejected;
        transition.note = reason.safe_message;
        transition.error = reason;
        (void)impl_->ledger->advance(entry.input_id, transition);
        disposition.stage = InputStage::Rejected;
        disposition.error = reason;
        return disposition;
    }

    // Delivery: a bound sink receives the input now, otherwise the input
    // waits for the loop of that task.
    std::string delivery_note;
    {
        std::shared_ptr<IUserInputSink> sink;
        {
            std::lock_guard lock(impl_->mutex);
            const auto found = impl_->sinks.find(target_task);
            if (found != impl_->sinks.end()) {
                sink = found->second;
            }
        }
        if (sink) {
            const auto delivered = sink->deliver(envelope, target_task);
            if (!delivered) {
                const auto reason =
                    make_error(ErrorCode::Unavailable, InteractionDomainCode::SinkUnavailable,
                               delivered.error().safe_message);
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            delivery_note = "delivered";
        } else {
            std::lock_guard lock(impl_->mutex);
            auto &queue = impl_->pending[target_task];
            if (queue.size() >= impl_->config.max_pending_deliveries) {
                const auto reason =
                    make_error(ErrorCode::ResourceExhausted, InteractionDomainCode::SinkUnavailable,
                               "too many inputs are waiting for the loop of this task");
                LedgerTransition transition;
                transition.stage = InputStage::Rejected;
                transition.note = reason.safe_message;
                transition.error = reason;
                (void)impl_->ledger->advance(entry.input_id, transition);
                disposition.stage = InputStage::Rejected;
                disposition.error = reason;
                return disposition;
            }
            queue.push_back(envelope);
            delivery_note = "awaiting the loop of the task";
        }
    }

    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.session_id = target_session;
    transition.task_id = target_task;
    transition.intent = decision.intent;
    transition.note = rationale + " (" + delivery_note + ")";
    const auto routed = impl_->ledger->advance(entry.input_id, transition);
    disposition.stage = InputStage::Routed;
    disposition.session_id = target_session;
    disposition.task_id = target_task;
    if (!routed) {
        disposition.stage = InputStage::Rejected;
        disposition.error = routed.error();
    }
    return disposition;
}

Result<CommandHandle> InteractionCoordinator::cancel_work(TaskId task_id) {
    return impl_->runtime.cancel_task(task_id);
}

Result<void> InteractionCoordinator::register_sink(TaskId task_id,
                                                   std::shared_ptr<IUserInputSink> sink) {
    if (sink == nullptr) {
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::SinkUnavailable,
                          "the input sink must not be null");
    }
    std::lock_guard submit_lock(impl_->submit_mutex);
    std::vector<InputEnvelope> waiting;
    std::shared_ptr<IUserInputSink> bound;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->sinks.size() >= impl_->config.max_sinks && !impl_->sinks.contains(task_id)) {
            return make_error(ErrorCode::ResourceExhausted, InteractionDomainCode::SinkUnavailable,
                              "too many input sinks are registered");
        }
        bound = std::move(sink);
        impl_->sinks[task_id] = bound;
        const auto found = impl_->pending.find(task_id);
        if (found != impl_->pending.end()) {
            waiting = std::move(found->second);
            impl_->pending.erase(found);
        }
    }
    for (const auto &envelope : waiting) {
        const auto delivered = bound->deliver(envelope, task_id);
        if (!delivered) {
            // The input stays pending for the next sink: a refused delivery
            // is never dropped and never executed twice.
            std::lock_guard lock(impl_->mutex);
            impl_->pending[task_id].insert(impl_->pending[task_id].begin(), envelope);
            return make_error(ErrorCode::Unavailable, InteractionDomainCode::SinkUnavailable,
                              delivered.error().safe_message);
        }
    }
    return Result<void>{};
}

Result<void> InteractionCoordinator::unregister_sink(TaskId task_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->sinks.erase(task_id);
    return Result<void>{};
}

Result<InputDisposition> InteractionCoordinator::record_answer(std::string_view input_id,
                                                               std::string summary) {
    if (!impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    }
    LedgerTransition transition;
    transition.stage = InputStage::Answered;
    transition.note = bound_text(summary, kMaxConstraintBytes);
    const auto answered = impl_->ledger->advance(input_id, transition);
    if (!answered) {
        return answered.error();
    }
    const auto &entry = answered.value();
    InputDisposition disposition;
    disposition.ingress_id = entry.ingress_id;
    disposition.input_id = entry.input_id;
    disposition.stage = entry.stage;
    disposition.session_id = entry.session_id;
    disposition.task_id = entry.task_id;
    disposition.clarification = entry.clarification;
    return disposition;
}

Result<InputDisposition> InteractionCoordinator::record_settlement(std::string_view input_id,
                                                                   TaskState terminal) {
    if (!impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    }
    if (!is_terminal(terminal)) {
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                          "a settlement needs a terminal task state");
    }
    LedgerTransition transition;
    transition.stage = InputStage::TaskSettled;
    transition.note = std::string("terminal: ") + std::string(task_state_name(terminal));
    const auto settled = impl_->ledger->advance(input_id, transition);
    if (!settled) {
        return settled.error();
    }
    const auto &entry = settled.value();
    InputDisposition disposition;
    disposition.ingress_id = entry.ingress_id;
    disposition.input_id = entry.input_id;
    disposition.stage = entry.stage;
    disposition.session_id = entry.session_id;
    disposition.task_id = entry.task_id;
    return disposition;
}

Result<InputDisposition> InteractionCoordinator::record_rejection(std::string_view input_id,
                                                                  std::string reason) {
    if (!impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    }
    const auto failure = make_error(ErrorCode::SafetyRejected, InteractionDomainCode::RouteRejected,
                                    bound_text(reason, kMaxConstraintBytes));
    LedgerTransition transition;
    transition.stage = InputStage::Rejected;
    transition.note = failure.safe_message;
    transition.error = failure;
    const auto rejected = impl_->ledger->advance(input_id, transition);
    if (!rejected) {
        return rejected.error();
    }
    const auto &entry = rejected.value();
    InputDisposition disposition;
    disposition.ingress_id = entry.ingress_id;
    disposition.input_id = entry.input_id;
    disposition.stage = entry.stage;
    disposition.session_id = entry.session_id;
    disposition.task_id = entry.task_id;
    disposition.error = entry.error;
    return disposition;
}

Result<PrincipalState>
InteractionCoordinator::principal_state(const PrincipalContext &principal) const {
    impl_->refresh_task_states();
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot(principal);
}

Result<LedgerEntry> InteractionCoordinator::input_record(std::string_view input_id) const {
    return impl_->ledger->lookup(input_id);
}

void InteractionCoordinator::shutdown() {
    if (impl_->stopping.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard submit_lock(impl_->submit_mutex);
    // The coordinator stops producing work, then consumes the futures it
    // still holds. The Runtime and the Executor stay owned by the host.
    {
        std::lock_guard lock(impl_->mutex);
        impl_->sinks.clear();
        impl_->pending.clear();
    }
    for (auto &future : impl_->retired_futures) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            try {
                future.get();
            } catch (...) {
            }
        }
    }
    impl_->retired_futures.clear();
    impl_->initialized = false;
}

// ---------------------------------------------------------------------------
// Loop adapters
// ---------------------------------------------------------------------------

Result<void> AgentLoopInputSink::deliver(const InputEnvelope &envelope, TaskId) {
    if (loop_ == nullptr) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::SinkUnavailable,
                          "the agent loop sink has no loop");
    }
    const auto delivered = loop_->enqueue_user_message(envelope.content);
    if (!delivered) {
        return delivered.error();
    }
    last_input_id_ = envelope.input_id;
    return Result<void>{};
}

Result<void> ConversationLoopInputSink::deliver(const InputEnvelope &envelope, TaskId) {
    if (loop_ == nullptr) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::SinkUnavailable,
                          "the conversation loop sink has no loop");
    }
    const auto delivered = loop_->enqueue_user_message(envelope.content);
    if (!delivered) {
        return delivered.error();
    }
    last_input_id_ = envelope.input_id;
    return Result<void>{};
}

} // namespace mira
