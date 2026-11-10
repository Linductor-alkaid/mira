#include "interaction_coordinator_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/conversation_loop.hpp>

namespace mira {
namespace {
using interaction_support::bound_text;
using interaction_support::kMaxConstraintBytes;
[[nodiscard]] Error make_error(ErrorCode code, InteractionDomainCode domain_code,
                               std::string message, bool retryable = false) {
    return interaction_error(code, domain_code, std::move(message), retryable);
}
} // namespace

Result<CommandHandle> InteractionCoordinator::cancel_work(TaskId task_id) {
    auto cancelled = impl_->runtime.cancel_task(task_id);
    if (cancelled) {
        std::lock_guard submit_lock(impl_->submit_mutex);
        impl_->revoke_memory(task_id);
    }
    return cancelled;
}

Result<void>
InteractionCoordinator::Impl::flush_pending(TaskId task,
                                            const std::shared_ptr<IUserInputSink> &sink) {
    if (const auto frozen = uncertain.find(task); frozen != uncertain.end())
        return frozen->second;
    const auto waiting = pending.find(task);
    if (waiting == pending.end())
        return Result<void>{};
    auto &queue = waiting->second;
    while (!queue.empty()) {
        const auto record = ledger->lookup(queue.front().input_id);
        if (!record)
            return record.error();
        const bool tracked = static_cast<bool>(artifacts);
        const auto uncertain_delivery = [&]() -> Result<void> {
            auto error =
                make_error(ErrorCode::Unavailable, InteractionDomainCode::DeliveryUncertain,
                           "input delivery is uncertain; host reconciliation is required");
            uncertain[task] = error;
            LedgerTransition transition;
            transition.stage = InputStage::Routed;
            transition.task_id = task;
            transition.note = "delivery uncertain; host reconciliation is required";
            transition.error = error;
            if (tracked)
                transition.delivery = InputDeliveryState::Uncertain;
            if (!ledger->advance(queue.front().input_id, transition))
                return make_error(ErrorCode::DataLoss, InteractionDomainCode::DeliveryUncertain,
                                  "delivery is uncertain and its durable diagnostic failed");
            return error;
        };
        if (tracked) {
            if (record.value().delivery == InputDeliveryState::Delivered ||
                record.value().stage == InputStage::Answered ||
                record.value().stage == InputStage::TaskSettled) {
                queue.erase(queue.begin());
                continue;
            }
            if (record.value().delivery != InputDeliveryState::NotDispatched)
                return uncertain_delivery();
            LedgerTransition starting;
            starting.stage = InputStage::Routed;
            starting.delivery = InputDeliveryState::Dispatching;
            const auto saved = ledger->advance(record.value().input_id, starting);
            if (!saved)
                return saved.error();
        }
        try {
            const auto delivered = sink->deliver(queue.front(), task);
            if (!delivered) {
                if (tracked) {
                    LedgerTransition refused;
                    refused.stage = InputStage::Routed;
                    refused.delivery = InputDeliveryState::NotDispatched;
                    if (!ledger->advance(record.value().input_id, refused))
                        return uncertain_delivery();
                }
                return make_error(ErrorCode::Unavailable, InteractionDomainCode::SinkUnavailable,
                                  "the input sink refused delivery", true);
            }
        } catch (...) {
            return uncertain_delivery();
        }
        if (tracked) {
            LedgerTransition delivered;
            delivered.stage = InputStage::Routed;
            delivered.delivery = InputDeliveryState::Delivered;
            const auto saved = ledger->advance(record.value().input_id, delivered);
            if (!saved)
                return uncertain_delivery();
        }
        queue.erase(queue.begin());
    }
    pending.erase(waiting);
    activate_memory(task);
    return Result<void>{};
}

Result<void> InteractionCoordinator::register_sink(TaskId task_id,
                                                   std::shared_ptr<IUserInputSink> sink) {
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire))
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator is not accepting sinks");
    if (!sink)
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::SinkUnavailable,
                          "the input sink must not be null");
    if (impl_->sinks.size() >= impl_->config.max_sinks && !impl_->sinks.contains(task_id))
        return make_error(ErrorCode::ResourceExhausted, InteractionDomainCode::SinkUnavailable,
                          "too many input sinks are registered");
    // The host may bind only a registered, live work item. A late registration
    // must not deliver queued input into a settled task.
    bool known = false;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto &principal : impl_->registry)
            for (const auto &task : principal.second.tasks)
                known = known || (task.task_id == task_id && !task.historical);
    }
    const auto snapshot = impl_->runtime.task_snapshot(task_id);
    if (!known || !snapshot || is_terminal(snapshot.value().state))
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::SinkUnavailable,
                          "the sink target is not a registered live task");
    impl_->sinks[task_id] = sink;
    return impl_->flush_pending(task_id, sink);
}

Result<void> InteractionCoordinator::unregister_sink(TaskId task_id) {
    std::lock_guard submit_lock(impl_->submit_mutex);
    impl_->sinks.erase(task_id);
    return Result<void>{};
}

Result<InputDisposition> InteractionCoordinator::record_answer(std::string_view input_id,
                                                               std::string summary) {
    std::lock_guard submit_lock(impl_->submit_mutex);
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
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (!impl_->initialized) {
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction coordinator was not initialized");
    }
    if (!is_terminal(terminal)) {
        return make_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                          "a settlement needs a terminal task state");
    }
    const auto current = impl_->ledger->lookup(input_id);
    if (!current)
        return current.error();
    if (current.value().stage != InputStage::TaskSettled && current.value().task_id) {
        const auto snapshot = impl_->runtime.task_snapshot(*current.value().task_id);
        if (snapshot && is_terminal(snapshot.value().state) && snapshot.value().state != terminal)
            return make_error(ErrorCode::InvalidState, InteractionDomainCode::RuntimeCommandFailed,
                              "the settlement conflicts with the terminal Runtime state");
    }
    if (impl_->artifacts) {
        const auto entry = impl_->ledger->lookup(input_id);
        if (!entry)
            return entry.error();
        if (entry.value().task_id && entry.value().session_id &&
            entry.value().stage != InputStage::TaskSettled &&
            entry.value().stage != InputStage::Rejected) {
            const auto recorded = impl_->append_task_event(
                *entry.value().task_id, *entry.value().session_id, "TaskStateChanged",
                JsonValue::Object{{"to", std::string(task_state_name(terminal))}});
            if (!recorded)
                return recorded.error();
        }
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
    if (entry.task_id && current.value().stage != InputStage::TaskSettled)
        impl_->settle_memory(*entry.task_id);
    return disposition;
}

Result<InputDisposition> InteractionCoordinator::record_rejection(std::string_view input_id,
                                                                  std::string reason) {
    std::lock_guard submit_lock(impl_->submit_mutex);
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
    if (!impl_->ledger)
        return make_error(ErrorCode::InvalidState, InteractionDomainCode::NotInitialized,
                          "the interaction ledger is not initialized");
    return impl_->ledger->lookup(input_id);
}

void InteractionCoordinator::shutdown() { (void)close(); }

Result<void> InteractionCoordinator::close() {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->memory_alive->store(false, std::memory_order_release);
    std::lock_guard submit_lock(impl_->submit_mutex);
    for (const auto &[task, work] : impl_->memory_works) {
        (void)work;
        impl_->revoke_memory(task);
    }
    impl_->sinks.clear();
    impl_->pending.clear();
    impl_->clarification_inputs.clear();
    impl_->recovery_permits.clear();
    // Each retired job is finite pure computation. Keep the Executor alive
    // until these futures settle; none can call back into this object.
    for (auto &future : impl_->retired_futures)
        impl_->consume(future);
    impl_->retired_futures.clear();
    impl_->initialized = false;
    if (impl_->late_failure)
        return *impl_->late_failure;
    return Result<void>{};
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
