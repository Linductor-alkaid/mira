#pragma once

#include <mira/task_checkpoint.hpp>
#include <set>

namespace mira::checkpoint_recovery {
enum class GuardResult { NotGuarded, Applied, Rejected };

// Guarded recovery proposals are separate event types so older reducers
// ignore them conservatively. Validation runs against the PREVIOUS prefix.
inline GuardResult apply(TaskCheckpoint &checkpoint, const EventEnvelope &event,
                         std::uint64_t previous_sequence, ReducerStats &stats) {
    const bool observation = event.payload.type == "RecoveryObservationRecorded";
    const bool verification = event.payload.type == "RecoveryVerificationResult";
    if (!observation && !verification)
        return GuardResult::NotGuarded;
    const auto json = parse_json(event.payload.data);
    const auto malformed = [&] {
        ++stats.malformed;
        return GuardResult::Rejected;
    };
    if (!json || !json.value().is_object() || event.task_id != checkpoint.task_id ||
        event.session_id != checkpoint.session_id)
        return malformed();
    const auto *schema = json.value().find("schema");
    const auto *expected = json.value().find("expected_through_event_sequence");
    if (!schema || !schema->is_string() ||
        *schema->as_string() != (observation ? "mira.interaction.recovery-observation.v1"
                                             : "mira.interaction.recovery-verification.v1") ||
        !expected || !expected->is_integer() || expected->as_integer().value() < 0)
        return malformed();
    if (static_cast<std::uint64_t>(expected->as_integer().value()) != previous_sequence) {
        ++stats.stale_skipped;
        return GuardResult::Rejected;
    }
    const auto *id = json.value().find("observation_id");
    const auto *digest = json.value().find(observation ? "digest" : "observation_digest");
    if (!id || !id->is_string() || !digest || !digest->is_string())
        return malformed();
    const auto observation_id = ObservationId::parse(*id->as_string());
    const auto observation_digest = digest_from_hex(*digest->as_string());
    if (!observation_id || observation_id->is_nil() || !observation_digest)
        return malformed();
    if (observation) {
        checkpoint.current_observation =
            CheckpointObservationRef{*observation_id, *observation_digest, event.session_sequence};
    } else {
        const auto *verdict = json.value().find("verdict");
        const auto *resolved = json.value().find("resolved_actions");
        if (!checkpoint.current_observation ||
            checkpoint.current_observation->sequence != previous_sequence ||
            checkpoint.current_observation->observation_id != *observation_id ||
            checkpoint.current_observation->digest != *observation_digest || !verdict ||
            !verdict->is_string() || *verdict->as_string() != "satisfied" || !resolved ||
            !resolved->is_array() ||
            resolved->as_array()->size() != checkpoint.uncertain_side_effects.size())
            return malformed();
        std::set<std::string> actions;
        for (const auto &effect : checkpoint.uncertain_side_effects)
            actions.insert(effect.action_id.to_string());
        for (const auto &value : *resolved->as_array())
            if (!value.is_string() || actions.erase(*value.as_string()) != 1)
                return malformed();
        checkpoint.verification_pending = false;
        checkpoint.uncertain_side_effects.clear();
        checkpoint.recovery_verification_sequence = event.session_sequence;
    }
    ++stats.applied;
    return GuardResult::Applied;
}
} // namespace mira::checkpoint_recovery
