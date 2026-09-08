#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/workflow_ir.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Workflow asset versioning (DEC-020 §4 / W-03)
// ---------------------------------------------------------------------------

enum class WorkflowValidationResult : std::uint8_t {
    NotValidated,
    DryRunPassed,
    Validated,
    Rejected,
};

[[nodiscard]] std::string workflow_validation_result_name(WorkflowValidationResult result);
[[nodiscard]] Result<WorkflowValidationResult>
parse_workflow_validation_result(std::string_view name);

// One immutable version record. Who/Why are sanitized, length-bounded strings;
// What Changed is content-addressed (this version's IR digest chained to the
// parent's).
struct WorkflowVersionRecord final {
    SemanticVersion version{1, 0, 0};
    std::string actor;          // Who; sanitized by the caller before append.
    std::string reason;         // Why.
    Sha256Digest content_digest{};
    Sha256Digest parent_digest{}; // Nil digest for the first record.
    WorkflowValidationResult validation = WorkflowValidationResult::NotValidated;
    std::optional<Sha256Digest> validation_evidence;
    Timestamp created_at;
};

// Append-only history for one workflow definition. Records are immutable once
// appended; indexes ("latest", by-name) are rebuildable projections.
struct WorkflowVersionHistory final {
    WorkflowId workflow_id;
    std::vector<WorkflowVersionRecord> records; // Ordered by version.
};

// Whether a version record may be referenced as a run's creation-time
// version: only DryRunPassed and Validated (W-04; unvalidated model output
// never becomes executable content).
[[nodiscard]] bool workflow_version_is_runnable(const WorkflowVersionRecord &record);

// Appends one record, failing closed on: nil workflow id, nil content digest,
// non-increasing version, a parent digest that does not chain to the current
// head, invalid validation evidence shape (evidence required unless
// NotValidated) and oversize Who/Why strings. Existing records are never
// modified.
[[nodiscard]] Result<void> append_workflow_version(WorkflowVersionHistory &history,
                                                   const WorkflowVersionRecord &record);

// Resolves the record whose content digest equals `ir_digest`. Run replay and
// continuation always resolve by digest (W-03); unknown digests fail closed.
[[nodiscard]] Result<WorkflowVersionRecord>
resolve_workflow_version(const WorkflowVersionHistory &history, const Sha256Digest &ir_digest);

// The newest runnable version, or NotFound when none exists. Runs may only be
// created against runnable versions (W-04).
[[nodiscard]] Result<WorkflowVersionRecord>
latest_runnable_workflow_version(const WorkflowVersionHistory &history);

} // namespace mira
