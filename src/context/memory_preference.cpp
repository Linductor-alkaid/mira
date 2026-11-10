#include "context_text.hpp"
#include "memory_preference_support.hpp"

namespace mira {
using namespace memory_preference;
namespace {
Result<std::optional<MemoryRecord>> current_claim(IMemory &memory, const MemoryScope &scope,
                                                  const std::string &statement) {
    MemoryQuery query;
    query.scopes = {scope};
    query.kinds = std::vector<MemoryKind>{MemoryKind::Preference};
    query.claim_key = memory_claim_key(statement).key;
    query.max_results = 2;
    const auto found = memory.query(query);
    if (!found)
        return found.error();
    if (found.value().records.size() > 1)
        return make_memory_error(MemoryDomainCode::VersionConflict,
                                 "preference claim is ambiguous");
    if (found.value().records.empty())
        return std::optional<MemoryRecord>{};
    const auto &record = found.value().records.front();
    if (record.scope != scope || record.kind != MemoryKind::Preference ||
        record.status != MemoryStatus::Active ||
        memory_claim_key(record.statement).key != query.claim_key)
        return error(ErrorCode::PermissionDenied, "memory backend returned a foreign preference");
    return std::optional<MemoryRecord>{record};
}
JsonValue::Object decision_json(const PreferenceProposal &proposal) {
    return JsonValue::Object{{"id", proposal.id.to_string()},
                             {"digest", proposal.digest.to_string()}};
}
Result<void> precondition(IMemory &memory, const PreferenceProposal &proposal) {
    auto current = current_claim(memory, proposal.scope, proposal.statement);
    if (!current)
        return current.error();
    const auto &mutation = *proposal.mutation;
    if ((!mutation.target && current.value()) ||
        (mutation.target && (!current.value() || current.value()->id != *mutation.target ||
                             current.value()->version != *mutation.expected_version)))
        return make_memory_error(MemoryDomainCode::VersionConflict,
                                 "preference changed since its proposal; review a new proposal");
    return {};
}
} // namespace

Result<PreferenceProposal>
MemoryAgentService::State::propose_preference(PreferenceProposalRequest request,
                                              const OperationContext &context) {
    const std::lock_guard lifecycle(close_mutex);
    const std::lock_guard serialize(commit_mutex);
    if (auto ready = preference_available(context); !ready)
        return ready.error();
    if (request.id.is_nil() || request.statement.empty() || request.statement.size() > 4096 ||
        !memory_claim_key(request.statement).keyed ||
        request.target.has_value() != request.expected_version.has_value())
        return error(ErrorCode::InvalidArgument, "preference requires an id and bounded key=value");
    MemoryWorkBinding binding;
    {
        const std::lock_guard lock(mutex);
        const auto work = works.find(context.task);
        if (work == works.end() || !work->second.active || work->second.revoked ||
            work->second.input_pending ||
            !memory_agent_support::same_owner(context, work->second.binding.work) ||
            !memory_agent_support::request_current(work->second.binding))
            return error(ErrorCode::PermissionDenied,
                         "preference caller is not an active bound work");
        binding = work->second.binding;
    }
    const auto scope = memory_user_scope(binding.principal);
    if (binding.principal.identity_version == 0 ||
        std::find(binding.scopes.begin(), binding.scopes.end(), scope) == binding.scopes.end() ||
        blocked_scopes.contains(memory_agent_support::scope_key(scope)))
        return error(ErrorCode::PermissionDenied, "preference user scope is not authorized");
    JsonValue::Object fingerprint{
        {"tenant", binding.principal.tenant_id.to_string()},
        {"user", binding.principal.user_id.to_string()},
        {"identity_version", std::to_string(binding.principal.identity_version)},
        {"task", context.task.to_string()},
        {"epoch", std::to_string(context.task_epoch)},
        {"statement", request.statement}};
    if (request.target) {
        fingerprint.emplace_back("target", request.target->to_string());
        fingerprint.emplace_back("version", std::to_string(*request.expected_version));
    }
    const auto request_digest = canonical_json_digest(JsonValue{std::move(fingerprint)});
    if (const auto existing = preference_entries.find(request.id);
        existing != preference_entries.end()) {
        if (!owner(existing->second.owner, binding.principal) ||
            existing->second.request_digest != request_digest)
            return error(ErrorCode::PermissionDenied, "preference request id is unavailable");
        return read_preference(existing->second);
    }
    if (preference_entries.size() >= config.max_preference_proposals)
        return error(ErrorCode::ResourceExhausted, "preference receipt capacity exceeded");
    auto previous = current_claim(*memory, scope, request.statement);
    if (!previous)
        return previous.error();
    if (request.target && (!previous.value() || previous.value()->id != *request.target ||
                           previous.value()->version != *request.expected_version))
        return make_memory_error(MemoryDomainCode::VersionConflict,
                                 "preference target is not current");
    const auto source = identity<EventId>("mira.preference.source.v1/", request.id);
    MemoryCandidate candidate;
    candidate.proposed.id = identity<MemoryId>("mira.preference.record.v1/", request.id);
    candidate.proposed.scope = scope;
    candidate.proposed.kind = MemoryKind::Preference;
    candidate.proposed.statement = request.statement;
    candidate.proposed.sensitivity = Sensitivity::Sensitive;
    candidate.proposed.verification = MemoryVerification::Unverified;
    candidate.proposed.confidence = 0.5F;
    candidate.proposed.validity.valid_from =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(Timestamp::now().wall);
    candidate.proposed.recorded_at = candidate.proposed.validity.valid_from;
    candidate.proposed.provenance = {source};
    candidate.evidence = {source};
    candidate.model_assisted = true;
    candidate.reason =
        previous.value() ? MutationReasonCode::HumanCorrection : MutationReasonCode::Consolidation;
    auto policy = config.consolidation;
    policy.allow_model_import_exemption = false;
    policy.require_explicit_approval = true;
    auto report = MemoryConsolidator{policy}.consolidate_candidates(*memory, {candidate}, scope,
                                                                    Timestamp::now());
    if (!report)
        return report.error();
    const bool no_change = report.value().entries.size() == 1 &&
                           report.value().entries.front().reason_code == "duplicate-noop";
    if (!no_change && report.value().pending_approval.size() != 1)
        return error(ErrorCode::SafetyRejected,
                     "preference candidate was rejected by consolidation");
    JsonValue::Object body{{"schema", "mira.memory-agent.preference.v1"},
                           {"id", request.id.to_string()},
                           {"statement", request.statement}};
    if (previous.value())
        body.emplace_back("previous", memory_record_to_json(*previous.value()));
    if (!no_change)
        body.emplace_back("mutation",
                          memory_mutation_to_json(report.value().pending_approval.front()));
    const auto bytes = to_json_string(JsonValue{std::move(body)});
    if (bytes.size() > config.max_source_bytes)
        return error(ErrorCode::ResourceExhausted, "preference artifact exceeds capacity");
    if (auto ready = preference_available(context); !ready)
        return ready.error();
    ArtifactWriteSpec spec;
    spec.media_type = "application/json";
    spec.encoding = ArtifactEncoding::Utf8;
    spec.sensitivity = Sensitivity::Sensitive;
    spec.content_schema = {1, 0};
    spec.max_bytes = config.max_source_bytes;
    auto writer = config.artifacts->begin(spec);
    if (!writer)
        return writer.error();
    if (auto wrote = writer.value().write(bytes.data(), bytes.size()); !wrote)
        return wrote.error();
    auto artifact = config.artifacts->commit(writer.value());
    if (!artifact) {
        preference_fence =
            error(ErrorCode::DataLoss, "preference artifact commit is uncertain; reopen required");
        return *preference_fence;
    }
    const auto abandon = [&](Error reason) -> Result<PreferenceProposal> {
        auto erased = config.artifacts->erase({artifact.value().id, "interrupted-preference"});
        if (erased && erased.value().payload_erased)
            return reason;
        preference_fence = error(ErrorCode::DataLoss, "unregistered preference cleanup is pending");
        return *preference_fence;
    };
    if (auto ready = preference_available(context); !ready) {
        return abandon(ready.error());
    }
    if (!memory_agent_support::request_current(binding)) {
        return abandon(error(ErrorCode::InvalidState, "preference work frame changed"));
    }
    PreferenceEntry entry;
    entry.owner.tenant_id = binding.principal.tenant_id;
    entry.owner.user_id = binding.principal.user_id;
    entry.owner.identity_version = binding.principal.identity_version;
    entry.request_digest = request_digest;
    entry.artifact = artifact.value();
    entry.proposal.id = request.id;
    entry.proposal.scope = scope;
    entry.proposal.digest = artifact.value().digest;
    entry.proposal.expires_at = Timestamp::now().wall + config.preference_proposal_ttl;
    if (previous.value()) {
        for (const auto until :
             {previous.value()->expires_at, previous.value()->validity.valid_until})
            if (until)
                entry.proposal.expires_at =
                    std::min(entry.proposal.expires_at,
                             std::chrono::time_point_cast<std::chrono::nanoseconds>(*until));
    }
    entry.proposal.state =
        no_change ? PreferenceProposalState::NoChange : PreferenceProposalState::Pending;
    auto &stored = preference_entries.emplace(request.id, std::move(entry)).first->second;
    auto registered = append_preference(
        "mira.memory-agent.preference-proposed.v1",
        JsonValue::Object{{"id", request.id.to_string()},
                          {"tenant", binding.principal.tenant_id.to_string()},
                          {"user", binding.principal.user_id.to_string()},
                          {"identity_version", std::to_string(binding.principal.identity_version)},
                          {"request_digest", request_digest.to_string()},
                          {"artifact_id", stored.artifact.id.to_string()},
                          {"digest", stored.proposal.digest.to_string()},
                          {"bytes", std::to_string(stored.artifact.byte_size)},
                          {"expires_at", wall_nanos(stored.proposal.expires_at)},
                          {"no_change", no_change}},
        source);
    if (!registered)
        return registered.error(); // Keep body if its registration may be durable.
    return read_preference(stored);
}

Result<PreferenceProposal>
MemoryAgentService::State::decide_preference(const PrincipalContext &principal, MutationId id,
                                             Sha256Digest digest, ConfirmationDecision decision,
                                             const OperationContext &context) {
    const std::lock_guard lifecycle(close_mutex);
    const std::lock_guard serialize(commit_mutex);
    if (auto ready = preference_available(context); !ready)
        return ready.error();
    const auto found = preference_entries.find(id);
    if (found == preference_entries.end() || !owner(found->second.owner, principal) ||
        !manage(principal, found->second.proposal.scope))
        return error(ErrorCode::PermissionDenied, "preference proposal is unavailable");
    auto &entry = found->second;
    if (digest != entry.proposal.digest)
        return error(ErrorCode::InvalidArgument, "preference approval digest does not match");
    if (closed(entry.proposal.state))
        return error(ErrorCode::InvalidState, "preference proposal is closed");
    if (decision != ConfirmationDecision::Approve && decision != ConfirmationDecision::Reject)
        return error(ErrorCode::InvalidArgument, "unknown preference decision");
    if (entry.proposal.state == PreferenceProposalState::Applied ||
        entry.proposal.state == PreferenceProposalState::NoChange) {
        if (decision == ConfirmationDecision::Reject)
            return error(ErrorCode::InvalidState, "completed preference cannot be rejected");
        return read_preference(entry);
    }
    if (Timestamp::now().wall >= entry.proposal.expires_at)
        return error(ErrorCode::DeadlineExceeded,
                     entry.proposal.state == PreferenceProposalState::Approving
                         ? "expired approval may have committed; host reconciliation required"
                         : "preference proposal expired");
    auto proposal = read_preference(entry);
    if (!proposal)
        return proposal.error();
    if (decision == ConfirmationDecision::Reject) {
        if (entry.proposal.state != PreferenceProposalState::Pending)
            return error(ErrorCode::InvalidState, "resolve in-progress preference approval first");
        auto rejected = append_preference("mira.memory-agent.preference-rejected.v1",
                                          decision_json(entry.proposal));
        if (!rejected)
            return rejected.error();
        entry.proposal.state = PreferenceProposalState::Rejected;
        if (auto erased = clean_preference_bodies(); !erased)
            return erased.error();
        return read_preference(entry);
    }
    const auto &mutation = *proposal.value().mutation;
    if (context_text::carries_marker(mutation.proposed.statement,
                                     config.consolidation.forbidden_markers) ||
        context_text::carries_marker(mutation.proposed.statement,
                                     config.consolidation.injection_markers))
        return error(ErrorCode::SafetyRejected, "current preference policy rejects the candidate");
    bool replay = false;
    if (entry.proposal.state == PreferenceProposalState::Approving) {
        const auto existing = memory->get(mutation.proposed.id, mutation.scope);
        if (!existing)
            return existing.error();
        replay = existing.value() && existing.value()->statement == mutation.proposed.statement &&
                 existing.value()->provenance == mutation.proposed.provenance &&
                 existing.value()->verification == mutation.proposed.verification;
    }
    if (!replay) {
        if (auto current = precondition(*memory, proposal.value()); !current)
            return current.error();
    }
    if (auto ready = preference_available(context); !ready)
        return ready.error();
    if (entry.proposal.state == PreferenceProposalState::Pending) {
        auto intent = append_preference("mira.memory-agent.preference-approval-intent.v1",
                                        decision_json(entry.proposal));
        if (!intent)
            return intent.error();
        entry.proposal.state = PreferenceProposalState::Approving;
    }
    {
        const std::lock_guard lock(mutex);
        if (closing || blocked_scopes.contains(memory_agent_support::scope_key(mutation.scope)))
            return error(ErrorCode::InvalidState, "preference scope is closed");
        for (auto &[task, work] : works) {
            (void)task;
            if (std::find(work.binding.scopes.begin(), work.binding.scopes.end(), mutation.scope) !=
                work.binding.scopes.end())
                invalidate(work);
        }
    }
    if (auto ready = preference_available(context); !ready)
        return ready.error();
    if (!manage(principal, entry.proposal.scope))
        return error(ErrorCode::PermissionDenied, "preference approval grant expired");
    if (Timestamp::now().wall >= entry.proposal.expires_at)
        return error(ErrorCode::DeadlineExceeded, "preference review window expired before apply");
    auto applied = MemoryConsolidator::apply_pending(*memory, mutation);
    if (!applied) {
        preference_fence =
            error(ErrorCode::DataLoss, "preference apply is uncertain; retry after reopen");
        return *preference_fence;
    }
    auto completion = decision_json(entry.proposal);
    completion.emplace_back("result", result_json(applied.value()));
    auto recorded =
        append_preference("mira.memory-agent.preference-applied.v1", std::move(completion));
    if (!recorded)
        return recorded.error();
    entry.proposal.state = PreferenceProposalState::Applied;
    entry.proposal.applied = applied.value();
    return read_preference(entry);
}

Result<PreferenceProposal> MemoryAgentService::propose_preference(PreferenceProposalRequest request,
                                                                  const OperationContext &context) {
    try {
        return state_->propose_preference(std::move(request), context);
    } catch (...) {
        const std::lock_guard lock(state_->commit_mutex);
        state_->preference_fence =
            error(ErrorCode::DataLoss, "preference proposal failed; reopen required");
        return *state_->preference_fence;
    }
}
Result<PreferenceProposal>
MemoryAgentService::preference_proposal(const PrincipalContext &principal, MutationId id,
                                        const OperationContext &context) const {
    try {
        const std::lock_guard lifecycle(state_->close_mutex);
        const std::lock_guard serialize(state_->commit_mutex);
        if (auto ready = state_->preference_available(context); !ready)
            return ready.error();
        const auto found = state_->preference_entries.find(id);
        if (found == state_->preference_entries.end() || !owner(found->second.owner, principal) ||
            !manage(principal, found->second.proposal.scope))
            return error(ErrorCode::PermissionDenied, "preference proposal is unavailable");
        auto result = state_->read_preference(found->second);
        if (auto ready = state_->preference_available(context); !ready)
            return ready.error();
        if (!manage(principal, found->second.proposal.scope))
            return error(ErrorCode::PermissionDenied, "preference preview grant expired");
        return result;
    } catch (...) {
        return error(ErrorCode::Unavailable, "preference preview failed");
    }
}
Result<PreferenceProposal> MemoryAgentService::decide_preference(const PrincipalContext &principal,
                                                                 MutationId id, Sha256Digest digest,
                                                                 ConfirmationDecision decision,
                                                                 const OperationContext &context) {
    try {
        return state_->decide_preference(principal, id, digest, decision, context);
    } catch (...) {
        const std::lock_guard lock(state_->commit_mutex);
        state_->preference_fence =
            error(ErrorCode::DataLoss, "preference approval failed; reopen required");
        return *state_->preference_fence;
    }
}
} // namespace mira
