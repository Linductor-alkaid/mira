// TR2 runtime wiring (DEC-040 §18, M7 §4.7): the WorkflowRuntime surface that
// consumes the TR0 reference layer and the TR1 Skill publication layer.
//
// Everything here runs on the serial control plane or inside an Executor-managed
// dispatch; no thread, timer or I/O loop is created. The mount table, the run
// records' skill depth and the learning context are owned by WorkflowRuntime;
// the SkillPublicationRegistry stays host-owned and is only referenced.

#include <mira/workflow_runtime.hpp>

#include <mira/tool_skill.hpp>
#include <mira/workflow_learning.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error wiring_error(ErrorCode code, std::string message, bool retryable = false) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = std::move(message);
    error.retryable = retryable;
    return error;
}

[[nodiscard]] std::string bounded_text(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

[[nodiscard]] bool carries_tool_calls(const WorkflowDefinition &definition) {
    return std::any_of(
        definition.steps.begin(), definition.steps.end(),
        [](const WorkflowStep &step) { return step.kind == WorkflowStepKind::ToolCall; });
}

// Deterministic Procedure identity: one name+version+descriptor digest maps to
// one memory record and one mutation, so a re-sync replays as idempotent NoOps
// instead of duplicating statements (DEC-030 seed discipline).
[[nodiscard]] std::string procedure_seed(const SkillProcedureEntry &entry) {
    return "mira.workflow.skill_procedure|" + entry.name + "|" +
           std::to_string(entry.version.major) + "." + std::to_string(entry.version.minor) + "." +
           std::to_string(entry.version.patch) + "|" + entry.descriptor_digest.to_string();
}

} // namespace

// ---------------------------------------------------------------------------
// Tool-compatibility admission gate (design §18.2)
// ---------------------------------------------------------------------------

Result<std::optional<WorkflowToolCompatProjection>>
WorkflowRuntime::evaluate_tool_compat_gate(const WorkflowDefinition &definition,
                                           const std::shared_ptr<BuiltinToolRegistry> &tools) {
    using GateOutcome = std::optional<WorkflowToolCompatProjection>;
    if (!tools) {
        // No exposure view to project against; the per-step admission raises
        // the registry requirement exactly as before this gate existed.
        return Result<GateOutcome>{GateOutcome{}};
    }
    const Sha256Digest digest = workflow_definition_digest(definition);
    std::optional<WorkflowToolRefManifest> refs;
    {
        std::lock_guard lock(mutex_);
        const auto per_workflow = tool_refs_.find(definition.workflow_id);
        if (per_workflow != tool_refs_.end()) {
            const auto mounted = per_workflow->second.find(digest);
            if (mounted != per_workflow->second.end()) {
                refs = mounted->second;
            }
        }
    }
    const auto exposed = tools->exposed_tools();
    if (!refs.has_value()) {
        // v1.1 definitions carry their pinning inside the reference strings:
        // extract on the fly with follow-latest as the bare-name default (a
        // bare wire name means "follow" at run time; explicit toolref forms
        // keep their own mode). Extraction is fail closed, so a v1.1 run
        // whose references do not resolve is rejected, never absorbed.
        if (definition.schema_version.minor < 1 || !carries_tool_calls(definition)) {
            return Result<GateOutcome>{GateOutcome{}};
        }
        ToolRefExtractionOptions options;
        options.default_mode = ToolReferenceMode::FollowLatest;
        auto extracted = extract_workflow_tool_references(definition, exposed, options);
        if (!extracted.has_value()) {
            return Result<GateOutcome>{extracted.error()};
        }
        refs = std::move(extracted.value());
    }
    auto projected = project_workflow_tool_compatibility(definition, *refs, exposed);
    if (!projected.has_value()) {
        return Result<GateOutcome>{projected.error()};
    }
    return Result<GateOutcome>{GateOutcome{std::move(projected.value())}};
}

Result<std::optional<std::string>>
WorkflowRuntime::resolve_tool_reference_member(const WorkflowDefinition &definition,
                                               const JsonValue &resolved) {
    if (definition.schema_version.minor < 1 || !resolved.is_object()) {
        return Result<std::optional<std::string>>{std::optional<std::string>{}};
    }
    const auto *tool = resolved.find("tool");
    if (tool == nullptr || !tool->is_string()) {
        return Result<std::optional<std::string>>{std::optional<std::string>{}};
    }
    const std::string &text = *tool->as_string();
    if (text.compare(0, kToolReferenceScheme.size(), kToolReferenceScheme) != 0) {
        return Result<std::optional<std::string>>{std::optional<std::string>{}};
    }
    auto reference = parse_tool_reference(text);
    if (!reference.has_value()) {
        return Result<std::optional<std::string>>{reference.error()};
    }
    return Result<std::optional<std::string>>{
        std::optional<std::string>{reference.value().wire_name}};
}

void WorkflowRuntime::emit_tool_compat_degraded(const WorkflowRunView &view, TaskId task,
                                                const WorkflowToolCompatProjection &projection) {
    if (!events_) {
        return;
    }
    WorkflowToolCompatDegradedEvent event;
    event.run_id = view.run_id;
    event.workflow_id = view.workflow_id;
    event.ir_digest = view.ir_digest;
    event.projection = workflow_tool_compat_to_json(projection);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.task_id = task;
    append.payload = to_event_payload(event);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
    }
}

// ---------------------------------------------------------------------------
// Mount table (design §18.2)
// ---------------------------------------------------------------------------

Result<void> WorkflowRuntime::attach_workflow_tool_refs(const WorkflowToolRefManifest &refs) {
    std::lock_guard lock(mutex_);
    if (!accepting_ || shut_down_) {
        return wiring_error(ErrorCode::Unavailable, "workflow runtime is shutting down");
    }
    const auto history = library_.find(refs.workflow_id);
    if (history == library_.end()) {
        return wiring_error(ErrorCode::NotFound, "workflow is not published");
    }
    auto record = resolve_workflow_version(history->second, refs.definition_digest);
    if (!record.has_value()) {
        return record.error();
    }
    const auto definition = definitions_.find(refs.definition_digest);
    if (definition == definitions_.end()) {
        return wiring_error(ErrorCode::NotFound, "workflow definition content is unavailable");
    }
    if (definition->second.workflow_id.value != refs.workflow_id.value) {
        return wiring_error(ErrorCode::InvalidArgument,
                            "tool refs manifest workflow does not match the definition");
    }
    if (auto bound = verify_workflow_tool_refs(refs, refs.workflow_id, refs.definition_digest);
        !bound.has_value()) {
        return bound.error();
    }
    auto &per_workflow = tool_refs_[refs.workflow_id];
    const auto mounted = per_workflow.find(refs.definition_digest);
    if (mounted != per_workflow.end()) {
        if (mounted->second.digest == refs.digest) {
            return Result<void>{}; // Idempotent re-mount of the same manifest.
        }
        return wiring_error(ErrorCode::InvalidState,
                            "a different tool refs manifest is already mounted for this version");
    }
    if (mounted_tool_refs_ >= config_.max_mounted_tool_refs) {
        return wiring_error(ErrorCode::ResourceExhausted, "tool refs mount table is at capacity",
                            true);
    }
    per_workflow.emplace(refs.definition_digest, refs);
    ++mounted_tool_refs_;
    return Result<void>{};
}

Result<WorkflowToolRefManifest>
WorkflowRuntime::workflow_tool_refs(const WorkflowId &workflow_id,
                                    const Sha256Digest &ir_digest) const {
    std::lock_guard lock(mutex_);
    const auto per_workflow = tool_refs_.find(workflow_id);
    if (per_workflow == tool_refs_.end()) {
        return wiring_error(ErrorCode::NotFound, "tool refs manifest is not mounted");
    }
    const auto mounted = per_workflow->second.find(ir_digest);
    if (mounted == per_workflow->second.end()) {
        return wiring_error(ErrorCode::NotFound, "tool refs manifest is not mounted");
    }
    return mounted->second;
}

// ---------------------------------------------------------------------------
// Skill execution over the Tool channel (design §18.3)
// ---------------------------------------------------------------------------

std::vector<BuiltinToolRegistration>
WorkflowRuntime::skill_tool_registrations(SkillPublicationRegistry &skills) {
    std::vector<BuiltinToolRegistration> registrations;
    for (const auto &publication : skills.publications()) {
        if (publication.status != SkillPublicationStatus::Published) {
            continue;
        }
        const auto &descriptor = publication.descriptor;
        BuiltinToolRegistration registration;
        registration.spec.tool_id = ToolId::generate();
        registration.spec.wire_name = descriptor.name;
        registration.spec.version = descriptor.version;
        registration.spec.description = descriptor.surface.description;
        registration.spec.parameters_schema = descriptor.surface.parameters_schema;
        registration.spec.has_side_effects = descriptor.surface.has_side_effects;
        registration.handler = [this, &skills, name = descriptor.name, digest = descriptor.digest,
                                workflow_id = descriptor.source_workflow_id,
                                ir_digest = descriptor.source_ir_digest](
                                   const JsonValue &arguments, const OperationContext &context) {
            return execute_skill_tool_call(skills, name, digest, workflow_id, ir_digest, arguments,
                                           context);
        };
        registrations.push_back(std::move(registration));
    }
    return registrations;
}

Result<JsonValue> WorkflowRuntime::execute_skill_tool_call(SkillPublicationRegistry &skills,
                                                           const std::string &name,
                                                           const Hash &registered_descriptor_digest,
                                                           const WorkflowId &source_workflow_id,
                                                           const Sha256Digest &source_ir_digest,
                                                           const JsonValue &arguments,
                                                           const OperationContext &context) {
    if (shut_down_) {
        return wiring_error(ErrorCode::Unavailable, "workflow runtime is shut down");
    }
    const auto publication = skills.find(name);
    if (!publication.has_value() || publication->status != SkillPublicationStatus::Published) {
        return wiring_error(ErrorCode::NotFound, "skill '" + name + "' is not published");
    }
    if (!(publication->descriptor.digest == registered_descriptor_digest)) {
        return wiring_error(ErrorCode::InvalidState,
                            "skill '" + name +
                                "' drifted from its registered descriptor; re-register the tool");
    }
    // Cooperative cancellation: an already-withdrawn caller never starts a
    // child run (RULE-05 discipline).
    if (context.cancellation_requested && context.cancellation_requested()) {
        return wiring_error(ErrorCode::Cancelled, "skill call was cancelled before dispatch");
    }
    const std::uint32_t parent_depth = t_skill_dispatch_depth_;
    if (parent_depth >= config_.max_skill_call_depth) {
        return wiring_error(ErrorCode::ResourceExhausted,
                            "skill call depth exceeds the configured bound");
    }
    auto created =
        create_skill_child_run(source_workflow_id, source_ir_digest, arguments, parent_depth + 1);
    if (!created.has_value()) {
        return created.error();
    }
    // Drive on the calling (dispatch) thread: a finite, executor-managed task
    // bounded by the child run's step budget; the parent's cancellation probe
    // converges at the child's step boundaries.
    auto driven = execute_run(created.value().run_id, context);
    if (!driven.has_value()) {
        return driven.error();
    }
    if (context.cancellation_requested && context.cancellation_requested()) {
        return wiring_error(ErrorCode::Cancelled, "skill call was cancelled");
    }
    const auto &result = driven.value();
    if (result.state == WorkflowRunState::Cancelled) {
        return wiring_error(ErrorCode::Cancelled, "skill run was cancelled");
    }
    if (result.state != WorkflowRunState::Completed) {
        return wiring_error(ErrorCode::InvalidState,
                            "skill run settled as " +
                                std::string(workflow_run_state_name(result.state)) + ": " +
                                bounded_text(result.safe_summary, 256));
    }
    JsonValue::Object object;
    object.emplace_back("run_id", created.value().run_id.to_string());
    object.emplace_back("state", std::string(workflow_run_state_name(result.state)));
    object.emplace_back("safe_summary", bounded_text(result.safe_summary, 512));
    return JsonValue{std::move(object)};
}

Result<WorkflowRunView> WorkflowRuntime::create_skill_child_run(const WorkflowId &workflow_id,
                                                                const Sha256Digest &ir_digest,
                                                                JsonValue parameters,
                                                                std::uint32_t skill_depth) {
    WorkflowDefinition definition;
    {
        std::lock_guard lock(mutex_);
        const auto library = library_.find(workflow_id);
        if (library == library_.end()) {
            return wiring_error(ErrorCode::NotFound,
                                "skill source workflow is not published in the library");
        }
        auto record = resolve_workflow_version(library->second, ir_digest);
        if (!record.has_value()) {
            return record.error();
        }
        if (!workflow_version_is_runnable(record.value())) {
            return wiring_error(ErrorCode::InvalidArgument,
                                "skill source version is not validated; only DryRunPassed or "
                                "Validated versions may run");
        }
        const auto content = definitions_.find(ir_digest);
        if (content == definitions_.end()) {
            return wiring_error(ErrorCode::NotFound,
                                "skill source definition content is unavailable");
        }
        definition = content->second;
    }
    return create_run_locked(definition, std::move(parameters), std::nullopt, skill_depth);
}

// ---------------------------------------------------------------------------
// Procedure index memory wiring (design §18.4)
// ---------------------------------------------------------------------------

Result<WorkflowRuntime::WorkflowProcedureSyncReport>
WorkflowRuntime::sync_skill_procedure_index(const SkillPublicationRegistry &skills) {
    std::shared_ptr<IMemory> memory;
    MemoryScope scope;
    {
        std::lock_guard lock(mutex_);
        if (!learning_memory_) {
            return wiring_error(ErrorCode::InvalidState,
                                "procedure sync requires an installed learning context");
        }
        memory = learning_memory_;
        scope = learning_scope_;
    }
    auto index = project_skill_procedure_index(skills.publications());
    if (!index.has_value()) {
        return index.error();
    }
    // M4 mutation contract: every Add carries event evidence (upper-contract
    // priority; this corrects the initial freeze wording — design §18.4). The
    // sync audit event anchors the whole batch; without an event store the
    // sync refuses instead of writing untraceable memory.
    std::uint64_t published_count = 0;
    for (const auto &entry : index.value().entries) {
        if (entry.status == SkillPublicationStatus::Published) {
            ++published_count;
        }
    }
    if (!events_) {
        return wiring_error(ErrorCode::Unavailable,
                            "procedure sync requires an event store for mutation evidence");
    }
    WorkflowProceduresSyncedEvent audit;
    audit.procedure_index_digest = index.value().digest;
    audit.published_count = published_count;
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_id_;
    append.session_id = session_;
    append.payload = to_event_payload(audit);
    if (!events_->append(append).has_value()) {
        ++event_emit_failures_;
        return wiring_error(ErrorCode::Unavailable, "procedure sync audit event failed to append");
    }
    WorkflowProcedureSyncReport report;
    const auto now = std::chrono::system_clock::now();
    for (const auto &entry : index.value().entries) {
        if (entry.status != SkillPublicationStatus::Published) {
            continue; // Revoked entries never (re-)enter memory here.
        }
        const std::string seed = procedure_seed(entry);
        MemoryRecord record;
        record.id = MemoryId{learning_id_from_seed(seed)};
        record.scope = scope;
        record.kind = MemoryKind::Procedure;
        record.statement = entry.statement;
        record.validity.valid_from = now;
        record.recorded_at = now;
        // The statement is publication-derived, not run-verified; the M4
        // discipline keeps unverified content conservative even though the
        // mutation now carries the sync event as evidence.
        record.confidence = 0.3F;
        auto applied = memory->apply([&] {
            MemoryMutation mutation;
            mutation.id = MutationId{learning_id_from_seed(seed + "|mutation")};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = scope;
            mutation.proposed = record;
            mutation.evidence = {append.event_id};
            mutation.reason = MutationReasonCode::VerifiedEvent;
            return mutation;
        }());
        if (!applied.has_value()) {
            ++report.failed;
            continue;
        }
        if (applied.value().applied == MemoryMutationType::Noop ||
            applied.value().idempotent_replay) {
            ++report.idempotent;
        } else {
            ++report.applied;
        }
    }
    return report;
}

} // namespace mira
