#include "memory_preference_support.hpp"

namespace mira {
using namespace memory_preference;

Result<void>
MemoryAgentService::State::preference_available(const OperationContext &context) const {
    if (!context.deadline)
        return error(ErrorCode::InvalidArgument, "preference operations require a deadline");
    if (context.cancelled_or_expired(Timestamp::now()))
        return error(context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded,
                     "preference operation was interrupted");
    const std::lock_guard lock(mutex);
    if (!initialized || closing || !config.artifacts || preference_fence)
        return error(ErrorCode::InvalidState,
                     "preference management is closed or requires rebuild");
    return {};
}

Result<void> MemoryAgentService::State::append_preference(const char *type,
                                                          JsonValue::Object payload,
                                                          EventId event) {
    payload.emplace_back("schema", type);
    AppendRequest request;
    request.event_id = event.is_nil() ? EventId::generate() : event;
    request.runtime_id = runtime;
    request.session_id = memory_agent_support::control_partition();
    request.payload = {type, to_json_string(JsonValue{std::move(payload)}), EventClass::State};
    request.required = Durability::ProcessCrash;
    try {
        const auto receipt = events->append(request);
        if (receipt && receipt.value().achieved >= Durability::ProcessCrash)
            return {};
    } catch (...) {
    }
    preference_fence = error(ErrorCode::DataLoss,
                             "preference acknowledgement is uncertain; reopen stores and service");
    return *preference_fence;
}

Result<PreferenceProposal>
MemoryAgentService::State::read_preference(const PreferenceEntry &entry) const {
    auto result = entry.proposal;
    if (closed(result.state))
        return result;
    // The frozen body is never reissued after its review/source window.
    // Terminal metadata remains useful without replaying old personal data.
    if (Timestamp::now().wall >= result.expires_at) {
        if (result.state == PreferenceProposalState::Pending)
            result.state = PreferenceProposalState::Expired;
        return result;
    }
    if (!config.artifacts)
        return error(ErrorCode::InvalidState, "preference artifacts are not configured");
    const auto reader = config.artifacts->open(entry.artifact);
    if (!reader)
        return reader.error();
    if (reader.value().size() != entry.artifact.byte_size ||
        reader.value().size() > config.max_source_bytes)
        return error(ErrorCode::DataLoss, "preference artifact size is invalid");
    const std::string body(reinterpret_cast<const char *>(reader.value().bytes().data()),
                           reader.value().size());
    if (digest_string(body) != result.digest)
        return error(ErrorCode::DataLoss, "preference artifact digest is invalid");
    const auto parsed = parse_json(body);
    const auto *schema = parsed ? field(parsed.value(), "schema") : nullptr;
    const auto *id = parsed ? field(parsed.value(), "id") : nullptr;
    const auto *statement = parsed ? field(parsed.value(), "statement") : nullptr;
    if (!schema || *schema != "mira.memory-agent.preference.v1" || !id ||
        *id != result.id.to_string() || !statement || statement->size() > 4096 ||
        !memory_claim_key(*statement).keyed)
        return error(ErrorCode::DataLoss, "preference artifact is malformed");
    result.statement = *statement;
    if (const auto *previous = parsed.value().find("previous")) {
        auto record = memory_record_from_json(*previous);
        if (!record || record.value().scope != result.scope ||
            record.value().kind != MemoryKind::Preference)
            return error(ErrorCode::DataLoss, "preference predecessor is malformed");
        result.previous = std::move(record).value();
    }
    if (const auto *mutation = parsed.value().find("mutation")) {
        auto decoded = memory_mutation_from_json(*mutation);
        if (!decoded || decoded.value().scope != result.scope ||
            decoded.value().proposed.kind != MemoryKind::Preference ||
            decoded.value().proposed.verification != MemoryVerification::Unverified ||
            decoded.value().proposed.statement != result.statement ||
            decoded.value().proposed.sensitivity == Sensitivity::Secret ||
            (decoded.value().type != MemoryMutationType::Add &&
             decoded.value().type != MemoryMutationType::Supersede))
            return error(ErrorCode::DataLoss, "preference mutation is malformed");
        result.mutation = std::move(decoded).value();
        if (result.mutation->target.has_value() != result.previous.has_value() ||
            (result.previous && (result.mutation->target != result.previous->id ||
                                 result.mutation->expected_version != result.previous->version ||
                                 memory_claim_key(result.previous->statement).key !=
                                     memory_claim_key(result.statement).key)) ||
            (result.applied && result.applied->record != result.mutation->proposed.id))
            return error(ErrorCode::DataLoss, "preference approval frame is inconsistent");
    } else if (result.state != PreferenceProposalState::NoChange)
        return error(ErrorCode::DataLoss, "preference mutation is missing");
    return result;
}

Result<void> MemoryAgentService::State::clean_preference_bodies() {
    for (const auto &[id, entry] : preference_entries) {
        (void)id;
        if (!closed(entry.proposal.state))
            continue;
        if (!config.artifacts)
            return error(ErrorCode::InvalidState, "preference cleanup requires artifacts");
        const auto erased = config.artifacts->erase({entry.artifact.id, "preference-withdrawn"});
        if (erased && erased.value().payload_erased)
            continue;
        if (!erased && erased.error().code == ErrorCode::NotFound) {
            const auto remaining = config.artifacts->open(entry.artifact);
            if (!remaining && remaining.error().code == ErrorCode::NotFound)
                continue;
        }
        return error(ErrorCode::DataLoss, "preference artifact cleanup is pending");
    }
    return {};
}

Result<void> MemoryAgentService::State::erase_preference_bodies(const std::string &scope,
                                                                bool privacy) {
    for (auto &[id, entry] : preference_entries) {
        (void)id;
        if (memory_agent_support::scope_key(entry.proposal.scope) == scope &&
            (privacy || entry.proposal.state == PreferenceProposalState::Pending))
            entry.proposal.state =
                privacy ? PreferenceProposalState::Erased : PreferenceProposalState::Stale;
    }
    return clean_preference_bodies();
}

Result<void> MemoryAgentService::State::invalidate_preference_proposals(const MemoryScope &scope) {
    const auto key = memory_agent_support::scope_key(scope);
    bool pending = false;
    for (const auto &[id, entry] : preference_entries) {
        (void)id;
        if (entry.proposal.scope != scope)
            continue;
        if (entry.proposal.state == PreferenceProposalState::Approving)
            return error(ErrorCode::InvalidState,
                         "resolve preference approval before external writes");
        pending |= entry.proposal.state == PreferenceProposalState::Pending;
    }
    if (!pending)
        return {};
    if (preference_fence)
        return *preference_fence;
    auto recorded = append_preference("mira.memory-agent.preference-invalidated.v1",
                                      JsonValue::Object{{"scope_digest", key}});
    if (!recorded)
        return recorded;
    return erase_preference_bodies(key, false);
}

Result<bool> MemoryAgentService::State::restore_preference(const EventEnvelope &event) {
    const auto &type = event.payload.type;
    if (!type.starts_with("mira.memory-agent.preference-"))
        return false;
    const auto malformed = [] {
        return error(ErrorCode::DataLoss, "preference event is malformed");
    };
    const auto parsed = parse_json(event.payload.data);
    const auto *schema = parsed ? field(parsed.value(), "schema") : nullptr;
    if (!schema || *schema != type)
        return malformed();
    const auto &json = parsed.value();
    if (type == "mira.memory-agent.preference-invalidated.v1") {
        const auto *key = field(json, "scope_digest");
        if (!key || !digest_from_hex(*key))
            return malformed();
        if (auto erased = erase_preference_bodies(*key, false); !erased)
            return erased.error();
        return true;
    }
    const auto *id_text = field(json, "id");
    const auto id = id_text ? MutationId::parse(*id_text) : std::nullopt;
    if (!id || id->is_nil())
        return malformed();
    const auto found = preference_entries.find(*id);
    if (type == "mira.memory-agent.preference-proposed.v1") {
        const auto *tenant = field(json, "tenant");
        const auto *user = field(json, "user");
        const auto version = number(json, "identity_version");
        const auto *digest = field(json, "digest");
        const auto *fingerprint = field(json, "request_digest");
        const auto *artifact = field(json, "artifact_id");
        const auto bytes = number(json, "bytes");
        const auto *expires = json.find("expires_at");
        const auto *no_change = json.find("no_change");
        const auto tenant_id = tenant ? TenantId::parse(*tenant) : std::nullopt;
        const auto user_id = user ? UserId::parse(*user) : std::nullopt;
        const auto hash = digest ? digest_from_hex(*digest) : std::nullopt;
        const auto request_hash = fingerprint ? digest_from_hex(*fingerprint) : std::nullopt;
        const auto artifact_id = artifact ? ArtifactId::parse(*artifact) : std::nullopt;
        if (!tenant_id || tenant_id->is_nil() || !user_id || user_id->is_nil() || !version ||
            *version == 0 || !hash || !request_hash || !artifact_id || artifact_id->is_nil() ||
            !bytes || *bytes == 0 || *bytes > config.max_source_bytes || !expires ||
            !expires->is_integer() || !no_change || !no_change->is_boolean())
            return malformed();
        PreferenceEntry entry;
        entry.owner.tenant_id = *tenant_id;
        entry.owner.user_id = *user_id;
        entry.owner.identity_version = *version;
        entry.request_digest = *request_hash;
        entry.proposal.id = *id;
        entry.proposal.digest = *hash;
        entry.proposal.scope = memory_user_scope(entry.owner);
        entry.proposal.expires_at = WallTimePoint{std::chrono::nanoseconds{*expires->as_integer()}};
        entry.proposal.state = *no_change->as_boolean() ? PreferenceProposalState::NoChange
                                                        : PreferenceProposalState::Pending;
        entry.artifact = {
            *artifact_id,           *hash, *bytes, "application/json", ArtifactEncoding::Utf8,
            Sensitivity::Sensitive, {1, 0}};
        if (found != preference_entries.end()) {
            if (!owner(found->second.owner, entry.owner) ||
                found->second.request_digest != entry.request_digest ||
                found->second.artifact.id != entry.artifact.id ||
                found->second.proposal.digest != entry.proposal.digest)
                return malformed();
            return true; // Duplicate registration must never reset a terminal state.
        }
        if (preference_entries.size() >= config.max_preference_proposals)
            return error(ErrorCode::ResourceExhausted, "preference receipt capacity exceeded");
        preference_entries.emplace(*id, std::move(entry));
        return true;
    }
    const auto *digest = field(json, "digest");
    if (found == preference_entries.end() || !digest ||
        *digest != found->second.proposal.digest.to_string())
        return malformed();
    auto &proposal = found->second.proposal;
    if (type == "mira.memory-agent.preference-approval-intent.v1") {
        if (proposal.state == PreferenceProposalState::Pending)
            proposal.state = PreferenceProposalState::Approving;
        else if (proposal.state != PreferenceProposalState::Approving &&
                 proposal.state != PreferenceProposalState::Applied)
            return malformed();
    } else if (type == "mira.memory-agent.preference-applied.v1") {
        const auto *result = json.find("result");
        const auto *record_text = result ? field(*result, "record") : nullptr;
        const auto *mutation_type = result ? field(*result, "type") : nullptr;
        const auto record = record_text ? MemoryId::parse(*record_text) : std::nullopt;
        const auto applied =
            mutation_type ? memory_mutation_type_from(*mutation_type) : std::nullopt;
        const auto version = result ? number(*result, "version") : std::nullopt;
        const auto *replayed = result ? result->find("replayed") : nullptr;
        if ((proposal.state != PreferenceProposalState::Approving &&
             proposal.state != PreferenceProposalState::Applied) ||
            !record || record->is_nil() || !applied || !version || *version == 0 || !replayed ||
            !replayed->is_boolean())
            return malformed();
        proposal.state = PreferenceProposalState::Applied;
        proposal.applied =
            MemoryMutationResult{*applied, *record, *version, *replayed->as_boolean()};
    } else if (type == "mira.memory-agent.preference-rejected.v1") {
        if (proposal.state != PreferenceProposalState::Pending &&
            proposal.state != PreferenceProposalState::Rejected)
            return malformed();
        proposal.state = PreferenceProposalState::Rejected;
    } else
        return malformed();
    return true;
}
} // namespace mira
