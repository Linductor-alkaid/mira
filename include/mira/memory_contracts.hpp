#pragma once

#include <mira/artifact_store.hpp>
#include <mira/context_contracts.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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
    std::string note;
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
    [[nodiscard]] virtual Result<std::optional<MemoryRecord>> get(MemoryId record) const = 0;
    // Idempotent by mutation id; optimistic version conflicts return
    // VersionConflict instead of silently overwriting.
    [[nodiscard]] virtual Result<MemoryMutationResult>
    apply(const MemoryMutation &mutation) = 0;
    // Retention sweep: purge expired records, prune old versions and stale
    // tombstones. Artifacts referenced by purged records are erased too.
    [[nodiscard]] virtual Result<MemoryCompactionResult>
    compact(const MemoryScope &scope) = 0;
    // Privacy erasure. Partial failure keeps the request Pending and holds the
    // scope out of Context (query/apply reject for held scopes) until a retry
    // completes; the audit trail never records the erased statements.
    [[nodiscard]] virtual Result<ErasureResult> erase(const ErasureRequest &request) = 0;
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
