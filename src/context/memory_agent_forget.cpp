#include "memory_preference_support.hpp"

#include <limits>

namespace mira {
using namespace memory_preference;
namespace {
constexpr auto kPrepared = "mira.memory-agent.record-forget-prepared.v1";
constexpr auto kCompleted = "mira.memory-agent.record-forget-completed.v1";

Error denied() { return error(ErrorCode::PermissionDenied, "memory forget target is unavailable"); }

Error target_changed() {
    return make_memory_error(MemoryDomainCode::VersionConflict, "memory forget target changed");
}

Result<void> check_operation(const OperationContext &operation) {
    if (operation.cancelled())
        return error(ErrorCode::Cancelled, "memory intent was cancelled");
    if (operation.expired(Timestamp::now()))
        return error(ErrorCode::DeadlineExceeded, "memory intent deadline expired");
    return {};
}

MemoryScope intent_scope(MemoryScopeKind kind, const PrincipalContext &principal,
                         SessionId session) {
    return kind == MemoryScopeKind::User ? memory_user_scope(principal)
                                         : memory_session_scope(principal, session);
}

Result<void> append_forget(IEventStore &events, RuntimeId runtime, OperationId operation,
                           const char *type, JsonValue::Object body) {
    body.emplace_back("operation", operation.to_string());
    const AppendRequest append{
        identity<EventId>(type, MutationId{operation.value}),
        runtime,
        memory_agent_support::control_partition(),
        {},
        {type, to_json_string(JsonValue{std::move(body)}), EventClass::Critical},
        0,
        Durability::ProcessCrash};
    try {
        const auto receipt = events.append(append);
        if (receipt && receipt.value().achieved >= Durability::ProcessCrash &&
            receipt.value().event_id == append.event_id && receipt.value().session_sequence != 0)
            return {};
    } catch (...) {
    }
    return error(ErrorCode::DataLoss, "memory forget receipt was not durably acknowledged");
}
} // namespace

bool MemoryAgentService::State::record_forget_held(const MemoryWorkBinding &binding) const {
    return !record_forget_holds.empty() &&
           std::ranges::any_of(binding.scopes, [&](const auto &scope) {
               return record_forget_holds.contains(memory_agent_support::scope_key(scope));
           });
}

Result<void> MemoryAgentService::State::clean_record_forget(const RecordForget &entry) {
    for (const auto &id : entry.preferences) {
        const auto found = preference_entries.find(id);
        if (found == preference_entries.end() ||
            memory_agent_support::scope_key(found->second.proposal.scope) != entry.scope)
            return error(ErrorCode::DataLoss, "memory forget preference reference is invalid");
        found->second.proposal.state = PreferenceProposalState::Erased;
    }
    for (const auto &id : entry.preferences)
        if (auto erased = clean_preference_body(preference_entries.at(id)); !erased)
            return erased;
    return erase_projections(entry.scope);
}

Result<bool> MemoryAgentService::State::restore_record_forget(const EventEnvelope &event) {
    if (event.payload.type != kPrepared && event.payload.type != kCompleted)
        return false;
    const auto malformed = [] {
        return error(ErrorCode::DataLoss, "memory forget control receipt is malformed");
    };
    const auto json = parse_json(event.payload.data);
    const auto *operation_text = json ? field(json.value(), "operation") : nullptr;
    const auto id = operation_text ? OperationId::parse(*operation_text) : std::nullopt;
    if (!id || id->is_nil() || event.payload.classification != EventClass::Critical ||
        event.event_id != identity<EventId>(event.payload.type, MutationId{id->value}))
        return malformed();
    if (event.payload.type == kCompleted) {
        const auto found = record_forgets.find(*id);
        const auto *changed = json.value().find("target_changed");
        if (found == record_forgets.end() || found->second.complete || !changed ||
            !changed->is_boolean() || record_forget_holds.erase(found->second.scope) != 1)
            return malformed();
        found->second.complete = true;
        found->second.target_changed = *changed->as_boolean();
        found->second.preferences.clear();
        return true;
    }
    const auto *scope = field(json.value(), "scope_digest");
    const auto *fingerprint = field(json.value(), "identity");
    const auto hash = fingerprint ? digest_from_hex(*fingerprint) : std::nullopt;
    const auto *preferences = json.value().find("preferences");
    if (!scope || !digest_from_hex(*scope) || !hash || !preferences || !preferences->is_array() ||
        preferences->as_array()->size() > config.max_preference_proposals ||
        record_forgets.contains(*id) || record_forget_holds.contains(*scope))
        return malformed();
    RecordForget entry{*scope, *hash, {}, true};
    for (const auto &value : *preferences->as_array()) {
        const auto proposal =
            value.is_string() ? MutationId::parse(*value.as_string()) : std::nullopt;
        if (!proposal || proposal->is_nil())
            return malformed();
        entry.preferences.push_back(*proposal);
    }
    record_forget_holds.emplace(entry.scope, *id);
    if (auto cleaned = clean_record_forget(entry); !cleaned)
        return cleaned.error();
    record_forgets.emplace(*id, std::move(entry));
    return true;
}

Result<MemoryWorkBinding>
MemoryAgentService::State::record_intent_binding(const PrincipalContext &principal,
                                                 const MemoryScope &scope,
                                                 const OperationContext &operation) const {
    const auto key = memory_agent_support::scope_key(scope);
    MemoryWorkBinding binding;
    {
        const std::lock_guard lock(mutex);
        const auto found = works.find(operation.task);
        if (!initialized || closing || found == works.end() || !found->second.active ||
            found->second.revoked || found->second.input_pending ||
            !memory_agent_support::same_owner(operation, found->second.binding.work) ||
            !owner(found->second.binding.principal, principal) ||
            std::ranges::find(found->second.binding.scopes, scope) ==
                found->second.binding.scopes.end() ||
            blocked_scopes.contains(key))
            return error(ErrorCode::PermissionDenied, "memory intent target is unavailable");
        binding = found->second.binding;
    }
    if (!manage(principal, scope, operation.session) ||
        !memory_agent_support::request_current(binding))
        return error(ErrorCode::PermissionDenied, "memory intent target is unavailable");
    return binding;
}

Result<std::optional<MemoryIntentTarget>>
MemoryAgentService::resolve_memory_target(const PrincipalContext &principal,
                                          const MemoryTargetSelector &selector,
                                          const OperationContext &operation) const try {
    if ((selector.scope != MemoryScopeKind::User && selector.scope != MemoryScopeKind::Session) ||
        memory_kind_policy(selector.kind).kind != selector.kind || !operation.deadline ||
        selector.claim_key.size() > 4096 || selector.claim_key.find('\0') != std::string::npos)
        return error(ErrorCode::InvalidArgument, "memory target requires a scope, kind and key");
    const auto claim_key = memory_normalize_claim_key(selector.claim_key);
    if (claim_key.empty())
        return error(ErrorCode::InvalidArgument, "memory target claim key is empty");
    auto &s = *state_;
    const std::lock_guard lifecycle(s.close_mutex);
    const std::lock_guard serialize(s.commit_mutex);
    if (auto active = check_operation(operation); !active)
        return active.error();
    const auto scope = intent_scope(selector.scope, principal, operation.session);
    const auto binding = s.record_intent_binding(principal, scope, operation);
    if (!binding)
        return binding.error();
    if (s.record_forget_holds.contains(memory_agent_support::scope_key(scope)))
        return make_memory_error(MemoryDomainCode::ErasurePending,
                                 "memory scope is awaiting record forget");

    const auto match =
        s.memory->find_current_claim(scope, selector.kind, claim_key, *operation.deadline);
    if (auto active = check_operation(operation); !active)
        return active.error();
    if (!manage(principal, scope, operation.session) ||
        !memory_agent_support::request_current(binding.value()))
        return error(ErrorCode::PermissionDenied, "memory intent target is unavailable");
    if (!match)
        return match.error();
    if (!match.value())
        return std::optional<MemoryIntentTarget>{};
    const auto &record = *match.value();
    if (record.scope != scope || record.kind != selector.kind || record.id.is_nil() ||
        record.version == 0 || record.status != MemoryStatus::Active ||
        memory_claim_key(record).key != claim_key)
        return error(ErrorCode::DataLoss, "memory backend returned an invalid intent target");
    return std::optional{MemoryIntentTarget{record.id, record.version}};
} catch (...) {
    return error(ErrorCode::Unavailable, "memory target lookup failed");
}

Result<MemoryRecordErasureReceipt>
MemoryAgentService::forget_record(const PrincipalContext &principal,
                                  const MemoryForgetRequest &request,
                                  const OperationContext &operation) try {
    auto &s = *state_;
    const std::lock_guard lifecycle(s.close_mutex);
    const std::lock_guard serialize(s.commit_mutex);
    if (request.operation.is_nil() || request.record.is_nil() || request.expected_version == 0 ||
        request.expected_version >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        (request.scope != MemoryScopeKind::User && request.scope != MemoryScopeKind::Session))
        return error(ErrorCode::InvalidArgument,
                     "memory forget requires a scope, record and version");
    if (!operation.deadline)
        return error(ErrorCode::InvalidArgument, "memory forget requires a deadline");
    if (auto active = check_operation(operation); !active)
        return active.error();
    const auto scope = intent_scope(request.scope, principal, operation.session);
    const auto key = memory_agent_support::scope_key(scope);
    const auto binding = s.record_intent_binding(principal, scope, operation);
    if (!binding)
        return binding.error();
    const auto current = [&] {
        return manage(principal, scope, operation.session) &&
               memory_agent_support::request_current(binding.value());
    };
    if (!s.memory->supports_record_erasure())
        return error(ErrorCode::UnsupportedCapability,
                     "memory backend cannot erase an exact record");
    if (scope.kind == MemoryScopeKind::User && s.preference_fence)
        return *s.preference_fence;

    JsonValue::Object intent{{"user", principal.user_id.to_string()},
                             {"identity_version", std::to_string(principal.identity_version)},
                             {"scope_digest", key},
                             {"record", request.record.to_string()},
                             {"version", std::to_string(request.expected_version)}};
    if (request.input_source)
        intent.emplace_back("input_source", request.input_source->to_string());
    const auto fingerprint = canonical_json_digest(JsonValue{std::move(intent)});
    const MemoryRecordErasureRequest storage_request{request.operation, scope, request.record,
                                                     request.expected_version};
    auto found = s.record_forgets.find(request.operation);
    if (found != s.record_forgets.end()) {
        if (found->second.identity != fingerprint || found->second.scope != key)
            return denied();
        if (found->second.target_changed)
            return target_changed();
        if (found->second.complete)
            return s.memory->erase_record(storage_request);
    } else {
        if (request.input_source && request.input_source != binding.value().input_source)
            return error(ErrorCode::InvalidState, "memory intent input was replaced");
        if (s.record_forget_holds.contains(key))
            return make_memory_error(MemoryDomainCode::ErasurePending,
                                     "memory scope is awaiting an earlier forget");
        const auto record = s.memory->get(request.record, scope);
        if (!record)
            return record.error();
        if (!record.value() || record.value()->scope != scope ||
            record.value()->id != request.record)
            return denied();
        if (record.value()->version != request.expected_version)
            return target_changed();

        State::RecordForget entry{key, fingerprint, {}};
        for (const auto &[id, candidate] : s.preference_entries) {
            if (candidate.proposal.scope != scope)
                continue;
            if (auto active = check_operation(operation); !active)
                return active.error();
            if (closed(candidate.proposal.state)) {
                if (auto cleaned = s.clean_preference_body(candidate); !cleaned)
                    return cleaned.error();
                continue;
            }
            const auto proposal = s.read_preference(candidate, true);
            if (!proposal)
                return proposal.error();
            const auto &value = proposal.value();
            if ((value.previous && value.previous->id == request.record) ||
                (value.mutation && value.mutation->proposed.id == request.record) ||
                (value.applied && value.applied->record == request.record))
                entry.preferences.push_back(id);
        }
        if (auto active = check_operation(operation); !active)
            return active.error();
        if (!current())
            return denied();
        found = s.record_forgets.emplace(request.operation, std::move(entry)).first;
        const std::lock_guard lock(s.mutex);
        s.record_forget_holds.emplace(key, request.operation);
        for (auto &[id, work] : s.works) {
            (void)id;
            if (std::ranges::find(work.binding.scopes, scope) != work.binding.scopes.end())
                s.invalidate(work);
        }
    }

    auto &entry = found->second;
    if (!entry.durable) {
        JsonValue::Array preferences;
        for (const auto &id : entry.preferences)
            preferences.emplace_back(id.to_string());
        if (auto saved = append_forget(*s.events, s.runtime, request.operation, kPrepared,
                                       {{"scope_digest", key},
                                        {"identity", fingerprint.to_string()},
                                        {"preferences", std::move(preferences)}});
            !saved)
            return saved.error();
        entry.durable = true;
    }
    if (auto cleaned = s.clean_record_forget(entry); !cleaned)
        return cleaned.error();
    if (auto active = check_operation(operation); !active)
        return active.error();
    if (!current())
        return denied();
    auto erased = s.memory->erase_record(storage_request);
    const bool changed =
        !erased && erased.error().domain == "mira.memory" &&
        (erased.error().domain_code == static_cast<int>(MemoryDomainCode::VersionConflict) ||
         erased.error().domain_code == static_cast<int>(MemoryDomainCode::ScopeDenied));
    if ((!erased && !changed) ||
        (erased && erased.value().erasure.status == ErasureStatus::Pending))
        return erased;
    if (auto saved = append_forget(*s.events, s.runtime, request.operation, kCompleted,
                                   {{"target_changed", changed}});
        !saved)
        return saved.error();
    entry.complete = true;
    entry.target_changed = changed;
    entry.preferences.clear();
    const std::lock_guard lock(s.mutex);
    s.record_forget_holds.erase(key);
    if (changed)
        return target_changed();
    return erased;
} catch (...) {
    return error(ErrorCode::DataLoss,
                 "memory forget outcome is uncertain; retry the same operation");
}
} // namespace mira
