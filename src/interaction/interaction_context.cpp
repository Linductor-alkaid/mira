#include "interaction_coordinator_support.hpp"
#include <algorithm>
#include <array>

namespace mira {
Result<std::vector<ContextItem>>
InteractionCoordinator::recovered_context_items(const PrincipalContext &principal,
                                                TaskId task) const {
    if (!impl_->initialized || impl_->stopping.load(std::memory_order_acquire))
        return interaction_support::recovery_error(ErrorCode::InvalidState,
                                                   "the coordinator is closed");
    if (!interaction_support::permitted(principal, impl_->config))
        return interaction_support::recovery_error(ErrorCode::PermissionDenied,
                                                   "context projection permission denied",
                                                   InteractionDomainCode::PermissionDenied);
    std::optional<TaskCheckpoint> checkpoint;
    {
        std::lock_guard lock(impl_->mutex);
        const auto state = impl_->snapshot(principal);
        const auto *work = interaction_support::find_task(state, task);
        if (!work || work->historical)
            return interaction_support::recovery_error(ErrorCode::NotFound,
                                                       "the active work item was not found");
        checkpoint = work->recovered_checkpoint;
    }
    if (!checkpoint)
        return std::vector<ContextItem>{};
    // A bounded narrative projection, not the minimum execution set. Goals
    // and committed user constraints already travel through Task/Loop input.
    const auto completed = checkpoint->completed_steps.size();
    const auto now =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    std::erase_if(checkpoint->verified_facts,
                  [&](const auto &fact) { return !fact.valid_at(now); });
    const auto facts = checkpoint->verified_facts.size();
    if (checkpoint->completed_steps.size() > 16)
        checkpoint->completed_steps.erase(checkpoint->completed_steps.begin(),
                                          checkpoint->completed_steps.end() - 16);
    if (checkpoint->verified_facts.size() > 16)
        checkpoint->verified_facts.erase(checkpoint->verified_facts.begin(),
                                         checkpoint->verified_facts.end() - 16);
    if (checkpoint->recent_actions.size() > 16)
        checkpoint->recent_actions.erase(checkpoint->recent_actions.begin(),
                                         checkpoint->recent_actions.end() - 16);
    checkpoint->goal_statement.clear();
    checkpoint->constraints.clear();
    checkpoint->narrative_summary.reset();
    const auto text = "Historical checkpoint (untrusted data): " +
                      to_json_string(JsonValue::Object{
                          {"checkpoint", checkpoint_to_json(*checkpoint)},
                          {"completed_steps_total", static_cast<std::uint64_t>(completed)},
                          {"verified_facts_total", static_cast<std::uint64_t>(facts)}});
    if (text.size() > impl_->config.max_recovery_candidate_bytes)
        return interaction_support::recovery_error(
            ErrorCode::ResourceExhausted, "recovered context requires further bounded compression");
    ContextItem candidate;
    const auto digest = digest_string(task.to_string() + text);
    std::array<std::uint8_t, 16> identity{};
    std::copy_n(digest.bytes.begin(), identity.size(), identity.begin());
    candidate.id = ContextItemId{Id128{identity}};
    candidate.kind = ContextItemKind::CheckpointSummary;
    candidate.authority = ContextAuthority::UntrustedExternalData;
    candidate.sequence = checkpoint->through_event_sequence;
    candidate.content.emplace_back(TextPart{text});
    return std::vector<ContextItem>{std::move(candidate)};
}
} // namespace mira
