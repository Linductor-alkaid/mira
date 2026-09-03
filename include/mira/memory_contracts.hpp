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
// Stable error domain (schema "mira.memory.error.v1")
// ---------------------------------------------------------------------------

enum class MemoryDomainCode : std::int32_t {
    InvalidRecord = 1,
    InvalidMutation = 2,
    ScopeDenied = 3,
    VersionConflict = 4,
    SchemaUnsupported = 5,
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
