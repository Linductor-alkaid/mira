#include "interaction_coordinator_support.hpp"

#include <algorithm>

namespace mira {
using interaction_support::recovery_error;
namespace {
WallTimePoint wall_now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}
bool owned(const LedgerEntry &entry, const PrincipalContext &principal) {
    return entry.tenant_id == principal.tenant_id && entry.user_id == principal.user_id;
}
bool effective(const LedgerEntry &entry) {
    return entry.stage != InputStage::Rejected ||
           (entry.error && entry.error->domain_code ==
                               static_cast<std::int32_t>(InteractionDomainCode::DeliveryUncertain));
}
Result<LedgerEntry> select_source(const std::vector<LedgerEntry> &all,
                                  const PrincipalContext &principal, TaskId task) {
    std::optional<LedgerEntry> source;
    SessionSequence revision = 0;
    for (const auto &entry : all) {
        if (!owned(entry, principal) || entry.task_id != task || !effective(entry))
            continue;
        revision = std::max(revision, entry.updated_sequence);
        if (!source || (entry.context_ref && (!source->context_ref ||
                                              entry.updated_sequence > source->updated_sequence)))
            source = entry;
    }
    if (!source)
        return recovery_error(ErrorCode::NotFound, "the work item is not owned by this principal");
    source->updated_sequence = revision;
    return *source;
}
} // namespace

Result<LedgerEntry> InteractionCoordinator::Impl::source_record(const PrincipalContext &principal,
                                                                TaskId task) const {
    const auto all = ledger->records();
    if (!all)
        return all.error();
    return select_source(all.value(), principal, task);
}

Result<void> InteractionCoordinator::Impl::restore_registry() {
    const auto all = ledger->records();
    if (!all)
        return all.error();
    if (all.value().size() > config.max_ledger_entries)
        return recovery_error(ErrorCode::ResourceExhausted, "recovery index exceeds its bound");
    std::unordered_map<PrincipalKey, PrincipalEntry, PrincipalKeyHash> restored;
    for (const auto &entry : all.value()) {
        if (!entry.task_id || !entry.session_id || !effective(entry) || entry.tenant_id.is_nil() ||
            entry.user_id.is_nil())
            continue;
        const PrincipalKey owner{entry.tenant_id, entry.user_id};
        auto &principal = restored[owner];
        auto *existing = task_of(principal, *entry.task_id);
        if (existing) {
            if (existing->input_ids.size() >= config.max_inputs_per_task)
                return recovery_error(ErrorCode::ResourceExhausted,
                                      "recovered work input list exceeds its bound");
            existing->input_ids.push_back(entry.input_id);
            continue;
        }
        if (principal.tasks.size() >= config.max_tasks_per_principal)
            return recovery_error(ErrorCode::ResourceExhausted,
                                  "recovered task registry exceeds its bound");
        PrincipalContext identity;
        identity.tenant_id = owner.tenant_id;
        identity.user_id = owner.user_id;
        const auto source = select_source(all.value(), identity, *entry.task_id);
        if (!source)
            return source.error();
        PrincipalTaskEntry task;
        task.task_id = *entry.task_id;
        task.session_id = *entry.session_id;
        task.historical = true;
        if (wall_now() > source.value().window_expires_at) {
            task.recovery_availability = RecoveryAvailability::Expired;
        } else {
            const auto loaded = load_context(source.value());
            if (loaded)
                task = loaded.value();
            else if (loaded.error().code == ErrorCode::NotFound)
                task.recovery_availability = RecoveryAvailability::MissingOrErased;
            else if (loaded.error().code == ErrorCode::DataLoss ||
                     loaded.error().code == ErrorCode::InvalidArgument ||
                     loaded.error().code == ErrorCode::ResourceExhausted)
                task.recovery_availability = RecoveryAvailability::Incompatible;
            else
                return loaded.error();
        }
        // The settlement projection is durable even when the old Runtime is gone.
        for (const auto &record : all.value()) {
            if (record.task_id != task.task_id || record.stage != InputStage::TaskSettled ||
                !owned(record, identity))
                continue;
            constexpr std::string_view prefix = "terminal: ";
            if (record.note.starts_with(prefix))
                task.state =
                    task_state_from_name(std::string_view(record.note).substr(prefix.size()))
                        .value_or(task.state);
        }
        task.input_ids = {entry.input_id};
        task.context_block = task.recovery_availability == RecoveryAvailability::Available
                                 ? assemble_context(task)
                                 : "work context unavailable";
        if (std::find(principal.sessions.begin(), principal.sessions.end(), task.session_id) ==
            principal.sessions.end()) {
            if (principal.sessions.size() >= config.max_sessions_per_principal)
                return recovery_error(ErrorCode::ResourceExhausted,
                                      "recovered session registry exceeds its bound");
            principal.sessions.push_back(task.session_id);
        }
        principal.tasks.push_back(std::move(task));
    }
    std::lock_guard lock(mutex);
    registry = std::move(restored);
    return {};
}

Result<InputReplayPage> InteractionCoordinator::replay_inputs(const PrincipalContext &principal,
                                                              SessionSequence after,
                                                              std::size_t limit) const {
    std::lock_guard submit_lock(impl_->submit_mutex);
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire))
        return recovery_error(ErrorCode::InvalidState,
                              "the coordinator is not accepting replay queries");
    if (!interaction_support::permitted(principal, impl_->config))
        return recovery_error(ErrorCode::PermissionDenied, "replay permission denied",
                              InteractionDomainCode::PermissionDenied);
    if (limit == 0 || limit > impl_->config.max_replay_page)
        return recovery_error(ErrorCode::InvalidArgument, "replay page limit is outside its bound");
    const auto all = impl_->ledger->records();
    if (!all)
        return all.error();
    InputReplayPage page;
    page.next_sequence = after;
    page.window_start = wall_now() - impl_->config.idempotency_window;
    for (const auto &entry : all.value()) {
        if (!owned(entry, principal) || entry.admitted_sequence <= after)
            continue;
        if (wall_now() > entry.window_expires_at) {
            page.expired_gap = true;
            continue;
        }
        if (page.entries.size() == limit) {
            page.has_more = true;
            break;
        }
        page.entries.push_back(entry);
        page.next_sequence = entry.admitted_sequence;
    }
    return page;
}

Result<InputDisposition> InteractionCoordinator::resume_input(const PrincipalContext &principal,
                                                              std::string_view input_id) {
    InputEnvelope envelope;
    {
        std::lock_guard submit_lock(impl_->submit_mutex);
        if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire))
            return recovery_error(ErrorCode::InvalidState,
                                  "the coordinator is not accepting input recovery");
        if (!interaction_support::permitted(principal, impl_->config))
            return recovery_error(ErrorCode::PermissionDenied, "input recovery permission denied",
                                  InteractionDomainCode::PermissionDenied);
        const auto entry = impl_->ledger->lookup(input_id);
        if (!entry || !owned(entry.value(), principal))
            return recovery_error(ErrorCode::NotFound, "the recoverable input was not found");
        if (wall_now() > entry.value().window_expires_at)
            return recovery_error(ErrorCode::InvalidState, "the input recovery window expired",
                                  InteractionDomainCode::InputExpired);
        if (entry.value().stage == InputStage::Routed) {
            if (entry.value().delivery != InputDeliveryState::NotDispatched)
                return recovery_error(ErrorCode::InvalidState,
                                      "a possibly delivered input cannot be retried",
                                      InteractionDomainCode::DeliveryUncertain);
            std::lock_guard lock(impl_->mutex);
            const auto state = impl_->snapshot(principal);
            const auto *task = entry.value().task_id
                                   ? interaction_support::find_task(state, *entry.value().task_id)
                                   : nullptr;
            if (!task || task->historical)
                return recovery_error(ErrorCode::InvalidState,
                                      "historical work requires Observe/Verify and a new task",
                                      InteractionDomainCode::RecoveryRequired);
        } else if (entry.value().stage != InputStage::Accepted) {
            return recovery_error(ErrorCode::InvalidState,
                                  "the input is already answered, settled or rejected");
        }
        auto loaded = impl_->load_input(entry.value());
        if (!loaded)
            return loaded.error();
        envelope = std::move(loaded).value();
        envelope.principal = principal; // Never reuse stored authentication.
    }
    return submit_impl(std::move(envelope), true);
}
} // namespace mira
