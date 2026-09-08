#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/workflow_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Successful trajectory contract and the workflow compiler (DEC-025/026,
// stage D): one real, successful execution as compile input plus the
// deterministic pure functions turning it (and induction candidates) into a
// WorkflowDefinition draft.
// ---------------------------------------------------------------------------

// Deterministic compile/induction error codes (DEC-025 §2/§3, DEC-026 §1/§2).
enum class WorkflowCompileError : std::int32_t {
    CaptureNotExecuted = 1,      // run is not a Completed, side-effecting run
    CaptureNotCompleted = 2,     // run is not terminal-Completed
    CompileInvalidOptions = 3,   // nil target id / empty name / bad policy
    CompilePolicyNotAllowed = 4, // requested default policy outside the allowed set
    CompileToolBindingInvalid = 5, // effective tool_call arguments miss "tool"
    CompileJumpTargetSkipped = 6,  // control jump targets a dropped step
    PublishDryRunFailed = 7,     // gate drive did not reach Completed
    InductSkeletonMismatch = 8,  // trajectories are not same-skeleton
    InductTypeMismatch = 9,      // same leaf with different JSON scalar kinds
    InductCandidateInvalid = 10, // explicit candidate fails shape rules
    InductNameConflict = 11,     // name collides with a parameter or candidate
    InductLeafNotScalar = 12,    // pointer resolves to a non-scalar leaf
    InductReservedMember = 13,   // pointer targets the reserved "tool" member
    InductLimitExceeded = 14,    // candidates exceed max_parameters (RULE-08)
};

[[nodiscard]] Error make_workflow_compile_error(WorkflowCompileError code, std::string detail);

// One executed step as captured from a completed run (or built directly by
// the host for the agent tool-call path). `arguments` is the effective,
// fully resolved literal form; `raw_arguments` keeps the source form with
// {"$param": name} references so induction can recover parameter names.
struct WorkflowTrajectoryStep final {
    StepId source_step_id;
    std::string name;
    WorkflowStepKind kind = WorkflowStepKind::ToolCall;
    bool loop_head = false;
    JsonValue arguments;
    JsonValue raw_arguments;
    std::optional<WorkflowPredicate> precondition;
    std::optional<WorkflowPredicate> verification;
    std::optional<WorkflowRecoveryHook> recovery;
    std::uint32_t max_attempts = 1;
    std::optional<StepId> jump_to;
    std::uint32_t max_iterations = 1;
};

// One successful execution snapshot (DEC-025 §1). Runs capture this through
// WorkflowRuntime::capture_trajectory; hosts may construct it directly.
struct WorkflowTrajectory final {
    WorkflowId workflow_id;
    // Creation-time pinned content of the source run; nil for host-built
    // trajectories.
    Sha256Digest source_digest{};
    std::optional<WorkflowRunId> source_run_id;
    // Post-patch effective parameter table (defaults applied).
    JsonValue effective_parameters;
    WorkflowPolicy effective_policy = WorkflowPolicy::Strict;
    // Source definition shape carried for compilation: parameter specs (run
    // path; empty when host-built), the allowed policy set (empty means the
    // compile options alone decide) and the source default policy.
    std::vector<WorkflowParameterSpec> source_parameters;
    std::vector<WorkflowPolicy> allowed_policies;
    WorkflowPolicy source_default_policy = WorkflowPolicy::Strict;
    // Effective step sequence: skipped steps are already dropped by capture.
    std::vector<WorkflowTrajectoryStep> steps;
};

struct WorkflowCompileOptions final {
    // Target identity (DEC-025 §2): same id chains a new version onto the
    // existing history; a fresh id derives a new workflow.
    WorkflowId workflow_id;
    std::string name;
    std::string summary;
    // Defaults to the source default policy; must be a member of the
    // effective allowed set.
    std::optional<WorkflowPolicy> default_policy;
};

// Literal compilation (DEC-025 §2): effective arguments verbatim, observed
// parameter values baked as defaults (required -> optional + default),
// predicates/hooks/loop structure preserved, deterministic output. The
// result is a host-editable draft; entering the library goes through
// WorkflowRuntime::publish_validated.
[[nodiscard]] Result<WorkflowDefinition>
compile_workflow(const WorkflowTrajectory &trajectory, const WorkflowCompileOptions &options);

// ---------------------------------------------------------------------------
// Task induction (DEC-026)
// ---------------------------------------------------------------------------

enum class WorkflowCandidateProvenance : std::uint8_t {
    Provenance, // leaf was a {"$param": name} reference in the source
    Structural, // discovered by structural diff across trajectories
    Explicit,   // proposed by the host (or model through the host)
};

[[nodiscard]] std::string workflow_candidate_provenance_name(WorkflowCandidateProvenance provenance);
[[nodiscard]] Result<WorkflowCandidateProvenance>
parse_workflow_candidate_provenance(std::string_view name);

// One parameterization proposal (DEC-026 §1): a scalar leaf in one step's
// effective arguments worth lifting into a parameter. Proposals are drafts,
// never facts: they are host-editable and reach the library only through the
// publish gate.
struct WorkflowParameterCandidate final {
    std::string name;       // ^[a-z][a-z0-9_]{0,63}$
    std::size_t step_index = 0; // Index into WorkflowTrajectory::steps.
    std::string pointer;    // RFC 6901 pointer to the scalar leaf.
    WorkflowCandidateProvenance provenance = WorkflowCandidateProvenance::Structural;
    // Observed value per input trajectory, anchor (first) first; kept for
    // host review only, not part of the compiled IR.
    std::vector<JsonValue> observed_values;
};

// Structural diff over same-skeleton trajectories (DEC-026 §1): scalar leaves
// with equal type and differing values become candidates; constant leaves
// stay literal. Provenance names win over generated ones; generated names
// follow stable (step_index, pointer) order. 2..16 trajectories.
[[nodiscard]] Result<std::vector<WorkflowParameterCandidate>>
induce_parameters(const std::vector<WorkflowTrajectory> &trajectories,
                  const WorkflowLimits &limits = kDefaultWorkflowLimits);

// Parameterized compilation (DEC-026 §2): rewrites candidate leaves into
// {"$param": name} references (the existing binding pure functions resolve
// them; no second substitution mechanism) and adds inferred parameter specs
// (type from the anchor observation, required=false, default=anchor value).
// Provenance candidates matching a source parameter reuse its spec; name
// collisions elsewhere fail closed.
[[nodiscard]] Result<WorkflowDefinition>
compile_workflow(const WorkflowTrajectory &trajectory, const WorkflowCompileOptions &options,
                 const std::vector<WorkflowParameterCandidate> &candidates,
                 const WorkflowLimits &limits = kDefaultWorkflowLimits);

} // namespace mira
