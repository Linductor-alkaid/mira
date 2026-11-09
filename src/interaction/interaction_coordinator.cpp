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

#include "interaction_coordinator_support.hpp"

namespace mira {
namespace {

// The error of the interaction layer; the domain code keeps the failures
// machine-readable.
[[nodiscard]] Error make_error(ErrorCode code, InteractionDomainCode domain_code,
                               std::string message, bool retryable = false) {
    return interaction_error(code, domain_code, std::move(message), retryable);
}

using mira::interaction_support::bound_text;
using mira::interaction_support::decide_route;
using mira::interaction_support::find_task;
using mira::interaction_support::kMaxClarificationBytes;
using mira::interaction_support::kMaxConstraintBytes;
using mira::interaction_support::kMaxConstraintsPerTask;
using mira::interaction_support::kMaxGoalBytes;
using mira::interaction_support::permitted;

} // namespace

std::size_t PrincipalKeyHash::operator()(const PrincipalKey &key) const noexcept {
    const auto tenant = Id128Hash{}(key.tenant_id.value);
    const auto user = Id128Hash{}(key.user_id.value);
    return tenant ^ (user + 0x9e3779b97f4a7c15ULL + (tenant << 6U) + (tenant >> 2U));
}

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
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (impl_->stopping.load(std::memory_order_acquire))
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "a closed coordinator cannot be reinitialized");
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
    // Rebuild only the input ledger. Registry/context recovery remains M29-05;
    // this must not be advertised as restoration of live Runtime tasks.
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
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (impl_->stopping.load(std::memory_order_acquire))
        return make_error(ErrorCode::Unavailable, InteractionDomainCode::RouteRejected,
                          "the interaction coordinator is shutting down");
    if (!impl_->initialized)
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    if (envelope.input_id.empty())
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                          "the input id must not be empty");

    InputDisposition disposition;
    disposition.input_id = envelope.input_id;
    const auto admitted = impl_->ledger->admit(envelope);
    if (!admitted) {
        disposition.stage = InputStage::Rejected;
        disposition.error =
            permitted(envelope.principal, impl_->config)
                ? admitted.error()
                : make_error(ErrorCode::PermissionDenied, InteractionDomainCode::PermissionDenied,
                             "the principal lacks the required capability");
        return disposition;
    }
    const auto &entry = admitted.value();
    disposition.ingress_id = entry.ingress_id;
    // Fresh permission denials retain M28's durable rejection. A denied
    // retry cannot reveal or change an earlier record, even of its own id.
    if (!permitted(envelope.principal, impl_->config)) {
        disposition.stage = InputStage::Rejected;
        disposition.ingress_id = {};
        disposition.error =
            make_error(ErrorCode::PermissionDenied, InteractionDomainCode::PermissionDenied,
                       "the principal lacks the required capability");
        if (!entry.duplicate) {
            LedgerTransition denied;
            denied.stage = InputStage::Rejected;
            denied.error = disposition.error;
            const auto recorded = impl_->ledger->advance(entry.input_id, denied);
            if (!recorded)
                return recorded.error();
            disposition.ingress_id = entry.ingress_id;
        }
        return disposition;
    }
    if (entry.duplicate) {
        disposition.stage = entry.stage;
        disposition.duplicate = true;
        disposition.session_id = entry.session_id;
        disposition.task_id = entry.task_id;
        disposition.clarification = entry.clarification;
        disposition.candidates = entry.candidates;
        disposition.error = entry.error;
        return disposition;
    }
    const auto reject = [&](Error reason) -> Result<InputDisposition> {
        LedgerTransition transition;
        transition.stage = InputStage::Rejected;
        transition.error = reason;
        transition.note = reason.safe_message;
        const auto rejected = impl_->ledger->advance(entry.input_id, transition);
        if (!rejected)
            return rejected.error();
        disposition.stage = InputStage::Rejected;
        disposition.error = std::move(reason);
        return disposition;
    };
    const auto wait_applied = [&](const CommandHandle &handle) -> Result<void> {
        const auto outcome = handle.outcome(impl_->config.command_timeout);
        if (!outcome)
            return make_error(ErrorCode::DeadlineExceeded,
                              InteractionDomainCode::RuntimeCommandTimeout,
                              "the runtime command did not settle in time", true);
        if (outcome.value().status != SettlementStatus::Applied &&
            outcome.value().status != SettlementStatus::NoOp)
            return make_error(ErrorCode::InvalidState, InteractionDomainCode::RuntimeCommandFailed,
                              "the runtime command was not applied");
        return Result<void>{};
    };

    impl_->refresh_task_states();
    PrincipalState state;
    {
        std::lock_guard lock(impl_->mutex);
        state = impl_->snapshot(envelope.principal);
    }
    std::optional<RouteSuggestion> suggestion;
    if (!envelope.target_task && !envelope.reply_to && !envelope.requested_intent &&
        impl_->resolver) {
        try {
            suggestion = impl_->resolver(envelope, state);
        } catch (...) {
            return reject(make_error(ErrorCode::Internal, InteractionDomainCode::RouteRejected,
                                     "the route resolver failed"));
        }
    }
    const auto decision = decide_route(envelope, state, suggestion);
    const auto rationale = bound_text(decision.rationale, 512);
    if (decision.intent == RouteIntent::Reject)
        return reject(
            make_error(ErrorCode::SafetyRejected, InteractionDomainCode::RouteRejected, rationale));
    if (decision.intent == RouteIntent::Clarify) {
        LedgerTransition transition;
        transition.clarification = decision.clarification;
        transition.candidates = decision.candidates;
        transition.note = rationale;
        const auto clarified = impl_->ledger->advance(entry.input_id, transition);
        if (!clarified)
            return clarified.error();
        disposition.clarification = clarified.value().clarification;
        disposition.candidates = clarified.value().candidates;
        return disposition;
    }

    if (decision.intent == RouteIntent::PauseTask || decision.intent == RouteIntent::CancelTask) {
        const auto command = decision.intent == RouteIntent::PauseTask
                                 ? impl_->runtime.pause_task(*decision.task_id)
                                 : impl_->runtime.cancel_task(*decision.task_id);
        if (!command)
            return reject(make_error(ErrorCode::InvalidState,
                                     InteractionDomainCode::RuntimeCommandFailed,
                                     "the runtime refused the control command"));
        const auto applied = wait_applied(command.value());
        if (!applied)
            return reject(applied.error());
        LedgerTransition transition;
        transition.stage = InputStage::Routed;
        transition.session_id = decision.session_id;
        transition.task_id = decision.task_id;
        transition.intent = decision.intent;
        transition.note = rationale;
        const auto routed = impl_->ledger->advance(entry.input_id, transition);
        if (!routed)
            return routed.error();
        disposition.stage = InputStage::Routed;
        disposition.session_id = decision.session_id;
        disposition.task_id = decision.task_id;
        return disposition;
    }

    const bool new_task =
        decision.intent == RouteIntent::NewTask || decision.intent == RouteIntent::ResumeTask;
    PrincipalTaskEntry candidate;
    if (new_task) {
        // Check all tracked bounds before opening runtime objects.
        if (state.tasks.size() >= impl_->config.max_tasks_per_principal)
            return reject(make_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::RouteRejected,
                                     "the principal has too many tracked tasks"));
        candidate.goal = bound_text(envelope.content, kMaxGoalBytes);
        if (decision.intent == RouteIntent::ResumeTask) {
            const auto *previous = find_task(state, *decision.task_id);
            candidate.goal = previous->goal;
            candidate.constraints = previous->constraints;
            candidate.resumed_from = previous->task_id;
            // Resumption must carry the new requirement as well as old context.
            Impl::append_constraint(candidate, bound_text(envelope.content, kMaxConstraintBytes));
        }
    } else {
        candidate = *find_task(state, *decision.task_id);
        if (candidate.input_ids.size() >= impl_->config.max_inputs_per_task)
            return reject(make_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::RouteRejected,
                                     "the task has too many associated inputs"));
        if (impl_->uncertain.contains(candidate.task_id))
            return reject(impl_->uncertain.at(candidate.task_id));
        const auto waiting = impl_->pending.find(candidate.task_id);
        if (waiting != impl_->pending.end() &&
            waiting->second.size() >= impl_->config.max_pending_deliveries)
            return reject(make_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::SinkUnavailable,
                                     "the task input queue is full"));
        Impl::append_constraint(candidate, bound_text(envelope.content, kMaxConstraintBytes));
    }
    if (impl_->config.max_pending_deliveries == 0 || impl_->config.max_inputs_per_task == 0)
        return reject(make_error(ErrorCode::ResourceExhausted,
                                 InteractionDomainCode::SinkUnavailable,
                                 "task input capacity is zero"));
    const auto prepared = impl_->prepare_context(candidate);
    if (!prepared)
        return reject(prepared.error());
    candidate.context_block = prepared.value();

    if (new_task) {
        auto session = decision.session_id;
        if (!session) {
            if (state.sessions.size() >= impl_->config.max_sessions_per_principal)
                return reject(make_error(ErrorCode::ResourceExhausted,
                                         InteractionDomainCode::RouteRejected,
                                         "the principal has too many tracked sessions"));
            if (!impl_->environment_provider)
                return reject(make_error(ErrorCode::Unavailable,
                                         InteractionDomainCode::NoSessionAvailable,
                                         "no environment provider is bound"));
            Result<std::shared_ptr<IEnvironment>> environment =
                make_error(ErrorCode::Unavailable, InteractionDomainCode::NoSessionAvailable,
                           "the environment provider failed");
            try {
                environment = impl_->environment_provider();
            } catch (...) {
                return reject(environment.error());
            }
            if (!environment || !environment.value())
                return reject(make_error(ErrorCode::Unavailable,
                                         InteractionDomainCode::NoSessionAvailable,
                                         "the environment provider returned no environment"));
            SessionConfig config;
            config.principal = envelope.principal;
            const auto opened = impl_->runtime.open_session(environment.value(), config);
            if (!opened)
                return reject(opened.error());
            const auto applied = wait_applied(opened.value().command);
            if (!applied)
                return reject(applied.error());
            session = opened.value().id;
            std::lock_guard lock(impl_->mutex);
            impl_->registry[impl_->key_of(envelope.principal)].sessions.push_back(*session);
        }
        TaskSpec spec;
        spec.goal = candidate.goal;
        spec.user_constraints = candidate.constraints;
        const auto submitted = impl_->runtime.submit_task(*session, spec);
        if (!submitted)
            return reject(submitted.error());
        const auto applied = wait_applied(submitted.value().command);
        if (!applied)
            return reject(applied.error());
        candidate.task_id = submitted.value().id;
        candidate.session_id = *session;
    }

    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.session_id = candidate.session_id;
    transition.task_id = candidate.task_id;
    transition.intent = decision.intent;
    transition.note = rationale;
    const auto routed = impl_->ledger->advance(entry.input_id, transition);
    if (!routed) {
        if (new_task) {
            const auto cleanup = impl_->runtime.cancel_task(candidate.task_id);
            if (!cleanup || !wait_applied(cleanup.value()))
                return make_error(ErrorCode::DataLoss, InteractionDomainCode::RuntimeCommandFailed,
                                  "route commit failed; task cleanup is uncertain: " +
                                      candidate.task_id.to_string());
        }
        return routed.error(); // Nothing has reached the loop.
    }
    candidate.input_ids.push_back(entry.input_id);
    {
        std::lock_guard lock(impl_->mutex);
        auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
        if (new_task)
            principal.tasks.push_back(candidate);
        else
            *impl_->task_of(principal, candidate.task_id) = candidate;
    }
    disposition.stage = InputStage::Routed;
    disposition.session_id = candidate.session_id;
    disposition.task_id = candidate.task_id;

    auto &queue = impl_->pending[candidate.task_id];
    const bool already_waiting = !queue.empty();
    queue.push_back(envelope);
    const auto sink = impl_->sinks.find(candidate.task_id);
    if (!already_waiting && sink != impl_->sinks.end()) {
        const auto delivered = impl_->flush_pending(candidate.task_id, sink->second);
        if (!delivered) {
            // An explicit refusal accepts nothing; a throw is uncertain and
            // freezes the queue. Both are visible and durably associated.
            if (delivered.error().domain_code !=
                static_cast<std::int32_t>(InteractionDomainCode::DeliveryUncertain)) {
                impl_->pending[candidate.task_id].clear();
                // A refused correction is not an effective user constraint.
                if (!new_task) {
                    std::lock_guard lock(impl_->mutex);
                    auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
                    *impl_->task_of(principal, candidate.task_id) =
                        *find_task(state, candidate.task_id);
                }
            }
            return reject(delivered.error());
        }
    }
    return disposition;
}

} // namespace mira
