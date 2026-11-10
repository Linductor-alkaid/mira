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
    // Historical projections are restored separately; never recreate live Tasks.
    if (auto *ledger = dynamic_cast<EventStoreInputLedger *>(impl_->ledger.get());
        ledger != nullptr) {
        const auto rebuilt = ledger->rebuild();
        if (!rebuilt) {
            return rebuilt.error();
        }
    }
    if (static_cast<bool>(impl_->artifacts) != static_cast<bool>(impl_->checkpoints) ||
        (impl_->artifacts && (!impl_->events || impl_->config.max_recovery_artifact_bytes == 0)))
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::RecoveryUnavailable,
                          "recovery requires event, artifact and checkpoint stores");
    if (impl_->artifacts) {
        const auto restored = impl_->restore_registry();
        if (!restored)
            return restored.error();
    }
    if (impl_->memory) {
        if (auto initialized = impl_->memory->initialize(); !initialized)
            return initialized.error();
    }
    impl_->initialized = true;
    return Result<void>{};
}

Result<InputDisposition> InteractionCoordinator::submit_input(const InputEnvelope &envelope) {
    return submit_impl(envelope, false);
}

Result<InputDisposition> InteractionCoordinator::submit_impl(InputEnvelope envelope, bool resume) {
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
    const auto admitted = impl_->admit_input(envelope);
    if (!admitted) {
        disposition.stage = InputStage::Rejected;
        disposition.error =
            permitted(envelope.principal, impl_->config)
                ? admitted.error()
                : make_error(ErrorCode::PermissionDenied, InteractionDomainCode::PermissionDenied,
                             "the principal lacks the required capability");
        return disposition;
    }
    auto entry = admitted.value();
    const auto response_entry = entry;
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
    if (entry.duplicate && entry.clarification_for) {
        const auto original = impl_->ledger->lookup(*entry.clarification_for);
        if (original && original.value().resolved_by == entry.input_id) {
            const auto repaired = impl_->finish_clarification(entry, original.value());
            if (!repaired)
                return repaired.error();
            entry = repaired.value();
            entry.duplicate = true;
        }
    }
    if (entry.duplicate && resume && entry.stage == InputStage::Routed &&
        entry.delivery == InputDeliveryState::NotDispatched) {
        std::optional<PrincipalTaskEntry> task;
        {
            std::lock_guard lock(impl_->mutex);
            const auto state = impl_->snapshot(envelope.principal);
            if (entry.task_id) {
                const auto *found = find_task(state, *entry.task_id);
                if (found)
                    task = *found;
            }
        }
        if (!task || task->historical)
            return make_error(ErrorCode::InvalidState, InteractionDomainCode::RecoveryRequired,
                              "historical work requires recovery and a new task");
        if (entry.resolved_by) {
            const auto answer = impl_->ledger->lookup(*entry.resolved_by);
            if (!answer)
                return answer.error();
            const auto body = impl_->load_input(answer.value());
            if (!body)
                return body.error();
            envelope.content += "\nClarification: " + body.value().content;
        }
        auto &queue = impl_->pending[task->task_id];
        const auto found = std::find_if(queue.begin(), queue.end(), [&](const auto &item) {
            return item.input_id == entry.input_id;
        });
        if (found == queue.end()) {
            if (queue.size() >= impl_->config.max_pending_deliveries)
                return make_error(ErrorCode::ResourceExhausted,
                                  InteractionDomainCode::SinkUnavailable,
                                  "the task input queue is full");
            queue.push_back(envelope);
        }
        const auto sink = impl_->sinks.find(task->task_id);
        if (sink != impl_->sinks.end()) {
            const auto flushed = impl_->flush_pending(task->task_id, sink->second);
            if (!flushed)
                return flushed.error();
        }
    }
    if (entry.duplicate &&
        !(resume && entry.stage == InputStage::Accepted && !entry.clarification)) {
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
        if (entry.input_id != response_entry.input_id) {
            const auto original = impl_->ledger->lookup(entry.input_id);
            if (original && original.value().stage == InputStage::Routed) {
                const auto rejected_original = impl_->ledger->advance(entry.input_id, transition);
                if (!rejected_original)
                    return rejected_original.error();
            }
        }
        const auto rejected = impl_->ledger->advance(response_entry.input_id, transition);
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

    std::optional<LedgerEntry> clarification_answer;
    if (envelope.clarification_for) {
        const auto original = impl_->ledger->lookup(*envelope.clarification_for);
        const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        if (!original || original.value().tenant_id != envelope.principal.tenant_id ||
            original.value().user_id != envelope.principal.user_id ||
            original.value().stage != InputStage::Accepted || !original.value().clarification ||
            original.value().resolved_by || !envelope.target_task || envelope.reply_to ||
            now > original.value().window_expires_at ||
            std::find(original.value().candidates.begin(), original.value().candidates.end(),
                      *envelope.target_task) == original.value().candidates.end())
            return reject(
                make_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputConflict,
                           "the clarification reference or selected candidate is invalid"));
        const auto body = impl_->load_input(original.value());
        if (!body)
            return reject(body.error());
        auto effective = body.value();
        effective.principal = envelope.principal;
        effective.target_task = envelope.target_task;
        effective.requested_intent =
            envelope.requested_intent ? envelope.requested_intent : effective.requested_intent;
        effective.content += "\nClarification: " + envelope.content;
        effective.reply_to.reset();
        effective.clarification_for.reset();
        if (effective.content.size() > impl_->config.max_recovery_artifact_bytes)
            return reject(make_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::InputInvalid,
                                     "combined clarification input exceeds its bound"));
        clarification_answer = entry;
        entry = original.value();
        envelope = std::move(effective);
    }
    impl_->refresh_task_states();
    PrincipalState state;
    {
        std::lock_guard lock(impl_->mutex);
        state = impl_->snapshot(envelope.principal);
    }
    if (envelope.reply_to && !interaction_support::task_of_input(state, *envelope.reply_to)) {
        // Controls retain their durable association even if the hot list is full.
        const auto referenced = impl_->ledger->lookup(*envelope.reply_to);
        if (referenced && referenced.value().tenant_id == envelope.principal.tenant_id &&
            referenced.value().user_id == envelope.principal.user_id &&
            referenced.value().task_id && referenced.value().stage != InputStage::Accepted &&
            referenced.value().stage != InputStage::Rejected) {
            for (auto &task : state.tasks)
                if (task.task_id == *referenced.value().task_id)
                    task.input_ids.push_back(*envelope.reply_to);
        }
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
        if (!impl_->artifacts) {
            if (!impl_->clarification_inputs.contains(entry.input_id) &&
                impl_->clarification_inputs.size() >= impl_->config.max_ledger_entries)
                return reject(make_error(ErrorCode::ResourceExhausted,
                                         InteractionDomainCode::LedgerFull,
                                         "pending clarification capacity exhausted"));
            impl_->clarification_inputs[entry.input_id] = envelope;
        }
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
        const auto *target = find_task(state, *decision.task_id);
        if (!target || target->historical)
            return reject(make_error(ErrorCode::InvalidState,
                                     InteractionDomainCode::RecoveryRequired,
                                     "historical tasks have no active Runtime control target"));
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
        if (impl_->artifacts) {
            const auto snapshot = impl_->runtime.task_snapshot(*decision.task_id);
            if (!snapshot)
                return reject(snapshot.error());
            const auto epoch = impl_->append_task_event(
                *decision.task_id, *decision.session_id, "TaskEpochAdvanced",
                JsonValue::Object{{"epoch", snapshot.value().epoch}});
            if (!epoch)
                return epoch.error();
            const auto state_event = impl_->append_task_event(
                *decision.task_id, *decision.session_id, "TaskStateChanged",
                JsonValue::Object{{"to", std::string(task_state_name(snapshot.value().state))}});
            if (!state_event)
                return state_event.error();
        }
        LedgerTransition transition;
        transition.stage = InputStage::Routed;
        transition.session_id = decision.session_id;
        transition.task_id = decision.task_id;
        transition.intent = decision.intent;
        transition.note = rationale;
        transition.delivery = InputDeliveryState::NotRequired;
        if (clarification_answer)
            transition.resolved_by = clarification_answer->input_id;
        const auto routed = impl_->ledger->advance(entry.input_id, transition);
        if (!routed)
            return routed.error();
        {
            std::lock_guard lock(impl_->mutex);
            auto &owner = impl_->registry[impl_->key_of(envelope.principal)];
            if (auto *task = impl_->task_of(owner, *decision.task_id)) {
                if (task->input_ids.size() < impl_->config.max_inputs_per_task)
                    task->input_ids.push_back(entry.input_id);
                if (clarification_answer &&
                    task->input_ids.size() < impl_->config.max_inputs_per_task)
                    task->input_ids.push_back(clarification_answer->input_id);
            }
        }
        if (clarification_answer) {
            const auto completed =
                impl_->finish_clarification(*clarification_answer, routed.value());
            if (!completed)
                return completed.error();
            impl_->clarification_inputs.erase(entry.input_id);
        }
        disposition.stage = InputStage::Routed;
        disposition.session_id = decision.session_id;
        disposition.task_id = decision.task_id;
        return disposition;
    }

    const bool new_task =
        decision.intent == RouteIntent::NewTask || decision.intent == RouteIntent::ResumeTask;
    PrincipalTaskEntry candidate;
    std::shared_ptr<IEnvironment> recovered_environment;
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
            candidate.source_artifacts = previous->source_artifacts;
            candidate.resumed_from = previous->task_id;
            if (previous->historical || impl_->artifacts) {
                if (previous->recovery_availability != RecoveryAvailability::Available)
                    return reject(make_error(ErrorCode::NotFound,
                                             InteractionDomainCode::RecoveryUnavailable,
                                             "historical work context is unavailable"));
                const auto recovered =
                    impl_->recovery_environment(envelope.principal, previous->task_id);
                if (!recovered)
                    return reject(recovered.error());
                recovered_environment = recovered.value();
                candidate.recovered_checkpoint =
                    impl_->recovery_permits.at(previous->task_id).checkpoint;
            }
            // Resumption must carry the new requirement as well as old context.
            Impl::append_constraint(candidate, bound_text(envelope.content, kMaxConstraintBytes));
        }
    } else {
        candidate = *find_task(state, *decision.task_id);
        if (candidate.input_ids.size() + (clarification_answer ? 2U : 1U) >
            impl_->config.max_inputs_per_task)
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
    if (impl_->config.max_pending_deliveries == 0 ||
        impl_->config.max_inputs_per_task < (clarification_answer ? 2U : 1U))
        return reject(make_error(ErrorCode::ResourceExhausted,
                                 InteractionDomainCode::SinkUnavailable,
                                 "task input capacity is zero"));
    if (impl_->artifacts) {
        if (entry.input_ref)
            candidate.source_artifacts.push_back(*entry.input_ref);
        if (clarification_answer && clarification_answer->input_ref)
            candidate.source_artifacts.push_back(*clarification_answer->input_ref);
        if (candidate.source_artifacts.size() > impl_->config.max_inputs_per_task)
            return reject(make_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::InputInvalid,
                                     "work provenance exceeds its bound"));
    }
    const auto prepared = impl_->prepare_context(candidate);
    if (!prepared)
        return reject(prepared.error());
    candidate.context_block = prepared.value();

    if (new_task) {
        auto session = recovered_environment ? std::optional<SessionId>{} : decision.session_id;
        if (!session) {
            if (state.sessions.size() >= impl_->config.max_sessions_per_principal)
                return reject(make_error(ErrorCode::ResourceExhausted,
                                         InteractionDomainCode::RouteRejected,
                                         "the principal has too many tracked sessions"));
            if (!recovered_environment && !impl_->environment_provider)
                return reject(make_error(ErrorCode::Unavailable,
                                         InteractionDomainCode::NoSessionAvailable,
                                         "no environment provider is bound"));
            Result<std::shared_ptr<IEnvironment>> environment =
                make_error(ErrorCode::Unavailable, InteractionDomainCode::NoSessionAvailable,
                           "the environment provider failed");
            try {
                environment = recovered_environment
                                  ? Result<std::shared_ptr<IEnvironment>>{recovered_environment}
                                  : impl_->environment_provider();
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

    std::optional<ArtifactDescriptor> context_ref;
    if (impl_->artifacts) {
        Result<SessionSequence> anchor{SessionSequence{0}};
        if (new_task)
            anchor = impl_->append_task_event(candidate.task_id, candidate.session_id,
                                              "TaskEpochAdvanced",
                                              JsonValue::Object{{"epoch", std::int64_t{0}}});
        auto stored = anchor ? impl_->store_context(candidate, envelope.principal)
                             : Result<ArtifactDescriptor>{anchor.error()};
        if (!stored) {
            if (new_task) {
                const auto cleanup = impl_->runtime.cancel_task(candidate.task_id);
                if (!cleanup || !wait_applied(cleanup.value()))
                    return make_error(ErrorCode::DataLoss,
                                      InteractionDomainCode::RuntimeCommandFailed,
                                      "recovery persistence failed; new task cleanup is uncertain");
            }
            return reject(stored.error());
        }
        context_ref = stored.value();
    }
    LedgerTransition transition;
    transition.stage = InputStage::Routed;
    transition.session_id = candidate.session_id;
    transition.task_id = candidate.task_id;
    transition.intent = decision.intent;
    transition.note = rationale;
    transition.context_ref = context_ref;
    if (clarification_answer)
        transition.resolved_by = clarification_answer->input_id;
    if (recovered_environment && candidate.resumed_from) {
        const auto current =
            impl_->recovery_environment(envelope.principal, *candidate.resumed_from);
        if (!current) {
            const auto cleanup = impl_->runtime.cancel_task(candidate.task_id);
            if (!cleanup || !wait_applied(cleanup.value()))
                return make_error(ErrorCode::DataLoss, InteractionDomainCode::RuntimeCommandFailed,
                                  "recovery became stale; task cleanup is uncertain");
            return reject(current.error());
        }
    }
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
    if (clarification_answer)
        candidate.input_ids.push_back(clarification_answer->input_id);
    {
        std::lock_guard lock(impl_->mutex);
        auto &principal = impl_->registry[impl_->key_of(envelope.principal)];
        if (new_task)
            principal.tasks.push_back(candidate);
        else
            *impl_->task_of(principal, candidate.task_id) = candidate;
    }
    if (recovered_environment && candidate.resumed_from)
        impl_->recovery_permits.erase(*candidate.resumed_from);
    if (clarification_answer) {
        const auto completed = impl_->finish_clarification(*clarification_answer, routed.value());
        if (!completed)
            return completed.error();
        impl_->clarification_inputs.erase(entry.input_id);
    }
    disposition.stage = InputStage::Routed;
    disposition.session_id = candidate.session_id;
    disposition.task_id = candidate.task_id;

    impl_->stage_memory(envelope, candidate);

    auto &queue = impl_->pending[candidate.task_id];
    const bool already_waiting = !queue.empty();
    if (new_task && candidate.resumed_from && !candidate.constraints.empty()) {
        std::string constraints = "[Previously committed user constraints]";
        for (const auto &constraint : candidate.constraints)
            constraints += "\n- " + constraint;
        envelope.content = constraints + "\n[Current user input]\n" + envelope.content;
    }
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
