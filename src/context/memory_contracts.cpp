#include <mira/memory_contracts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error memory_error(MemoryDomainCode code, std::string message) {
    return make_memory_error(code, std::move(message));
}

[[nodiscard]] std::chrono::system_clock::time_point wall_from_nanos(std::int64_t nanos) {
    // system_clock tick periods differ across platforms (msvc 100ns, libc++
    // micro on some Android builds); round-trip through an explicit cast.
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(nanos)));
}

[[nodiscard]] JsonValue artifact_ref_to_json(const ArtifactRef &reference) {
    JsonValue::Object object;
    object.emplace_back("id", reference.id.to_string());
    object.emplace_back("digest", reference.digest.to_string());
    object.emplace_back("byte_size", static_cast<std::int64_t>(reference.byte_size));
    object.emplace_back("media_type", reference.media_type);
    object.emplace_back("sensitivity", sensitivity_name(reference.sensitivity));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ArtifactRef> artifact_ref_from(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "artifact reference must be an object");
    }
    ArtifactRef reference;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "artifact reference requires an id string");
    }
    const auto parsed_id = ArtifactId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidRecord, "artifact reference id is malformed");
    }
    reference.id = *parsed_id;
    const auto *digest = json.find("digest");
    if (digest != nullptr && digest->is_string()) {
        auto parsed = digest_from_hex(*digest->as_string());
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "artifact reference digest is malformed");
        }
        reference.digest = *parsed;
    }
    const auto *byte_size = json.find("byte_size");
    if (byte_size != nullptr && byte_size->is_integer()) {
        reference.byte_size = static_cast<std::uint64_t>(byte_size->as_integer().value());
    }
    const auto *media_type = json.find("media_type");
    if (media_type != nullptr && media_type->is_string()) {
        reference.media_type = *media_type->as_string();
    }
    const auto *sensitivity = json.find("sensitivity");
    if (sensitivity != nullptr && sensitivity->is_string()) {
        const auto parsed = sensitivity_from(*sensitivity);
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "artifact reference has unknown sensitivity");
        }
        reference.sensitivity = *parsed;
    }
    return reference;
}

template <typename Enum>
[[nodiscard]] std::optional<Enum> enum_from_json(const JsonValue &json, const char *key,
                                                 std::optional<Enum> (*from)(std::string_view)) {
    const auto *text = json.find(key);
    if (text == nullptr || !text->is_string()) {
        return std::nullopt;
    }
    return from(*text->as_string());
}

} // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string memory_scope_kind_name(MemoryScopeKind kind) {
    switch (kind) {
    case MemoryScopeKind::Task:
        return "task";
    case MemoryScopeKind::Session:
        return "session";
    case MemoryScopeKind::Environment:
        return "environment";
    case MemoryScopeKind::Application:
        return "application";
    case MemoryScopeKind::User:
        return "user";
    case MemoryScopeKind::Agent:
        return "agent";
    case MemoryScopeKind::TaskSkill:
        return "task-skill";
    }
    return "unknown";
}

std::optional<MemoryScopeKind> memory_scope_kind_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryScopeKind> kKinds[] = {
        {"task", MemoryScopeKind::Task},
        {"session", MemoryScopeKind::Session},
        {"environment", MemoryScopeKind::Environment},
        {"application", MemoryScopeKind::Application},
        {"user", MemoryScopeKind::User},
        {"agent", MemoryScopeKind::Agent},
        {"task-skill", MemoryScopeKind::TaskSkill},
    };
    for (const auto &[kind_name, kind] : kKinds) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string memory_kind_name(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Preference:
        return "preference";
    case MemoryKind::EnvironmentFact:
        return "environment-fact";
    case MemoryKind::ApplicationFact:
        return "application-fact";
    case MemoryKind::Episode:
        return "episode";
    case MemoryKind::Procedure:
        return "procedure";
    case MemoryKind::RecoveryLesson:
        return "recovery-lesson";
    case MemoryKind::SkillHint:
        return "skill-hint";
    }
    return "unknown";
}

std::optional<MemoryKind> memory_kind_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryKind> kKinds[] = {
        {"preference", MemoryKind::Preference},
        {"environment-fact", MemoryKind::EnvironmentFact},
        {"application-fact", MemoryKind::ApplicationFact},
        {"episode", MemoryKind::Episode},
        {"procedure", MemoryKind::Procedure},
        {"recovery-lesson", MemoryKind::RecoveryLesson},
        {"skill-hint", MemoryKind::SkillHint},
    };
    for (const auto &[kind_name, kind] : kKinds) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string memory_verification_name(MemoryVerification verification) {
    switch (verification) {
    case MemoryVerification::Unverified:
        return "unverified";
    case MemoryVerification::Observed:
        return "observed";
    case MemoryVerification::Verified:
        return "verified";
    case MemoryVerification::HumanConfirmed:
        return "human-confirmed";
    }
    return "unknown";
}

std::optional<MemoryVerification> memory_verification_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryVerification> kLevels[] = {
        {"unverified", MemoryVerification::Unverified},
        {"observed", MemoryVerification::Observed},
        {"verified", MemoryVerification::Verified},
        {"human-confirmed", MemoryVerification::HumanConfirmed},
    };
    for (const auto &[level_name, level] : kLevels) {
        if (level_name == name) {
            return level;
        }
    }
    return std::nullopt;
}

int memory_verification_rank(MemoryVerification verification) noexcept {
    switch (verification) {
    case MemoryVerification::Unverified:
        return 0;
    case MemoryVerification::Observed:
        return 1;
    case MemoryVerification::Verified:
        return 2;
    case MemoryVerification::HumanConfirmed:
        return 3;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Claim key (design Context/Memory §13.5)
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool is_claim_space(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

} // namespace

std::string memory_normalize_claim_key(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pending_space = false;
    for (const char character : text) {
        if (is_claim_space(character)) {
            // A run of whitespace is one separator, and a leading run is
            // nothing at all: the normalized text never starts with a space.
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(character >= 'A' && character <= 'Z'
                                 ? static_cast<char>(character - 'A' + 'a')
                                 : character);
    }
    return normalized;
}

MemoryClaimKey memory_claim_key(std::string_view statement) {
    MemoryClaimKey claim;
    const auto separator = statement.find('=');
    if (separator == std::string_view::npos) {
        // No value: the statement is its own subject.
        claim.key = memory_normalize_claim_key(statement);
        return claim;
    }
    claim.key = memory_normalize_claim_key(statement.substr(0, separator));
    claim.value = memory_normalize_claim_key(statement.substr(separator + 1));
    claim.keyed = true;
    return claim;
}

bool memory_same_claim(std::string_view lhs, std::string_view rhs) {
    const auto left = memory_claim_key(lhs);
    const auto right = memory_claim_key(rhs);
    return left.key == right.key && left.value == right.value;
}

// ---------------------------------------------------------------------------
// Per-kind policy (design Context/Memory §6.2)
// ---------------------------------------------------------------------------

namespace {

constexpr std::chrono::seconds kDay{86'400};

// The per-kind policy table. One entry per MemoryKind, in declaration order.
//
// The values follow the memory layers of design §5 and the reference
// implementation's actual scope use:
//
// - Preference is the semantic layer of a user profile (§5): it outlives a
//   session, so it may live in a user scope and carries the longest default
//   lifetime. A stated preference is its own evidence, so its verification
//   floor is Unverified.
// - EnvironmentFact and ApplicationFact are the semantic facts of an
//   environment and an application. An environment fact is short-lived (a
//   screen layout outlives neither a day nor a rebuild) and must be observed
//   before it may be filed; an application fact is stable but still needs an
//   observation behind it.
// - Episode is the episodic layer (§5): a verified run outcome. It is a
//   canonical structured record, so it is recalled by identifier and keyword,
//   not by embedding similarity.
// - Procedure, RecoveryLesson and SkillHint are the procedural layer (§5):
//   versioned operating knowledge owned by the agent or a task skill. A
//   published procedure is registry-derived rather than run-verified, so its
//   floor is Unverified; a recovery lesson is distilled from a verified
//   failure and requires that verification.
//
// The Exact and Lexical legs are eligible for every kind: the
// conflict-detection pipeline (exact terms inside the proposed kind) and the
// failure-signature retrieval both depend on them, and withholding them
// would silently degrade conflict detection rather than retrieval quality.
// The Semantic leg is the one a kind withholds: a canonical structured
// record is not reachable by embedding similarity.
const std::array<MemoryKindPolicy, 7> &kind_policy_table() {
    static const std::array<MemoryKindPolicy, 7> table = {{
        MemoryKindPolicy{
            MemoryKind::Preference,
            180 * kDay,
            MemoryVerification::Unverified,
            {MemoryScopeKind::User, MemoryScopeKind::Session, MemoryScopeKind::Environment,
             MemoryScopeKind::Application},
            {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical, MemoryRetrievalLeg::Semantic}},
        MemoryKindPolicy{
            MemoryKind::EnvironmentFact,
            7 * kDay,
            MemoryVerification::Observed,
            {MemoryScopeKind::Environment, MemoryScopeKind::Task, MemoryScopeKind::Session,
             MemoryScopeKind::Agent, MemoryScopeKind::TaskSkill},
            {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical, MemoryRetrievalLeg::Semantic}},
        MemoryKindPolicy{
            MemoryKind::ApplicationFact,
            30 * kDay,
            MemoryVerification::Observed,
            {MemoryScopeKind::Application, MemoryScopeKind::Task, MemoryScopeKind::Session,
             MemoryScopeKind::Agent},
            {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical, MemoryRetrievalLeg::Semantic}},
        MemoryKindPolicy{MemoryKind::Episode,
                         30 * kDay,
                         MemoryVerification::Observed,
                         {MemoryScopeKind::Task, MemoryScopeKind::Session,
                          MemoryScopeKind::Environment, MemoryScopeKind::Agent},
                         {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical}},
        MemoryKindPolicy{
            MemoryKind::Procedure,
            180 * kDay,
            MemoryVerification::Unverified,
            {MemoryScopeKind::Agent, MemoryScopeKind::TaskSkill, MemoryScopeKind::Application},
            {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical}},
        MemoryKindPolicy{MemoryKind::RecoveryLesson,
                         90 * kDay,
                         MemoryVerification::Verified,
                         {MemoryScopeKind::Agent, MemoryScopeKind::Task, MemoryScopeKind::Session},
                         {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical}},
        MemoryKindPolicy{
            MemoryKind::SkillHint,
            30 * kDay,
            MemoryVerification::Observed,
            {MemoryScopeKind::TaskSkill, MemoryScopeKind::Task, MemoryScopeKind::Session},
            {MemoryRetrievalLeg::Exact, MemoryRetrievalLeg::Lexical}},
    }};
    return table;
}

} // namespace

const MemoryKindPolicy &memory_kind_policy(MemoryKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    const auto &table = kind_policy_table();
    if (index >= table.size()) {
        return table[static_cast<std::size_t>(MemoryKind::EnvironmentFact)];
    }
    return table[index];
}

Result<void> check_memory_kind_policy(const MemoryRecord &record) {
    const auto &policy = memory_kind_policy(record.kind);
    // Namespaced content carries its own authority contract: the import
    // path caps it below HumanConfirmed and its provenance is the namespace
    // itself (design Context/Memory §19). The per-kind gate binds Mira's
    // own claims, so it does not apply to namespaced records.
    if (record.source_namespace.has_value()) {
        return Result<void>{};
    }
    if (memory_verification_rank(record.verification) <
        memory_verification_rank(policy.min_verification)) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "memory kind requires a higher verification level");
    }
    const auto &allowed = policy.allowed_scope_kinds;
    if (std::find(allowed.begin(), allowed.end(), record.scope.kind) == allowed.end()) {
        return memory_error(MemoryDomainCode::ScopeDenied,
                            "memory kind is not allowed in this scope");
    }
    return Result<void>{};
}

std::optional<std::chrono::system_clock::time_point>
memory_default_expiry(MemoryKind kind, std::chrono::system_clock::time_point recorded_at) {
    const auto &policy = memory_kind_policy(kind);
    if (policy.default_ttl.count() <= 0) {
        return std::nullopt;
    }
    return recorded_at + policy.default_ttl;
}

std::string memory_status_name(MemoryStatus status) {
    switch (status) {
    case MemoryStatus::Active:
        return "active";
    case MemoryStatus::Superseded:
        return "superseded";
    case MemoryStatus::Tombstoned:
        return "tombstoned";
    }
    return "unknown";
}

std::optional<MemoryStatus> memory_status_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryStatus> kStatuses[] = {
        {"active", MemoryStatus::Active},
        {"superseded", MemoryStatus::Superseded},
        {"tombstoned", MemoryStatus::Tombstoned},
    };
    for (const auto &[status_name, status] : kStatuses) {
        if (status_name == name) {
            return status;
        }
    }
    return std::nullopt;
}

std::string memory_mutation_type_name(MemoryMutationType type) {
    switch (type) {
    case MemoryMutationType::Add:
        return "add";
    case MemoryMutationType::Update:
        return "update";
    case MemoryMutationType::Supersede:
        return "supersede";
    case MemoryMutationType::Tombstone:
        return "tombstone";
    case MemoryMutationType::Noop:
        return "noop";
    }
    return "unknown";
}

std::optional<MemoryMutationType> memory_mutation_type_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryMutationType> kTypes[] = {
        {"add", MemoryMutationType::Add},
        {"update", MemoryMutationType::Update},
        {"supersede", MemoryMutationType::Supersede},
        {"tombstone", MemoryMutationType::Tombstone},
        {"noop", MemoryMutationType::Noop},
    };
    for (const auto &[type_name, type] : kTypes) {
        if (type_name == name) {
            return type;
        }
    }
    return std::nullopt;
}

std::string mutation_reason_name(MutationReasonCode reason) {
    switch (reason) {
    case MutationReasonCode::VerifiedEvent:
        return "verified-event";
    case MutationReasonCode::HumanCorrection:
        return "human-correction";
    case MutationReasonCode::Consolidation:
        return "consolidation";
    case MutationReasonCode::RetentionExpiry:
        return "retention-expiry";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void> MemoryRecord::validate() const {
    const auto schema = validate_schema_version(schema_version, memory_schema_current());
    if (!schema) {
        return schema.error();
    }
    if (id.is_nil()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record id must be non-nil");
    }
    if (statement.empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record requires a statement");
    }
    if (!(confidence >= 0.0F && confidence <= 1.0F) || std::isnan(confidence)) {
        return memory_error(MemoryDomainCode::InvalidRecord, "confidence must be within [0,1]");
    }
    // Secrets never enter long-term memory (design Context/Memory §13.2).
    if (sensitivity == Sensitivity::Secret) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "secret content must not enter memory");
    }
    // Imported records carry their namespace and stay below HumanConfirmed;
    // home-grown HumanConfirmed records always trace to events.
    if (source_namespace.has_value() && verification == MemoryVerification::HumanConfirmed) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "imported memory cannot claim human confirmation");
    }
    if (!source_namespace.has_value() && verification == MemoryVerification::HumanConfirmed &&
        provenance.empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "human-confirmed memory requires event provenance");
    }
    if (provenance.empty() && confidence > 0.3F) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "untraceable memory must keep low confidence");
    }
    if (validity.valid_until.has_value() && *validity.valid_until < validity.valid_from) {
        return memory_error(MemoryDomainCode::InvalidRecord, "validity interval is inverted");
    }
    if (expires_at.has_value() && *expires_at < recorded_at) {
        return memory_error(MemoryDomainCode::InvalidRecord, "expiry precedes recording time");
    }
    if (status == MemoryStatus::Superseded && !supersedes.has_value()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "superseded records must reference their replacement");
    }
    if (version == 0) {
        return memory_error(MemoryDomainCode::InvalidRecord, "record version starts at one");
    }
    return Result<void>{};
}

Result<void> MemoryQuery::validate() const {
    if (scopes.empty()) {
        return memory_error(MemoryDomainCode::ScopeDenied,
                            "memory queries must state an explicit scope allowlist");
    }
    for (const auto &scope : scopes) {
        if (scope.subject_id.empty() && scope.kind != MemoryScopeKind::Environment &&
            scope.kind != MemoryScopeKind::Agent) {
            return memory_error(MemoryDomainCode::ScopeDenied,
                                "scoped memory queries require a subject id");
        }
        if (scope.subject_id.size() > 256) {
            return memory_error(MemoryDomainCode::ScopeDenied, "scope subject id is too long");
        }
        if (scope.tenant_id.has_value() && scope.tenant_id->size() > 256) {
            return memory_error(MemoryDomainCode::ScopeDenied, "scope tenant id is too long");
        }
    }
    if (kinds.has_value() && kinds->empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "kind filter must not be empty");
    }
    if (max_results == 0 || max_results > 512) {
        return memory_error(MemoryDomainCode::InvalidRecord, "max_results must be within (0, 512]");
    }
    if (deadline.count() < 0) {
        return memory_error(MemoryDomainCode::InvalidRecord, "deadline must not be negative");
    }
    if (query_embedding.size() > 4'096) {
        return memory_error(MemoryDomainCode::InvalidRecord, "query embedding exceeds bounds");
    }
    return Result<void>{};
}

std::string erasure_status_name(ErasureStatus status) {
    switch (status) {
    case ErasureStatus::Complete:
        return "Complete";
    case ErasureStatus::Pending:
        return "Pending";
    }
    return "Unknown";
}

Result<void> ErasureRequest::validate() const {
    const bool scope_set = scope.has_value();
    const bool record_set = record.has_value() && !record->is_nil();
    if (scope_set == record_set) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "erasure must target exactly one scope or record");
    }
    if (reason.empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "erasure requires a reason");
    }
    return Result<void>{};
}

Result<void> MemoryMutation::validate() const {
    const auto proposal = proposed.validate();
    if (!proposal) {
        return proposal.error();
    }
    if (id.is_nil()) {
        return memory_error(MemoryDomainCode::InvalidMutation, "mutation id must be non-nil");
    }
    if (proposed.scope != scope) {
        return memory_error(MemoryDomainCode::InvalidMutation,
                            "mutation scope must match the proposed record scope");
    }
    switch (type) {
    case MemoryMutationType::Add:
        if (target.has_value()) {
            return memory_error(MemoryDomainCode::InvalidMutation, "add must not target a record");
        }
        break;
    case MemoryMutationType::Update:
    case MemoryMutationType::Supersede:
    case MemoryMutationType::Tombstone:
        if (!target.has_value() || target->is_nil()) {
            return memory_error(MemoryDomainCode::InvalidMutation,
                                "mutation type requires a target record");
        }
        if (!expected_version.has_value()) {
            return memory_error(MemoryDomainCode::InvalidMutation,
                                "mutation type requires an expected version");
        }
        break;
    case MemoryMutationType::Noop:
        return Result<void>{};
    }
    if (type == MemoryMutationType::Supersede && !proposed.supersedes.has_value()) {
        return memory_error(MemoryDomainCode::InvalidMutation,
                            "supersede proposals must name the replaced record");
    }
    // The per-kind policy gates claims, not retirements: a Tombstone states
    // the record as it is, so it must stay applicable to a record written
    // before the policy existed (design Context/Memory §6.2, §13.4).
    if (type != MemoryMutationType::Tombstone) {
        const auto policy = check_memory_kind_policy(proposed);
        if (!policy) {
            return policy.error();
        }
    }
    if (evidence.empty() && reason != MutationReasonCode::RetentionExpiry) {
        return memory_error(MemoryDomainCode::InvalidMutation, "mutations require event evidence");
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Error domain
// ---------------------------------------------------------------------------

std::string memory_domain_code_name(MemoryDomainCode code) {
    switch (code) {
    case MemoryDomainCode::InvalidRecord:
        return "InvalidRecord";
    case MemoryDomainCode::InvalidMutation:
        return "InvalidMutation";
    case MemoryDomainCode::ScopeDenied:
        return "ScopeDenied";
    case MemoryDomainCode::VersionConflict:
        return "VersionConflict";
    case MemoryDomainCode::SchemaUnsupported:
        return "SchemaUnsupported";
    case MemoryDomainCode::IndexDegraded:
        return "IndexDegraded";
    case MemoryDomainCode::ErasurePending:
        return "ErasurePending";
    case MemoryDomainCode::StoreUnavailable:
        return "StoreUnavailable";
    }
    return "Unknown";
}

Error make_memory_error(MemoryDomainCode code, std::string safe_message, bool retryable,
                        std::optional<OperationId> operation) {
    Error error;
    error.domain = "mira.memory";
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = retryable;
    error.safe_message = std::move(safe_message);
    error.operation_id = operation;
    switch (code) {
    case MemoryDomainCode::InvalidRecord:
    case MemoryDomainCode::InvalidMutation:
    case MemoryDomainCode::SchemaUnsupported:
        error.code = ErrorCode::InvalidArgument;
        break;
    case MemoryDomainCode::ScopeDenied:
        error.code = ErrorCode::PermissionDenied;
        break;
    case MemoryDomainCode::VersionConflict:
        error.code = ErrorCode::InvalidState;
        break;
    case MemoryDomainCode::IndexDegraded:
        error.code = ErrorCode::Internal;
        error.retryable = true;
        break;
    case MemoryDomainCode::ErasurePending:
        error.code = ErrorCode::InvalidState;
        error.retryable = true;
        break;
    case MemoryDomainCode::StoreUnavailable:
        error.code = ErrorCode::Unavailable;
        error.retryable = true;
        break;
    }
    error.retryable = error.retryable || retryable;
    return error;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

JsonValue memory_scope_to_json(const MemoryScope &scope) {
    JsonValue::Object object;
    object.emplace_back("kind", memory_scope_kind_name(scope.kind));
    object.emplace_back("subject_id", scope.subject_id);
    if (scope.tenant_id.has_value()) {
        object.emplace_back("tenant_id", *scope.tenant_id);
    }
    return JsonValue(std::move(object));
}

Result<MemoryScope> memory_scope_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory scope must be an object");
    }
    MemoryScope scope;
    const auto *kind = json.find("kind");
    if (kind != nullptr && kind->is_string()) {
        const auto parsed = memory_scope_kind_from(*kind->as_string());
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord, "memory scope kind is unknown");
        }
        scope.kind = *parsed;
    }
    const auto *subject = json.find("subject_id");
    if (subject != nullptr && subject->is_string()) {
        scope.subject_id = *subject->as_string();
    }
    const auto *tenant = json.find("tenant_id");
    if (tenant != nullptr && tenant->is_string()) {
        scope.tenant_id = *tenant->as_string();
    }
    return scope;
}

JsonValue memory_record_to_json(const MemoryRecord &record) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(memory_schema_current().major)},
                          {"minor", static_cast<std::int64_t>(memory_schema_current().minor)}});
    object.emplace_back("id", record.id.to_string());
    object.emplace_back("scope", memory_scope_to_json(record.scope));
    object.emplace_back("kind", memory_kind_name(record.kind));
    object.emplace_back("statement", record.statement);
    if (record.evidence.has_value()) {
        object.emplace_back("evidence", artifact_ref_to_json(*record.evidence));
    }
    JsonValue::Object validity;
    validity.emplace_back("valid_from", wall_nanos(record.validity.valid_from));
    if (record.validity.valid_until.has_value()) {
        validity.emplace_back("valid_until", wall_nanos(*record.validity.valid_until));
    }
    object.emplace_back("validity", JsonValue(std::move(validity)));
    object.emplace_back("recorded_at", wall_nanos(record.recorded_at));
    JsonValue::Array provenance;
    for (const auto &event : record.provenance) {
        provenance.emplace_back(event.to_string());
    }
    object.emplace_back("provenance", JsonValue(std::move(provenance)));
    object.emplace_back("verification", memory_verification_name(record.verification));
    object.emplace_back("confidence", static_cast<double>(record.confidence));
    object.emplace_back("sensitivity", sensitivity_name(record.sensitivity));
    object.emplace_back("status", memory_status_name(record.status));
    if (record.supersedes.has_value()) {
        object.emplace_back("supersedes", record.supersedes->to_string());
    }
    if (record.expires_at.has_value()) {
        object.emplace_back("expires_at", wall_nanos(*record.expires_at));
    }
    if (record.source_namespace.has_value()) {
        object.emplace_back("source_namespace", *record.source_namespace);
    }
    object.emplace_back("version", static_cast<std::int64_t>(record.version));
    return JsonValue(std::move(object));
}

Result<MemoryRecord> memory_record_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::SchemaUnsupported, "memory record must be an object");
    }
    MemoryRecord record;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto *major = schema->find("major");
        const auto *minor = schema->find("minor");
        if (major != nullptr && major->is_integer() && minor != nullptr && minor->is_integer()) {
            record.schema_version =
                SchemaVersion{static_cast<std::uint16_t>(major->as_integer().value()),
                              static_cast<std::uint16_t>(minor->as_integer().value())};
        }
        const auto supported =
            validate_schema_version(record.schema_version, memory_schema_current());
        if (!supported) {
            return supported.error();
        }
    }
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record requires an id");
    }
    const auto parsed_id = MemoryId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record id is malformed");
    }
    record.id = *parsed_id;
    const auto *scope = json.find("scope");
    if (scope != nullptr) {
        auto parsed = memory_scope_from_json(*scope);
        if (!parsed) {
            return parsed.error();
        }
        record.scope = std::move(parsed).value();
    }
    if (auto kind = enum_from_json<MemoryKind>(json, "kind", memory_kind_from)) {
        record.kind = *kind;
    }
    const auto *statement = json.find("statement");
    if (statement != nullptr && statement->is_string()) {
        record.statement = *statement->as_string();
    }
    const auto *evidence = json.find("evidence");
    if (evidence != nullptr && !evidence->is_null()) {
        auto parsed = artifact_ref_from(*evidence);
        if (!parsed) {
            return parsed.error();
        }
        record.evidence = std::move(parsed).value();
    }
    const auto *validity = json.find("validity");
    if (validity != nullptr && validity->is_object()) {
        const auto *from = validity->find("valid_from");
        if (from != nullptr && from->is_integer()) {
            record.validity.valid_from = wall_from_nanos(from->as_integer().value());
        }
        const auto *until = validity->find("valid_until");
        if (until != nullptr && until->is_integer()) {
            record.validity.valid_until = wall_from_nanos(until->as_integer().value());
        }
    }
    const auto *recorded = json.find("recorded_at");
    if (recorded != nullptr && recorded->is_integer()) {
        record.recorded_at = wall_from_nanos(recorded->as_integer().value());
    }
    const auto *provenance = json.find("provenance");
    if (provenance != nullptr && provenance->is_array()) {
        for (const auto &event : *provenance->as_array()) {
            if (auto parsed = EventId::parse(event.as_string() ? *event.as_string() : ""); parsed) {
                record.provenance.emplace_back(*parsed);
            }
        }
    }
    if (auto verification =
            enum_from_json<MemoryVerification>(json, "verification", memory_verification_from)) {
        record.verification = *verification;
    }
    const auto *confidence = json.find("confidence");
    if (confidence != nullptr && confidence->is_number()) {
        const double value = confidence->as_number().value();
        if (value >= 0.0 && value <= 1.0) {
            record.confidence = static_cast<float>(value);
        }
    }
    const auto *sensitivity = json.find("sensitivity");
    if (sensitivity != nullptr && sensitivity->is_string()) {
        const auto parsed = sensitivity_from(*sensitivity);
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "memory record has unknown sensitivity");
        }
        record.sensitivity = *parsed;
    }
    if (auto status = enum_from_json<MemoryStatus>(json, "status", memory_status_from)) {
        record.status = *status;
    }
    const auto *supersedes = json.find("supersedes");
    if (supersedes != nullptr && supersedes->is_string()) {
        if (auto parsed = MemoryId::parse(*supersedes->as_string()); parsed) {
            record.supersedes = *parsed;
        }
    }
    const auto *expires = json.find("expires_at");
    if (expires != nullptr && expires->is_integer()) {
        record.expires_at = wall_from_nanos(expires->as_integer().value());
    }
    const auto *namespace_field = json.find("source_namespace");
    if (namespace_field != nullptr && namespace_field->is_string()) {
        record.source_namespace = *namespace_field->as_string();
    }
    const auto *version = json.find("version");
    if (version != nullptr && version->is_integer() && version->as_integer().value() > 0) {
        record.version = static_cast<std::uint64_t>(version->as_integer().value());
    }
    return record;
}

JsonValue memory_mutation_to_json(const MemoryMutation &mutation) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(memory_schema_current().major)},
                          {"minor", static_cast<std::int64_t>(memory_schema_current().minor)}});
    object.emplace_back("id", mutation.id.to_string());
    object.emplace_back("type", memory_mutation_type_name(mutation.type));
    object.emplace_back("scope", memory_scope_to_json(mutation.scope));
    if (mutation.target.has_value()) {
        object.emplace_back("target", mutation.target->to_string());
    }
    if (mutation.expected_version.has_value()) {
        object.emplace_back("expected_version",
                            static_cast<std::int64_t>(*mutation.expected_version));
    }
    object.emplace_back("proposed", memory_record_to_json(mutation.proposed));
    JsonValue::Array evidence;
    for (const auto &event : mutation.evidence) {
        evidence.emplace_back(event.to_string());
    }
    object.emplace_back("evidence", JsonValue(std::move(evidence)));
    object.emplace_back("reason", mutation_reason_name(mutation.reason));
    return JsonValue(std::move(object));
}

Result<MemoryMutation> memory_mutation_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::SchemaUnsupported,
                            "memory mutation must be an object");
    }
    MemoryMutation mutation;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidMutation, "memory mutation requires an id");
    }
    const auto parsed_id = MutationId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidMutation, "memory mutation id is malformed");
    }
    mutation.id = *parsed_id;
    if (auto type = enum_from_json<MemoryMutationType>(json, "type", memory_mutation_type_from)) {
        mutation.type = *type;
    }
    const auto *scope = json.find("scope");
    if (scope != nullptr) {
        auto parsed = memory_scope_from_json(*scope);
        if (!parsed) {
            return parsed.error();
        }
        mutation.scope = std::move(parsed).value();
    }
    const auto *target = json.find("target");
    if (target != nullptr && target->is_string()) {
        if (auto parsed = MemoryId::parse(*target->as_string()); parsed) {
            mutation.target = *parsed;
        }
    }
    const auto *version = json.find("expected_version");
    if (version != nullptr && version->is_integer()) {
        mutation.expected_version = static_cast<std::uint64_t>(version->as_integer().value());
    }
    const auto *proposed = json.find("proposed");
    if (proposed != nullptr) {
        auto parsed = memory_record_from_json(*proposed);
        if (!parsed) {
            return parsed.error();
        }
        mutation.proposed = std::move(parsed).value();
    }
    const auto *evidence = json.find("evidence");
    if (evidence != nullptr && evidence->is_array()) {
        for (const auto &event : *evidence->as_array()) {
            if (auto parsed = EventId::parse(event.as_string() ? *event.as_string() : ""); parsed) {
                mutation.evidence.emplace_back(*parsed);
            }
        }
    }
    const auto *reason = json.find("reason");
    if (reason != nullptr && reason->is_string()) {
        const std::string_view name = *reason->as_string();
        if (name == "verified-event") {
            mutation.reason = MutationReasonCode::VerifiedEvent;
        } else if (name == "human-correction") {
            mutation.reason = MutationReasonCode::HumanCorrection;
        } else if (name == "consolidation") {
            mutation.reason = MutationReasonCode::Consolidation;
        } else if (name == "retention-expiry") {
            mutation.reason = MutationReasonCode::RetentionExpiry;
        }
    }
    return mutation;
}

} // namespace mira
