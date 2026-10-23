#pragma once

#include <mira/artifact_store.hpp>
#include <mira/context_contracts.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Versioned long-term memory contracts (M4-01; durable backends are M4-09+)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr SchemaVersion memory_schema_current() noexcept { return {1, 0}; }

enum class MemoryScopeKind : std::uint8_t {
    Task,
    Session,
    Environment,
    Application,
    User,
    Agent,
    TaskSkill,
};

[[nodiscard]] std::string memory_scope_kind_name(MemoryScopeKind kind);
[[nodiscard]] std::optional<MemoryScopeKind> memory_scope_kind_from(std::string_view name);

// Access-control filter, not a ranking feature: retrieval never crosses
// scopes by similarity (design Context/Memory §6.1).
struct MemoryScope final {
    MemoryScopeKind kind = MemoryScopeKind::Environment;
    std::string subject_id;
    std::optional<std::string> tenant_id;
    friend bool operator==(const MemoryScope &, const MemoryScope &) = default;
};

enum class MemoryKind : std::uint8_t {
    Preference,
    EnvironmentFact,
    ApplicationFact,
    Episode,
    Procedure,
    RecoveryLesson,
    SkillHint,
};

[[nodiscard]] std::string memory_kind_name(MemoryKind kind);
[[nodiscard]] std::optional<MemoryKind> memory_kind_from(std::string_view name);

enum class MemoryVerification : std::uint8_t {
    Unverified,
    Observed,
    Verified,
    HumanConfirmed,
};

[[nodiscard]] std::string memory_verification_name(MemoryVerification verification);
[[nodiscard]] std::optional<MemoryVerification> memory_verification_from(std::string_view name);

// Verification levels are ordered: the rank drives the per-kind verification
// floor and the ranking input of the retrieval legs.
[[nodiscard]] int memory_verification_rank(MemoryVerification verification) noexcept;

// ---------------------------------------------------------------------------
// Claim key (design Context/Memory §13.5)
// ---------------------------------------------------------------------------

// The normalized identity of a claim: what the statement is *about*.
//
// A statement of the form `subject=value` is split at the first `=`; the
// subject becomes the key and the remainder the value. A statement without
// `=` is its own key and carries no value. Both halves are normalized, so
// "volume=30", "volume = 30" and "VOLUME=30" are one claim, while
// "volume=30" and "volume_level=high" are two.
//
// The rule is lexical by design: it never infers that two differently
// worded statements mean the same thing (design Context/Memory §23 — Core
// holds no inference).
struct MemoryClaimKey final {
    // The subject of the claim, normalized.
    std::string key;
    // The asserted value, normalized. Empty when the statement carried no
    // `=` and therefore no value.
    std::string value;
    // True when the statement carried the `subject=value` shape.
    bool keyed = false;
};

// Normalizes a claim subject: trims the ends, collapses internal whitespace
// runs to a single space and folds ASCII case. The normalization is
// idempotent, so a backend may apply it to a key it received.
[[nodiscard]] std::string memory_normalize_claim_key(std::string_view text);

// Splits a statement into its claim key and value.
[[nodiscard]] MemoryClaimKey memory_claim_key(std::string_view statement);

// Whether two statements make the same claim: same subject, same value.
[[nodiscard]] bool memory_same_claim(std::string_view lhs, std::string_view rhs);

// ---------------------------------------------------------------------------
// Per-kind policy (design Context/Memory §6.2)
// ---------------------------------------------------------------------------

// The retrieval legs a MemoryKind answers to. The store runs a leg only when
// the queried kinds declare it eligible.
enum class MemoryRetrievalLeg : std::uint8_t {
    Exact,    // verbatim identifier / substring matching
    Lexical,  // FTS5 keyword recall
    Semantic, // embedding cosine recall
};

// The policy a MemoryKind carries: how long a claim of that kind lives, the
// lowest verification level it may claim, the scope kinds it may be stored
// in, and the retrieval legs it answers to (design Context/Memory §6.2).
struct MemoryKindPolicy final {
    MemoryKind kind = MemoryKind::EnvironmentFact;
    // Default lifetime when the writer states no expiry of its own. Zero
    // means the kind never expires on its own.
    std::chrono::seconds default_ttl{0};
    // Lowest verification level a record of this kind may claim.
    MemoryVerification min_verification = MemoryVerification::Unverified;
    // Scope kinds this kind may be stored in.
    std::vector<MemoryScopeKind> allowed_scope_kinds;
    // Retrieval legs appropriate for this kind.
    std::vector<MemoryRetrievalLeg> retrieval_legs;
};

// The single per-kind policy table: one entry per MemoryKind. The returned
// reference stays valid for the lifetime of the program.
[[nodiscard]] const MemoryKindPolicy &memory_kind_policy(MemoryKind kind);

enum class MemoryStatus : std::uint8_t {
    Active,
    Superseded,
    Tombstoned,
};

[[nodiscard]] std::string memory_status_name(MemoryStatus status);
[[nodiscard]] std::optional<MemoryStatus> memory_status_from(std::string_view name);

// Bitemporal validity: when the fact held in the environment versus when Mira
// learned about it (design Context/Memory §7.5).
struct ValidityInterval final {
    std::chrono::system_clock::time_point valid_from{};
    std::optional<std::chrono::system_clock::time_point> valid_until;
    friend bool operator==(const ValidityInterval &, const ValidityInterval &) = default;
};

struct MemoryRecord final {
    MemoryId id;
    MemoryScope scope;
    MemoryKind kind = MemoryKind::EnvironmentFact;
    // The remembered statement is data, never a system instruction; payload
    // bytes stay behind an ArtifactRef.
    std::string statement;
    std::optional<ArtifactRef> evidence;
    ValidityInterval validity;
    std::chrono::system_clock::time_point recorded_at{};
    std::vector<EventId> provenance;
    MemoryVerification verification = MemoryVerification::Unverified;
    float confidence = 0.0F;
    Sensitivity sensitivity = Sensitivity::Internal;
    MemoryStatus status = MemoryStatus::Active;
    std::optional<MemoryId> supersedes;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    // External imports must carry their namespace and cannot claim
    // HumanConfirmed verification (design Context/Memory §19).
    std::optional<std::string> source_namespace;
    SchemaVersion schema_version = memory_schema_current();
    // Store-side optimistic-concurrency version; backends set it on read and
    // check it on Update/Supersede/Tombstone mutations.
    std::uint64_t version = 1;

    [[nodiscard]] Result<void> validate() const;
};

// Write-time gate for the per-kind policy: the verification floor and the
// scope allowlist (design Context/Memory §6.2). A record that carries a
// source namespace is governed by the import contract instead (design
// Context/Memory §19), so the gate does not apply to it.
[[nodiscard]] Result<void> check_memory_kind_policy(const MemoryRecord &record);

// Default expiry of a claim of this kind recorded at `recorded_at`.
// Nullopt when the kind carries no default lifetime.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
memory_default_expiry(MemoryKind kind, std::chrono::system_clock::time_point recorded_at);

enum class MemoryMutationType : std::uint8_t {
    Add,
    Update,
    Supersede,
    Tombstone,
    Noop,
};

[[nodiscard]] std::string memory_mutation_type_name(MemoryMutationType type);
[[nodiscard]] std::optional<MemoryMutationType> memory_mutation_type_from(std::string_view name);

enum class MutationReasonCode : std::uint8_t {
    VerifiedEvent,
    HumanCorrection,
    Consolidation,
    RetentionExpiry,
};

[[nodiscard]] std::string mutation_reason_name(MutationReasonCode reason);

struct MemoryMutation final {
    MutationId id;
    MemoryMutationType type = MemoryMutationType::Add;
    MemoryScope scope;
    std::optional<MemoryId> target;
    // Optimistic concurrency: the version the planner observed.
    std::optional<std::uint64_t> expected_version;
    MemoryRecord proposed;
    std::vector<EventId> evidence;
    MutationReasonCode reason = MutationReasonCode::VerifiedEvent;

    [[nodiscard]] Result<void> validate() const;
};

struct MemoryMutationResult final {
    MemoryMutationType applied = MemoryMutationType::Noop;
    MemoryId record;
    std::uint64_t new_version = 1;
    // True when the mutation id was replayed against an already-applied write.
    bool idempotent_replay = false;
};

// ---------------------------------------------------------------------------
// Retrieval contracts (M4-10/M4-11)
// ---------------------------------------------------------------------------

// Which retrieval legs ran and how they degraded. Quality is reported so the
// Reasoner can continue, ask a human, or fail — never silently.
struct MemoryQueryQuality final {
    bool exact_leg_ran = false;
    bool fts_leg_ran = false;
    bool vector_leg_ran = false;
    // The per-kind retrieval policy withheld the semantic leg: the queried
    // kinds do not declare embedding similarity eligible (design
    // Context/Memory §6.2). Not a degradation — the leg was never eligible.
    bool semantic_leg_withheld = false;
    // Active records without a usable embedding; the vector index may lag the
    // authoritative records (it is a rebuildable projection).
    std::size_t index_lag = 0;
    bool vector_degraded = false;
    bool deadline_exceeded = false;
    bool degraded = false;
    std::string note;
};

struct MemoryQuery final {
    // Mandatory ACL allowlist: results never include records outside these
    // exact scopes, regardless of similarity (design Context/Memory §6.1/§14.1).
    std::vector<MemoryScope> scopes;
    std::optional<std::vector<MemoryKind>> kinds;
    // Free text feeding the FTS5 leg (phrase-quoted) and ranking.
    std::string text;
    // Substrings that must appear verbatim in the statement (exact leg).
    std::vector<std::string> exact_terms;
    // Only records whose claim key equals this subject participate. A hard
    // filter, like the scope allowlist: it narrows the candidate set before
    // ranking, so the conflict retrieval of design Context/Memory §13.5
    // sees every record about one subject instead of the whole scope.
    // The value is a claim subject, normalized by
    // memory_normalize_claim_key().
    std::optional<std::string> claim_key;
    // Optional embedding for the bounded cosine leg; dimension must match the
    // indexed vectors or the leg degrades to exact/FTS.
    std::vector<float> query_embedding;
    std::size_t max_results = 32;
    // Conservative token packing budget for the selected statements.
    std::uint64_t token_budget = 4'096;
    std::optional<Sensitivity> max_sensitivity;
    // Bitemporal query times; nullopt means "now" (M4-15).
    std::optional<std::chrono::system_clock::time_point> as_of_recorded;
    std::optional<std::chrono::system_clock::time_point> as_of_valid;
    // Soft deadline for the retrieval phases; exceeding it returns partial
    // results with quality flags instead of blocking the control plane.
    std::chrono::milliseconds deadline{2'000};

    [[nodiscard]] Result<void> validate() const;
};

struct MemoryQueryResult final {
    std::vector<MemoryRecord> records;
    std::vector<double> scores;
    MemoryQueryQuality quality;
    std::uint64_t tokens_estimate = 0;
};

// ---------------------------------------------------------------------------
// Erasure and retention contracts (M4-13)
// ---------------------------------------------------------------------------

enum class ErasureStatus : std::uint8_t {
    Complete, // payload, FTS, embeddings, versions and artifact refs all gone
    Pending,  // partial failure; the scope is held out of Context until retry
};

[[nodiscard]] std::string erasure_status_name(ErasureStatus status);

struct ErasureCounts final {
    std::size_t records_removed = 0;
    std::size_t versions_removed = 0;
    std::size_t fts_entries_removed = 0;
    std::size_t embeddings_removed = 0;
    std::size_t artifacts_erased = 0;
};

struct ErasureRequest final {
    // Exactly one of scope or record is honored; a scope erasure removes every
    // record inside it regardless of status.
    std::optional<MemoryScope> scope;
    std::optional<MemoryId> record;
    std::string reason;
    // Also erase referenced evidence artifacts when an ArtifactStore is bound.
    bool include_artifacts = true;

    [[nodiscard]] Result<void> validate() const;
};

struct ErasureResult final {
    ErasureStatus status = ErasureStatus::Complete;
    ErasureCounts counts;
    // Present when Pending: which scope is held out of Context until retry.
    std::optional<MemoryScope> held_scope;
    std::string note;
};

struct MemoryCompactionResult final {
    // Retention expiry purges (payload/FTS/embeddings/artifacts included).
    std::size_t expired_records_purged = 0;
    // Historical versions pruned beyond the per-record keep window.
    std::size_t versions_pruned = 0;
    // Tombstones older than the purge horizon.
    std::size_t tombstones_purged = 0;
    // Erasures that failed and were rolled back. A failed purge is
    // visible, never a silent skip; the record stays whole and the next
    // pass retries it.
    std::size_t erasures_failed = 0;
    // Distinct scopes this sweep acted on.
    std::size_t scopes_swept = 0;
    // A bounded pass stopped at its bound and more work remains: the
    // caller runs another pass.
    bool more_pending = false;
    std::string note;
};

// Bounds for one cross-scope retention pass. A pass is bounded so a large
// store is swept over several passes instead of one long transaction that
// would outrun the store's operation timeout.
struct MemorySweepOptions final {
    // Maximum records the pass purges (expired records and tombstones).
    std::size_t max_records = 512;
    // Maximum version rows the pass prunes.
    std::size_t max_versions = 4'096;
    // Soft deadline for the pass; the pass stops early and reports
    // more_pending.
    std::chrono::milliseconds deadline{30'000};

    [[nodiscard]] Result<void> validate() const;
};

// ---------------------------------------------------------------------------
// IMemory (design Context/Memory §8.3)
// ---------------------------------------------------------------------------

// Durable long-term memory surface. Mirroring ICheckpointStore, operations
// are synchronous facades; implementations run their work on Executor-managed
// store workers (M4-16 supervises routing and shutdown).
class IMemory {
  public:
    virtual ~IMemory() = default;

    // Scope-filtered hybrid retrieval; never crosses the query scopes.
    [[nodiscard]] virtual Result<MemoryQueryResult> query(const MemoryQuery &query) const = 0;
    // Reads one record under the caller's declared scope. A record outside
    // that scope is indistinguishable from a record that does not exist and
    // yields an empty optional, so a by-id read never becomes an existence
    // or version oracle across scopes (design Context/Memory §8.3).
    //
    // Lifecycle is not filtered: Tombstone means "no longer retrieved", not
    // "deleted" (design §13.4 — privacy erasure has its own physical deletion
    // path, and Tombstone must not be used to fake one). A tombstoned,
    // superseded or expired row therefore stays readable by id inside its own
    // scope, for audit and retention work.
    [[nodiscard]] virtual Result<std::optional<MemoryRecord>>
    get(MemoryId record, const MemoryScope &scope) const = 0;
    // Idempotent by mutation id; optimistic version conflicts return
    // VersionConflict instead of silently overwriting.
    [[nodiscard]] virtual Result<MemoryMutationResult> apply(const MemoryMutation &mutation) = 0;
    // Retention sweep: purge expired records, prune old versions and stale
    // tombstones. Artifacts referenced by purged records are erased too.
    [[nodiscard]] virtual Result<MemoryCompactionResult> compact(const MemoryScope &scope) = 0;
    // Privacy erasure. Partial failure keeps the request Pending and holds the
    // scope out of Context (query/apply reject for held scopes) until a retry
    // completes; the audit trail never records the erased statements.
    [[nodiscard]] virtual Result<ErasureResult> erase(const ErasureRequest &request) = 0;
};

// The store owner's retention surface: one bounded pass over every scope.
//
// It is deliberately not part of IMemory: every IMemory method is bound to
// one scope, and a sweep that spans all scopes is the store owner's
// operation, not a tenant-scoped one (design Context/Memory §16.4).
// A durable store that has no global retention need not implement it.
class IMemoryRetentionSweep {
  public:
    virtual ~IMemoryRetentionSweep() = default;

    // One bounded pass: expired records, version history and stale
    // tombstones are reclaimed across every scope. The caller repeats the
    // pass while the result reports more_pending.
    [[nodiscard]] virtual Result<MemoryCompactionResult> sweep(MemorySweepOptions options) = 0;
};

// ---------------------------------------------------------------------------
// Stable error domain (schema "mira.memory.error.v1")
// ---------------------------------------------------------------------------

enum class MemoryDomainCode : std::int32_t {
    InvalidRecord = 1,
    InvalidMutation = 2,
    ScopeDenied = 3,
    VersionConflict = 4,
    SchemaUnsupported = 5,
    IndexDegraded = 6,
    ErasurePending = 7,
    StoreUnavailable = 8,
};

[[nodiscard]] std::string memory_domain_code_name(MemoryDomainCode code);
[[nodiscard]] Error make_memory_error(MemoryDomainCode code, std::string safe_message,
                                      bool retryable = false,
                                      std::optional<OperationId> operation = std::nullopt);

// ---------------------------------------------------------------------------
// Versioned JSON serialization (schema "mira.memory.record.v1" and
// "mira.memory.mutation.v1"); readers ignore unknown members and reject
// unsupported schema majors.
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue memory_scope_to_json(const MemoryScope &scope);
[[nodiscard]] Result<MemoryScope> memory_scope_from_json(const JsonValue &json);
[[nodiscard]] JsonValue memory_record_to_json(const MemoryRecord &record);
[[nodiscard]] Result<MemoryRecord> memory_record_from_json(const JsonValue &json);
[[nodiscard]] JsonValue memory_mutation_to_json(const MemoryMutation &mutation);
[[nodiscard]] Result<MemoryMutation> memory_mutation_from_json(const JsonValue &json);

} // namespace mira
