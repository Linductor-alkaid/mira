#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/workflow_events.hpp>
#include <mira/workflow_ir.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Memory four-domain organization (DEC-029 §1, stage F): a deterministic
// total mapping from the M4 MemoryKind set onto the DEC-014 §10 domains. An
// organizational view, not a storage layer: no new tables, no IMemory changes,
// no scope/ACL or approval rule moves. Domains never carry authorization
// (RULE-09): query scopes stay the first gate.
// ---------------------------------------------------------------------------

enum class MemoryDomain : std::uint8_t {
    EnvironmentModel, // EnvironmentFact + ApplicationFact (graph carrier: AppModel)
    UserModel,        // Preference (human-approval defaults unchanged)
    ProceduralMemory, // Procedure + SkillHint + RecoveryLesson (assets: workflow library)
    EpisodicMemory,   // Episode
};

[[nodiscard]] std::string memory_domain_name(MemoryDomain domain);
[[nodiscard]] Result<MemoryDomain> parse_memory_domain(std::string_view name);

// Total function: every MemoryKind maps to exactly one domain, line by line
// with DEC-014 §10.2.
[[nodiscard]] MemoryDomain memory_domain_of(MemoryKind kind);

// Inverse view: the complete MemoryKind set of one domain, for MemoryQuery
// kind filters. Order follows the MemoryKind declaration order.
[[nodiscard]] std::vector<MemoryKind> memory_kinds_of_domain(MemoryDomain domain);

// ---------------------------------------------------------------------------
// Learning contracts (DEC-029 §2-§5, stage F)
// ---------------------------------------------------------------------------

// Deterministic decode/validation error codes (domain "mira.workflow").
enum class WorkflowLearningError : std::int32_t {
    UnknownField = 1,    // payload object carries a member outside its closed key set
    VersionMismatch = 2, // unsupported schema_version
    LimitExceeded = 3,   // RULE-08 document/id/reason/action limits
    InvalidShape = 4,    // empty ids, malformed digests, unknown enum names, bad numbers
};

[[nodiscard]] Error make_workflow_learning_error(WorkflowLearningError code, std::string detail);

// RULE-08 bounds enforced while decoding and validating learning documents.
struct WorkflowLearningLimits final {
    std::size_t max_document_bytes = 16 * 1024;
    std::size_t max_id_bytes = 256;
    std::size_t max_reason_code_bytes = 128;
    std::size_t max_recovery_actions = 64;
    // Failure-retrieval bounds (DEC-029 §4); provisional defaults (RULE-10).
    std::size_t max_retrieval_results = 8;
    std::uint64_t retrieval_token_budget = 1'024;
    std::chrono::milliseconds retrieval_deadline{250};

    [[nodiscard]] Result<void> validate() const;
};

extern const WorkflowLearningLimits kDefaultWorkflowLearningLimits;

// One settled run's failure identity (DEC-029 §4). The free-string fields are
// sanitized identifiers: [A-Za-z0-9._:-] only, bounded; anything else fails
// closed so raw error text never reaches the retrieval or memory surface.
struct WorkflowFailureSignature final {
    std::string workflow_id;
    std::optional<std::string> step_id;
    std::optional<std::string> step_kind;
    std::string reason_code;
    [[nodiscard]] bool operator==(const WorkflowFailureSignature &) const = default;
};

[[nodiscard]] JsonValue failure_signature_to_json(const WorkflowFailureSignature &signature);
[[nodiscard]] Result<WorkflowFailureSignature>
failure_signature_from_json(const JsonValue &json,
                            const WorkflowLearningLimits &limits = kDefaultWorkflowLearningLimits);

// The episodic record of one settled run (DEC-029 §2). Statement surface:
// ids, digests, enum names, reason codes and bounded counters only — never
// parameter values or user text (DEC-022 §5 sanitation boundary).
struct WorkflowEpisodeRecord final {
    SchemaVersion schema_version{1, 0};
    std::string run_id;
    std::string workflow_id;
    std::string ir_digest; // hex
    std::string policy;    // closed workflow policy name
    std::string outcome;   // completed | failed | cancelled
    std::optional<std::string> failed_step_id;
    std::optional<std::string> failure_reason_code;
    std::uint32_t escalations = 0;
    std::uint32_t checkpoint_handoffs = 0;
    std::uint64_t recorded_at_ms = 0;
    [[nodiscard]] bool operator==(const WorkflowEpisodeRecord &) const = default;
};

[[nodiscard]] JsonValue workflow_episode_to_json(const WorkflowEpisodeRecord &episode);
[[nodiscard]] Result<WorkflowEpisodeRecord>
workflow_episode_from_json(const JsonValue &json,
                           const WorkflowLearningLimits &limits = kDefaultWorkflowLearningLimits);
[[nodiscard]] Sha256Digest workflow_episode_digest(const WorkflowEpisodeRecord &episode);

// One reusable recovery experience (DEC-029 §3): a failure signature plus the
// observed recovery that led to a completed run. Recorded only after an
// observed successful recovery (no failure-lesson form exists; failed
// episodes carry that knowledge).
struct WorkflowRecoveryAction final {
    std::string patch_id;
    std::string patch_digest; // hex
    // Target summary names from the closed WorkflowPatchTarget name set plus
    // the skip op; never parameter values or step arguments.
    std::vector<std::string> targets;
    [[nodiscard]] bool operator==(const WorkflowRecoveryAction &) const = default;
};

struct WorkflowRecoveryLesson final {
    SchemaVersion schema_version{1, 0};
    std::string lesson_id;
    std::string workflow_id;
    std::string ir_digest; // hex
    std::string recovered_run_id;
    WorkflowFailureSignature failure;
    std::vector<WorkflowRecoveryAction> recovery;
    bool resumed_without_patch = false;
    std::string outcome; // fixed "recovered"
    std::uint64_t recorded_at_ms = 0;
    [[nodiscard]] bool operator==(const WorkflowRecoveryLesson &) const = default;
};

[[nodiscard]] JsonValue recovery_lesson_to_json(const WorkflowRecoveryLesson &lesson);
[[nodiscard]] Result<WorkflowRecoveryLesson>
recovery_lesson_from_json(const JsonValue &json,
                          const WorkflowLearningLimits &limits = kDefaultWorkflowLearningLimits);
[[nodiscard]] Sha256Digest recovery_lesson_digest(const WorkflowRecoveryLesson &lesson);

// ---------------------------------------------------------------------------
// Pure conversions to MemoryRecord and deterministic id derivation
// (DEC-029 §2/§3/§5)
// ---------------------------------------------------------------------------

// Deterministic learning ids: the first 16 bytes of SHA-256(seed). The same
// run (or run + serial) always yields the same id, so IMemory mutation-id
// idempotency covers replays, retries and projection rebuilds.
[[nodiscard]] Id128 learning_id_from_seed(std::string_view seed);

[[nodiscard]] MemoryId workflow_episode_memory_id(const std::string &run_id);
[[nodiscard]] MutationId workflow_episode_mutation_id(const std::string &run_id);
[[nodiscard]] MemoryId recovery_lesson_memory_id(const std::string &run_id);
[[nodiscard]] MutationId recovery_lesson_mutation_id(const std::string &run_id);

// Pure conversion (DEC-029 §2): kind=Episode, statement=canonical JSON,
// verification=Verified (terminal state is committed fact), confidence=1.0
// ("it happened", not "it is useful"), provenance=evidence, validity from
// recorded_at. `now` is caller-supplied; no clock reads. The result passes
// MemoryRecord::validate.
[[nodiscard]] MemoryRecord
episode_to_memory_record(const WorkflowEpisodeRecord &episode, const MemoryScope &scope,
                         const std::vector<EventId> &evidence,
                         std::chrono::system_clock::time_point now);

// Pure conversion (DEC-029 §3): kind=RecoveryLesson, Verified, 1.0. The
// statement is the canonical lesson JSON; `recovery_lesson_from_record`
// accepts exactly that form and fails closed on anything else.
[[nodiscard]] MemoryRecord
recovery_lesson_to_memory_record(const WorkflowRecoveryLesson &lesson, const MemoryScope &scope,
                                 const std::vector<EventId> &evidence,
                                 std::chrono::system_clock::time_point now);

// Parses a RecoveryLesson from a memory record statement (canonical JSON
// only); the reuse surface never guesses structure it cannot verify.
[[nodiscard]] Result<WorkflowRecoveryLesson>
recovery_lesson_from_record(const MemoryRecord &record);

// ---------------------------------------------------------------------------
// Failure-retrieval query construction (DEC-029 §4, pure)
// ---------------------------------------------------------------------------

// Deterministic query for one failure signature: kinds are always the
// episodic recovery-serving subset {Episode, RecoveryLesson}; exact terms are
// the signature identifiers; no embedding leg. Equal signatures yield equal
// queries. The limits' retrieval bounds cap results, tokens and deadline.
[[nodiscard]] Result<MemoryQuery>
failure_retrieval_query(const WorkflowFailureSignature &signature, const MemoryScope &scope,
                        const WorkflowLearningLimits &limits = kDefaultWorkflowLearningLimits);

} // namespace mira
