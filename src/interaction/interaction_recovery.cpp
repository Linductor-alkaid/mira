#include "interaction_coordinator_support.hpp"
#include <algorithm>

namespace mira {
using interaction_support::recovery_error;
namespace {
Result<void> still_running(const OperationContext &context) {
    if (context.cancelled())
        return recovery_error(ErrorCode::Cancelled, "work recovery was cancelled");
    if (context.expired(Timestamp::now()))
        return recovery_error(ErrorCode::DeadlineExceeded, "work recovery deadline expired");
    return {};
}
} // namespace

Result<WorkRecoveryReport> InteractionCoordinator::recover_work(
    const PrincipalContext &principal, TaskId task, std::shared_ptr<IEnvironment> environment,
    const OperationContext &context, InteractionRecoveryVerifier verifier) {
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire) ||
        !impl_->artifacts || !impl_->checkpoints || !environment)
        return recovery_error(ErrorCode::InvalidState,
                              "work recovery storage or environment is unavailable");
    if (!interaction_support::permitted(principal, impl_->config))
        return recovery_error(ErrorCode::PermissionDenied, "work recovery permission denied",
                              InteractionDomainCode::PermissionDenied);
    if (!context.deadline)
        return recovery_error(ErrorCode::InvalidArgument, "work recovery requires a deadline");
    OperationContext read = context;
    read.operation = OperationId::generate();
    read.task = task;
    read.cancellation_requested = [this, probe = context.cancellation_requested] {
        if (impl_->stopping.load(std::memory_order_acquire))
            return true;
        try {
            return probe && probe();
        } catch (...) {
            return true;
        }
    };
    if (auto active = still_running(read); !active)
        return active.error();
    impl_->recovery_permits.erase(task);
    impl_->refresh_task_states();
    PrincipalTaskEntry work;
    {
        std::lock_guard lock(impl_->mutex);
        const auto state = impl_->snapshot(principal);
        const auto *found = interaction_support::find_task(state, task);
        if (!found || (!found->historical && !is_terminal(found->state)) ||
            found->recovery_availability != RecoveryAvailability::Available)
            return recovery_error(
                ErrorCode::NotFound,
                "historical work context is missing, erased, expired or incompatible");
        work = *found;
    }
    auto source = impl_->source_record(principal, task);
    if (!source)
        return source.error();
    const auto wall_now =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    if (wall_now > source.value().window_expires_at)
        return recovery_error(ErrorCode::InvalidState, "work recovery window expired",
                              InteractionDomainCode::InputExpired);
    if (auto body = impl_->load_input(source.value()); !body)
        return body.error();
    if (auto stored = impl_->load_context(source.value()); !stored)
        return stored.error();
    RecoveryPlanner planner(*impl_->events, *impl_->checkpoints, impl_->config.recovery_policy);
    const auto planned = planner.plan(task, work.session_id, Timestamp::now(), read);
    if (!planned)
        return planned.error();
    if (auto active = still_running(read); !active)
        return active.error();
    if (planned.value().action == RecoveryAction::NoState || !planned.value().checkpoint ||
        planned.value().stats.malformed != 0)
        return recovery_error(ErrorCode::DataLoss, "task event history is missing or incompatible");
    WorkRecoveryReport report;
    report.source_task = task;
    report.recovery = planned.value();
    if (planned.value().checkpoint->terminal_state) {
        std::lock_guard lock(impl_->mutex);
        auto &owner = impl_->registry[impl_->key_of(principal)];
        if (auto *cached = impl_->task_of(owner, task)) {
            cached->state = *planned.value().checkpoint->terminal_state;
            cached->context_block = Impl::assemble_context(*cached);
        }
    }
    const auto records = impl_->ledger->records();
    if (!records)
        return records.error();
    for (const auto &entry : records.value()) {
        if (entry.task_id != task || entry.tenant_id != principal.tenant_id ||
            entry.user_id != principal.user_id)
            continue;
        report.input_delivery_uncertain |=
            entry.delivery == InputDeliveryState::Dispatching ||
            entry.delivery == InputDeliveryState::Uncertain ||
            (entry.delivery == InputDeliveryState::Delivered && entry.stage == InputStage::Routed);
    }
    read.session = work.session_id;
    read.task_epoch = planned.value().task_epoch;
    ObservationRequest request;
    request.mode = ObservationMode::Verification;
    request.max_age = std::chrono::milliseconds(0);
    Result<Observation> observed =
        recovery_error(ErrorCode::Unavailable, "recovery observation failed");
    try {
        observed = environment->observe(request, read);
    } catch (...) {
        return recovery_error(ErrorCode::Unavailable, "recovery observation failed");
    }
    if (auto active = still_running(read); !active)
        return active.error();
    if (!observed || observed.value().id.is_nil())
        return recovery_error(ErrorCode::Unavailable, "recovery observation was not available");
    auto current = planner.plan(task, work.session_id, Timestamp::now(), read);
    if (!current)
        return current.error();
    if (current.value().durable_sequence != planned.value().durable_sequence)
        return recovery_error(ErrorCode::InvalidState,
                              "task events changed during recovery observation",
                              InteractionDomainCode::RecoveryStale);
    report.observation = observed.value();
    JsonValue::Object metadata{{"observation_id", observed.value().id.to_string()},
                               {"environment_epoch", observed.value().environment_epoch}};
    if (observed.value().screen) {
        metadata.emplace_back("artifact_id",
                              observed.value().screen->value.payload_artifact.to_string());
        metadata.emplace_back("artifact_digest",
                              observed.value().screen->value.payload_digest.to_string());
    }
    const auto digest = digest_string(to_json_string(JsonValue(metadata)));
    metadata.emplace_back("digest", digest.to_string());
    metadata.emplace_back("schema", "mira.interaction.recovery-observation.v1");
    metadata.emplace_back("expected_through_event_sequence", planned.value().durable_sequence);
    auto watermark = impl_->append_task_event(task, work.session_id, "RecoveryObservationRecorded",
                                              JsonValue(metadata));
    if (!watermark)
        return watermark.error();
    current = planner.plan(task, work.session_id, Timestamp::now(), read);
    if (!current)
        return current.error();
    if (!current.value().checkpoint || !current.value().checkpoint->current_observation ||
        current.value().checkpoint->current_observation->sequence != watermark.value() ||
        current.value().durable_sequence != watermark.value())
        return recovery_error(ErrorCode::InvalidState, "recovery observation commit was overtaken",
                              InteractionDomainCode::RecoveryStale);
    const bool needs_verification = report.input_delivery_uncertain ||
                                    report.recovery.checkpoint->verification_pending ||
                                    !report.recovery.pending_side_effects.empty();
    if (needs_verification) {
        if (!verifier)
            return report; // Observation alone does not resolve any uncertainty.
        Result<bool> verified = recovery_error(ErrorCode::Unavailable, "recovery verifier failed");
        try {
            verified = verifier(report, *report.observation, read);
        } catch (...) {
            return recovery_error(ErrorCode::Unavailable, "recovery verifier failed");
        }
        if (auto active = still_running(read); !active)
            return active.error();
        if (!verified)
            return verified.error();
        if (!verified.value())
            return report;
        current = planner.plan(task, work.session_id, Timestamp::now(), read);
        if (!current)
            return current.error();
        if (current.value().durable_sequence != watermark.value())
            return recovery_error(ErrorCode::InvalidState,
                                  "task events changed during recovery verification",
                                  InteractionDomainCode::RecoveryStale);
        JsonValue::Array resolved;
        for (const auto &effect : report.recovery.pending_side_effects)
            resolved.emplace_back(effect.action_id.to_string());
        watermark = impl_->append_task_event(
            task, work.session_id, "RecoveryVerificationResult",
            JsonValue::Object{{"schema", "mira.interaction.recovery-verification.v1"},
                              {"expected_through_event_sequence", watermark.value()},
                              {"verdict", "satisfied"},
                              {"observation_id", observed.value().id.to_string()},
                              {"observation_digest", digest.to_string()},
                              {"resolved_actions", std::move(resolved)},
                              {"source", "mira.interaction.recovery.v1"}});
        if (!watermark)
            return watermark.error();
    }
    if (auto active = still_running(read); !active)
        return active.error();
    current = planner.plan(task, work.session_id, Timestamp::now(), read);
    const auto latest = impl_->source_record(principal, task);
    if (!current)
        return current.error();
    if (current.value().durable_sequence != watermark.value() || !latest ||
        latest.value().updated_sequence != source.value().updated_sequence)
        return recovery_error(ErrorCode::InvalidState, "work recovery source changed",
                              InteractionDomainCode::RecoveryStale);
    if (needs_verification &&
        (!current.value().checkpoint ||
         current.value().checkpoint->recovery_verification_sequence != watermark.value()))
        return recovery_error(ErrorCode::InvalidState, "recovery verification commit was overtaken",
                              InteractionDomainCode::RecoveryStale);
    if (!current.value().checkpoint || current.value().checkpoint->verification_pending ||
        !current.value().pending_side_effects.empty())
        return report;
    if (auto saved = impl_->checkpoints->put(*current.value().checkpoint); !saved)
        return saved.error();
    if (auto active = still_running(read); !active)
        return active.error();
    if (!impl_->recovery_permits.contains(task) &&
        impl_->recovery_permits.size() >= impl_->config.max_ledger_entries)
        return recovery_error(ErrorCode::ResourceExhausted, "recovery permit capacity exhausted");
    Impl::RecoveryPermit permit;
    permit.principal = impl_->key_of(principal);
    permit.context_digest = source.value().context_ref->digest;
    permit.ledger_revision = source.value().updated_sequence;
    permit.event_watermark = current.value().durable_sequence;
    permit.expires = std::chrono::steady_clock::now() + impl_->config.recovery_permit_ttl;
    permit.environment = std::move(environment);
    permit.checkpoint = *current.value().checkpoint;
    const auto wall =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    std::erase_if(permit.checkpoint.verified_facts,
                  [&](const auto &fact) { return !fact.valid_at(wall); });
    impl_->recovery_permits[task] = std::move(permit);
    report.recovery = current.value();
    report.ready_for_new_task = true;
    return report;
}

Result<std::shared_ptr<IEnvironment>>
InteractionCoordinator::Impl::recovery_environment(const PrincipalContext &principal, TaskId task) {
    const auto found = recovery_permits.find(task);
    if (found == recovery_permits.end())
        return recovery_error(ErrorCode::InvalidState, "historical work needs fresh Observe/Verify",
                              InteractionDomainCode::RecoveryRequired);
    const auto &permit = found->second;
    const auto source = source_record(principal, task);
    const auto now =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    if ((source && now > source.value().window_expires_at) ||
        permit.principal != key_of(principal) ||
        std::chrono::steady_clock::now() >= permit.expires || !source ||
        !source.value().context_ref ||
        source.value().context_ref->digest != permit.context_digest ||
        source.value().updated_sequence != permit.ledger_revision)
        return recovery_error(ErrorCode::InvalidState, "the work recovery permit is stale",
                              InteractionDomainCode::RecoveryStale);
    if (auto stored = load_context(source.value()); !stored)
        return stored.error();
    if (auto body = load_input(source.value()); !body)
        return body.error();
    RecoveryPlanner planner(*events, *checkpoints, config.recovery_policy);
    auto context = make_control_context();
    context.deadline = std::chrono::steady_clock::now() + config.command_timeout;
    context.cancellation_requested = [this] { return stopping.load(std::memory_order_acquire); };
    const auto planned = planner.plan(task, *source.value().session_id, Timestamp::now(), context);
    if (!planned)
        return planned.error();
    if (planned.value().durable_sequence != permit.event_watermark ||
        std::chrono::steady_clock::now() >= permit.expires)
        return recovery_error(ErrorCode::InvalidState, "task history changed after recovery",
                              InteractionDomainCode::RecoveryStale);
    return permit.environment;
}
} // namespace mira
