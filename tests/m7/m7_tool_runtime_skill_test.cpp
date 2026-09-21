// M7 TR2 runtime wiring verification - skill execution and Procedure wiring
// domain: gates M7-TR2-G4..G6 (frozen in
// docs/plans/m7-tools-evaluation-platform-v1.md sections 4.7/5.7; contract
// source docs/design/tool_reference_and_skill_design.md section 18).
//
// Covers the WorkflowRuntime consumption of the TR1 Skill publication layer:
// the skill execution adapters over the Tool channel (DEC-015 identity gate,
// lifecycle outcomes, the skill call depth bound, the child-run
// compatibility gate), the Procedure memory sync with its
// WorkflowProceduresSynced audit event and the public surface closure. The
// reference expression and admission domain (gates M7-TR2-G1..G3) lives in
// m7_tool_runtime_wiring_test.cpp; shared fixtures live in
// tr2_wiring_support.hpp. Split is a pure move: no assertion, literal or gate
// identity changed.
//
// Deterministic by construction: every value that reaches an assertion or the
// --report output is content-derived, so reports reproduce byte-for-byte
// across processes and build trees.

#include "../support/test.hpp"
#include "tr2_wiring_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// --report scenario (pure content-derived values only)
// ---------------------------------------------------------------------------

std::string build_skill_wiring_report() {
    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.workflow.tool_runtime_skill.report.v1"));

    const std::vector<ExposedToolSpec> view = hand_view();

    const WorkflowDefinition v10 =
        make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "report projection summary");

    // Skill surface identity: descriptor digest over a fixed view is content.
    JsonValue::Object skill;
    {
        const WorkflowToolRefManifest refs = extract_or_abort(v10, view);
        const auto descriptor = must(
            make_skill_descriptor(v10, refs, view, "report.skill", {1, 0, 0}), "report descriptor");
        skill.emplace_back("descriptor_digest", JsonValue{descriptor.digest.to_string()});
        skill.emplace_back("surface_description", JsonValue{descriptor.surface.description});
        skill.emplace_back("has_side_effects", JsonValue{descriptor.surface.has_side_effects});
    }
    report.emplace_back("skill", JsonValue{std::move(skill)});

    // Procedure sync counters over two published skills (counts only; the
    // memory records carry wall-clock fields that never enter the report).
    JsonValue::Object procedure;
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
        auto workflow = fixture.make_workflow();
        must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
             "report learning context");
        SkillPublicationRegistry skills;
        int link = 0;
        for (const WorkflowId id : {kWorkflowAlpha, kWorkflowBeta}) {
            ++link;
            const WorkflowDefinition definition =
                make_lookup_definition(id, SchemaVersion{1, 0}, "report procedure summary");
            const auto published =
                must(workflow->publish_validated(definition, "fixture", "report procedure"),
                     "report procedure publish");
            const WorkflowToolRefManifest refs = must(
                workflow->workflow_tool_refs(id, published.ir_digest), "report procedure refs");
            const WorkflowVersionHistory history =
                single_version_history(id, definition, WorkflowValidationResult::DryRunPassed);
            const auto descriptor =
                must(make_skill_descriptor(definition, refs, view,
                                           "report.skill_" + std::to_string(link), {1, 0, 0}),
                     "report procedure descriptor");
            must(skills.publish_skill(descriptor, definition, refs, view, history),
                 "report procedure publish skill");
        }
        const auto first = must(workflow->sync_skill_procedure_index(skills), "report sync one");
        const auto second = must(workflow->sync_skill_procedure_index(skills), "report sync two");
        procedure.emplace_back("applied_first",
                               JsonValue{static_cast<std::int64_t>(first.applied)});
        procedure.emplace_back("idempotent_second",
                               JsonValue{static_cast<std::int64_t>(second.idempotent)});
        procedure.emplace_back("records",
                               JsonValue{static_cast<std::int64_t>(memory->stored_records())});
        const auto index =
            must(project_skill_procedure_index(skills.publications()), "report index");
        procedure.emplace_back("index_digest", JsonValue{index.digest.to_string()});
    }
    report.emplace_back("procedure", JsonValue{std::move(procedure)});

    return canonical_json_string(JsonValue{std::move(report)});

    return canonical_json_string(JsonValue{std::move(report)});
}

// ---------------------------------------------------------------------------
// G4: skill execution over the Tool channel (M7-TR2-G4)
// ---------------------------------------------------------------------------

// Publishes one skill whose source workflow runs through the runtime library
// path and returns the pinned scenario inputs.
struct SkillSetup final {
    WorkflowDefinition definition;
    SkillDescriptor descriptor;
    WorkflowToolRefManifest refs;
    WorkflowVersionHistory history;
};

SkillSetup publish_skill_source(WorkflowRuntime &workflow, std::span<const ExposedToolSpec> view,
                                SkillPublicationRegistry &skills, const WorkflowId &workflow_id,
                                const std::string &skill_name, const std::string &summary,
                                const std::string &tool_member, SemanticVersion version) {
    SkillSetup setup;
    setup.definition =
        wiring_definition(workflow_id, SchemaVersion{1, 0}, "fixture.skill_source", summary,
                          {key_param()}, {lookup_step(kStepLookup, tool_member)});
    const auto published =
        must(workflow.publish_validated(setup.definition, "fixture", "skill source"),
             "skill source publish");
    setup.refs =
        must(workflow.workflow_tool_refs(workflow_id, published.ir_digest), "skill source refs");
    setup.history = single_version_history(workflow_id, setup.definition,
                                           WorkflowValidationResult::DryRunPassed);
    setup.descriptor =
        must(make_skill_descriptor(setup.definition, setup.refs, view, skill_name, version),
             "skill descriptor");
    must(skills.publish_skill(setup.descriptor, setup.definition, setup.refs, view, setup.history),
         "skill publish");
    return setup;
}

// Registers every not-yet-registered Published skill tool into the registry
// (skill_tool_registrations re-lists the whole publication set each call).
void register_skill_tools(BuiltinToolRegistry &registry, WorkflowRuntime &workflow,
                          SkillPublicationRegistry &skills) {
    std::vector<std::string> known;
    for (const auto &entry : registry.exposed_tools()) {
        known.push_back(entry.wire_name);
    }
    for (auto registration : workflow.skill_tool_registrations(skills)) {
        if (std::find(known.begin(), known.end(), registration.spec.wire_name) != known.end()) {
            continue;
        }
        must(registry.register_tool(std::move(registration.spec), std::move(registration.handler)),
             "register skill tool");
    }
}

int g4_registration_surface_and_identity_gate() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto workflow = fixture.make_workflow();
    SkillPublicationRegistry skills;
    const SkillSetup setup =
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});

    const auto registrations = workflow->skill_tool_registrations(skills);
    MIRA_CHECK(registrations.size() == 1);
    const BuiltinToolRegistration &registration = registrations.front();
    // The spec carries the descriptor surface field by field.
    MIRA_CHECK(registration.spec.wire_name == setup.descriptor.name);
    MIRA_CHECK(registration.spec.version == setup.descriptor.version);
    MIRA_CHECK(registration.spec.description == setup.descriptor.surface.description);
    MIRA_CHECK(registration.spec.parameters_schema.root ==
               setup.descriptor.surface.parameters_schema.root);
    MIRA_CHECK(registration.spec.has_side_effects == setup.descriptor.surface.has_side_effects);
    // A side-effecting source surface maps as well (registration only; the
    // source workflow must carry the W-02 verification predicate).
    {
        FixtureTool render_tool;
        render_tool.tool_hex = "000000000000000000000000000000e2";
        render_tool.wire_name = "delta.render";
        render_tool.description = "wiring render tool";
        auto render_registration = render_tool.registration();
        render_registration.spec.has_side_effects = true;
        must(fixture.registry_->register_tool(std::move(render_registration.spec),
                                              std::move(render_registration.handler)),
             "register render tool");
        SkillPublicationRegistry render_skills;
        WorkflowDefinition render_definition =
            make_lookup_definition(kWorkflowBeta, SchemaVersion{1, 0}, "render skill source");
        render_definition.steps.front().arguments =
            json_or_abort(R"json({"tool":"delta.render","key":{"$param":"key"}})json");
        render_definition.steps.front().verification = step_result_predicate(
            fixed_step(kStepLookup), WorkflowPredicateOp::Eq, JsonValue{std::string("sent")});
        const auto published =
            must(workflow->publish_validated(render_definition, "fixture", "render source"),
                 "render publish");
        const auto render_view = fixture.registry_->exposed_tools();
        const auto render_refs =
            must(workflow->workflow_tool_refs(kWorkflowBeta, published.ir_digest), "render refs");
        const auto render_descriptor =
            must(make_skill_descriptor(render_definition, render_refs, render_view,
                                       "fixture.render_skill", {1, 0, 0}),
                 "render descriptor");
        MIRA_CHECK(render_descriptor.surface.has_side_effects);
        const auto render_history = single_version_history(kWorkflowBeta, render_definition,
                                                           WorkflowValidationResult::DryRunPassed);
        must(render_skills.publish_skill(render_descriptor, render_definition, render_refs,
                                         render_view, render_history),
             "render skill publish");
        const auto render_registrations = workflow->skill_tool_registrations(render_skills);
        MIRA_CHECK(render_registrations.size() == 1);
        MIRA_CHECK(render_registrations.front().spec.has_side_effects);
        MIRA_CHECK(render_registrations.front().spec.wire_name == "fixture.render_skill");
    }
    // Registration passes the DEC-015 identity gate: only a proposal built
    // from the exposed spec executes.
    {
        auto owned = registrations.front();
        must(fixture.registry_->register_tool(std::move(owned.spec), std::move(owned.handler)),
             "register skill");
    }
    const auto exposed = fixture.registry_->exposed_tools();
    const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
    MIRA_CHECK(skill_spec != nullptr);
    {
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"), "call-ok", context),
            context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(!outcome.value().failed);
        const auto *state = outcome.value().result.find("state");
        MIRA_CHECK(state != nullptr && state->is_string() && *state->as_string() == "completed");
        MIRA_CHECK(tool.dispatches.load() == 1);
    }
    // A stale identity is rejected exactly as any other registry entry.
    {
        const OperationContext context = plain_context();
        ToolProposal stale = proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"),
                                          "call-stale", context);
        stale.tool_id = fixed_tool("000000000000000000000000000000f1");
        const auto outcome = fixture.registry_->execute(stale, context);
        MIRA_CHECK(!outcome.has_value());
        MIRA_CHECK(outcome.error().code == ErrorCode::NotFound);
    }
    return 0;
}

int g4_skill_execution_outcomes() {
    const JsonValue arguments = json_or_abort(R"json({"key":"k"})json");

    // Completed, cancelled and revoked outcomes over one healthy fixture.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        MIRA_CHECK(skill_spec != nullptr);

        // Child run Completed -> structured JSON result.
        {
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-completed", context), context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
            const auto *run_id = outcome.value().result.find("run_id");
            MIRA_CHECK(run_id != nullptr && run_id->is_string());
            MIRA_CHECK(WorkflowRunId::parse(*run_id->as_string()).has_value());
            const auto *state = outcome.value().result.find("state");
            MIRA_CHECK(state != nullptr && state->is_string() &&
                       *state->as_string() == "completed");
            const auto *safe = outcome.value().result.find("safe_summary");
            MIRA_CHECK(safe != nullptr && safe->is_string() && !safe->as_string()->empty());
            MIRA_CHECK(outcome.value().result.is_object() &&
                       outcome.value().result.as_object()->size() == 3);
        }
        // A withdrawn caller never starts a child run: Cancelled.
        {
            OperationContext context = plain_context();
            context.cancellation_requested = [] { return true; };
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-cancelled", context), context);
            MIRA_CHECK(!outcome.has_value());
            MIRA_CHECK(outcome.error().code == ErrorCode::Cancelled);
        }
        // Revoked publication -> NotFound failed record.
        {
            must(skills.revoke_skill("fixture.skill", "outcome revoke"), "revoke");
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*skill_spec, arguments, "call-revoked", context), context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(outcome.value().safe_error_summary.find("not published") !=
                       std::string::npos);
        }
    }
    // Descriptor drift on a healthy publication (upgrade without
    // re-registering) -> InvalidState failed record.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        const SkillSetup v1 = publish_skill_source(
            *workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha, "fixture.skill",
            "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        // Upgrade the publication without re-registering the tool.
        const WorkflowDefinition v2 =
            make_lookup_definition(kWorkflowAlpha, SchemaVersion{1, 0}, "wiring skill source v2");
        const auto v2_view = fixture.registry_->exposed_tools();
        const auto v2_refs = extract_or_abort(v2, v2_view);
        const auto v2_descriptor =
            must(make_skill_descriptor(v2, v2_refs, v2_view, "fixture.skill", {2, 0, 0}),
                 "descriptor v2");
        const auto both = two_version_history(v1.definition, v2);
        must(skills.upgrade_skill("fixture.skill", v2_descriptor, v2, v2_refs, v2_view, both),
             "upgrade skill");
        MIRA_CHECK(skills.find("fixture.skill")->descriptor.digest != v1.descriptor.digest);

        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, arguments, "call-drift", context), context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().failed);
        MIRA_CHECK(outcome.value().safe_error_summary.find("drifted") != std::string::npos);
    }
    // Child run Failed (internal tool failure) -> InvalidState failed record.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        tool.failures_first = 1;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                             "fixture.skill", "wiring skill source", "delta.lookup", {1, 0, 0});
        register_skill_tools(*fixture.registry_, *workflow, skills);
        const auto exposed = fixture.registry_->exposed_tools();
        const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
        const OperationContext context = plain_context();
        const auto outcome = fixture.registry_->execute(
            proposal_for(*skill_spec, arguments, "call-failed", context), context);
        MIRA_CHECK(outcome.has_value());
        MIRA_CHECK(outcome.value().failed);
        MIRA_CHECK(outcome.value().safe_error_summary.find("failed") != std::string::npos);
        MIRA_CHECK(tool.dispatches.load() == 1);
    }
    return 0;
}

// Publishes one link of the skill chain: its source workflow calls the next
// skill by wire name (or the plain tool at the bottom of the chain).
void publish_chain_link(WorkflowFixture &fixture, WorkflowRuntime &workflow,
                        SkillPublicationRegistry &skills, const WorkflowId &workflow_id,
                        const std::string &skill_name, const std::string &summary,
                        const std::string &tool_member) {
    SkillSetup setup;
    setup.definition =
        wiring_definition(workflow_id, SchemaVersion{1, 0}, "fixture.chain_link", summary,
                          {key_param()}, {lookup_step(kStepLookup, tool_member)});
    const auto published = must(
        workflow.publish_validated(setup.definition, "fixture", "chain link"), "chain publish");
    const auto view = fixture.registry_->exposed_tools();
    setup.refs = must(workflow.workflow_tool_refs(workflow_id, published.ir_digest), "chain refs");
    setup.history = single_version_history(workflow_id, setup.definition,
                                           WorkflowValidationResult::DryRunPassed);
    setup.descriptor =
        must(make_skill_descriptor(setup.definition, setup.refs, view, skill_name, {1, 0, 0}),
             "chain descriptor");
    must(skills.publish_skill(setup.descriptor, setup.definition, setup.refs, view, setup.history),
         "chain skill publish");
    register_skill_tools(*fixture.registry_, workflow, skills);
}

int g4_skill_depth_matrix() {
    // Default bound (max_skill_call_depth = 2): the depth-1 direct call and
    // the depth-2 nested call run; the depth-3 chain is refused.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        auto workflow = fixture.make_workflow();
        SkillPublicationRegistry skills;
        publish_chain_link(fixture, *workflow, skills, kWorkflowDelta, "fixture.skill.innermost",
                           "chain innermost", "delta.lookup");
        publish_chain_link(fixture, *workflow, skills, kWorkflowEpsilon, "fixture.skill.inner",
                           "chain inner", "fixture.skill.innermost");
        publish_chain_link(fixture, *workflow, skills, kWorkflowZeta, "fixture.skill.outer",
                           "chain outer", "fixture.skill.inner");
        MIRA_CHECK(fixture.registry_->size() == 4); // lookup + three skills
        const auto exposed = fixture.registry_->exposed_tools();

        for (const char *wire : {"fixture.skill.innermost", "fixture.skill.inner"}) {
            const ExposedToolSpec *spec = find_exposed(exposed, wire);
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"),
                             std::string("call-") + wire, context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
        }
        // The depth-3 chain fails closed.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.outer");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth3",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(!outcome.value().safe_error_summary.empty());
        }
        MIRA_CHECK(tool.dispatches.load() >= 2);
    }
    // Tightened bound (max_skill_call_depth = 1): a single skill still runs,
    // any nested skill call is refused with the depth reason.
    {
        WorkflowFixture fixture;
        FixtureTool tool;
        fixture.registry_ = registry_with(tool);
        WorkflowRuntimeConfig config;
        config.max_skill_call_depth = 1;
        auto workflow = fixture.make_workflow(config);
        SkillPublicationRegistry skills;
        publish_chain_link(fixture, *workflow, skills, kWorkflowEpsilon, "fixture.skill.inner",
                           "tight inner", "delta.lookup");
        publish_chain_link(fixture, *workflow, skills, kWorkflowZeta, "fixture.skill.outer",
                           "tight outer", "fixture.skill.inner");
        const auto exposed = fixture.registry_->exposed_tools();
        // The single-level call succeeds.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.inner");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth1",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(!outcome.value().failed);
        }
        // The nested call is refused with the configured bound.
        {
            const ExposedToolSpec *spec = find_exposed(exposed, "fixture.skill.outer");
            MIRA_CHECK(spec != nullptr);
            const OperationContext context = plain_context();
            const auto outcome = fixture.registry_->execute(
                proposal_for(*spec, json_or_abort(R"json({"key":"k"})json"), "call-depth2",
                             context),
                context);
            MIRA_CHECK(outcome.has_value());
            MIRA_CHECK(outcome.value().failed);
            MIRA_CHECK(outcome.value().safe_error_summary.find("depth exceeds") !=
                       std::string::npos);
        }
    }
    return 0;
}

int g4_skill_child_passes_compat_gate() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto workflow = fixture.make_workflow();
    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.skill", "gated skill source", "delta.lookup", {1, 0, 0});
    // Build the evolved registry holding both the evolved lookup tool and the
    // already-registered skill tool, so the skill stays dispatchable while its
    // source workflow's mounted manifest turns Invalid.
    auto skill_regs = workflow->skill_tool_registrations(skills);
    MIRA_CHECK(skill_regs.size() == 1);
    auto evolved_registry = std::make_shared<BuiltinToolRegistry>();
    auto evolved = std::make_shared<FixtureTool>();
    evolved->description = "wiring lookup tool v3";
    evolved->schema_json = kEvolvedRequiredSchema;
    auto evolved_registration = evolved->registration();
    must(evolved_registry->register_tool(std::move(evolved_registration.spec),
                                         std::move(evolved_registration.handler)),
         "register evolved lookup");
    must(evolved_registry->register_tool(std::move(skill_regs.front().spec),
                                         std::move(skill_regs.front().handler)),
         "re-register skill tool");
    workflow->set_tool_registry(evolved_registry);

    const auto exposed = evolved_registry->exposed_tools();
    const ExposedToolSpec *skill_spec = find_exposed(exposed, "fixture.skill");
    MIRA_CHECK(skill_spec != nullptr);
    const OperationContext context = plain_context();
    const auto outcome = evolved_registry->execute(
        proposal_for(*skill_spec, json_or_abort(R"json({"key":"k"})json"), "call-gated", context),
        context);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().failed);
    MIRA_CHECK(outcome.value().safe_error_summary.find("tool compatibility admission rejected") !=
               std::string::npos);
    MIRA_CHECK(evolved->dispatches.load() == 0);
    MIRA_CHECK(tool.dispatches.load() == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// G5: Procedure memory wiring (M7-TR2-G5)
// ---------------------------------------------------------------------------

int g5_procedure_sync_wiring() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
    auto workflow = fixture.make_workflow();
    // The learning scope is the Agent scope; the User scope is rejected.
    must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
         "learning context");
    {
        MemoryScope user_scope;
        user_scope.kind = MemoryScopeKind::User;
        user_scope.subject_id = "mira.test.user";
        const auto rejected = workflow->set_learning_context(memory, user_scope);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidArgument);
    }

    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.proc_alpha", "procedure sync alpha", "delta.lookup", {1, 0, 0});
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowBeta,
                         "fixture.proc_beta", "procedure sync beta", "delta.lookup", {1, 0, 0});
    const auto index = must(project_skill_procedure_index(skills.publications()), "sync index");
    MIRA_CHECK(index.entries.size() == 2);
    MIRA_CHECK(index.entries.front().status == SkillPublicationStatus::Published);

    // First sync: one applied Add per Published entry.
    const auto first = must(workflow->sync_skill_procedure_index(skills), "first sync");
    MIRA_CHECK(first.applied == 2);
    MIRA_CHECK(first.idempotent == 0);
    MIRA_CHECK(first.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Record shape: deterministic identity, Procedure kind, learning scope,
    // byte-identical statement, timestamped, capped confidence.
    {
        const auto records = memory->snapshot();
        for (const auto &entry : index.entries) {
            const std::string seed = "mira.workflow.skill_procedure|" + entry.name + "|1.0.0|" +
                                     entry.descriptor_digest.to_string();
            const MemoryId expected_id{learning_id_from_seed(seed)};
            const auto found =
                std::find_if(records.begin(), records.end(),
                             [&](const MemoryRecord &record) { return record.id == expected_id; });
            MIRA_CHECK(found != records.end());
            MIRA_CHECK(found->kind == MemoryKind::Procedure);
            MIRA_CHECK(found->scope == mira::testing::learning_scope());
            MIRA_CHECK(found->statement == entry.statement);
            MIRA_CHECK(found->recorded_at.time_since_epoch().count() != 0);
            MIRA_CHECK(found->confidence == 0.3F);
            MIRA_CHECK(found->status == MemoryStatus::Active);
        }
    }

    // Second sync: idempotent replay, no duplicates.
    const auto second = must(workflow->sync_skill_procedure_index(skills), "second sync");
    MIRA_CHECK(second.applied == 0);
    MIRA_CHECK(second.idempotent == 2);
    MIRA_CHECK(second.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Revoked entries are never written; the surviving entry replays.
    must(skills.revoke_skill(index.entries.back().name, "sync revoke"), "sync revoke");
    const auto third = must(workflow->sync_skill_procedure_index(skills), "third sync");
    MIRA_CHECK(third.applied == 0);
    MIRA_CHECK(third.idempotent == 1);
    MIRA_CHECK(third.failed == 0);
    MIRA_CHECK(memory->stored_records() == 2);

    // Each sync emits exactly one State-level audit event with the closed
    // payload set; its id is the evidence anchor of every mutation in the
    // batch, and it binds the observed index digest and publication count.
    {
        const auto sync_events =
            typed_events(*fixture.events_, fixture.session_id_, "WorkflowProceduresSynced");
        MIRA_CHECK(sync_events.size() == 3);
        for (const auto &event : sync_events) {
            MIRA_CHECK(event.classification == EventClass::State);
            MIRA_CHECK(event.payload.is_object());
            for (const auto &member : *event.payload.as_object()) {
                const bool known = member.first == "schema" ||
                                   member.first == "procedure_index_digest" ||
                                   member.first == "published_count";
                MIRA_CHECK(known);
            }
        }
        const auto *digest = sync_events.front().payload.find("procedure_index_digest");
        MIRA_CHECK(digest != nullptr && digest->is_string() &&
                   *digest->as_string() == index.digest.to_string());
        const auto *count = sync_events.front().payload.find("published_count");
        MIRA_CHECK(count != nullptr && count->as_integer().has_value() &&
                   *count->as_integer() == 2);
        // The stored payload survives the strict parser.
        EventPayload wire;
        wire.type = sync_events.front().type;
        wire.data = to_json_string(sync_events.front().payload);
        const auto parsed = must(parse_workflow_procedures_synced(wire), "synced parse");
        MIRA_CHECK(parsed.procedure_index_digest == index.digest);
        MIRA_CHECK(parsed.published_count == 2);
    }
    // Without an installed learning context the sync refuses.
    {
        auto bare = std::make_unique<WorkflowRuntime>(fixture.executor_, *fixture.runtime_,
                                                      fixture.session_id_, fixture.environment_);
        bare->set_event_store(fixture.events_);
        const auto rejected = bare->sync_skill_procedure_index(skills);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::InvalidState);
        static_cast<void>(bare->shutdown());
    }
    // Without an event store there is no mutation evidence anchor: the sync
    // refuses instead of writing untraceable memory.
    {
        auto storeless = std::make_unique<WorkflowRuntime>(
            fixture.executor_, *fixture.runtime_, fixture.session_id_, fixture.environment_);
        must(storeless->set_learning_context(memory, mira::testing::learning_scope()),
             "storeless learning context");
        const auto rejected = storeless->sync_skill_procedure_index(skills);
        MIRA_CHECK(!rejected.has_value());
        MIRA_CHECK(rejected.error().code == ErrorCode::Unavailable);
        MIRA_CHECK(memory->stored_records() == 2); // nothing extra written
        static_cast<void>(storeless->shutdown());
    }
    return 0;
}

int g5_episode_path_intact() {
    WorkflowFixture fixture;
    FixtureTool tool;
    fixture.registry_ = registry_with(tool);
    auto memory = std::make_shared<mira::testing::FakeLearningMemory>();
    auto workflow = fixture.make_workflow();
    must(workflow->set_learning_context(memory, mira::testing::learning_scope()),
         "learning context");
    SkillPublicationRegistry skills;
    publish_skill_source(*workflow, fixture.registry_->exposed_tools(), skills, kWorkflowAlpha,
                         "fixture.proc", "episode path source", "delta.lookup", {1, 0, 0});
    must(workflow->sync_skill_procedure_index(skills), "sync");
    MIRA_CHECK(memory->stored_records() == 1);

    // The settlement-time episode loop keeps working after procedure syncs:
    // a Completed Strict run records its episode into the same memory.
    const WorkflowDefinition episode_definition =
        make_lookup_definition(kWorkflowGamma, SchemaVersion{1, 0}, "episode path run");
    const auto created =
        must(workflow->create_run(episode_definition, json_or_abort(R"json({"key":"k"})json"),
                                  std::nullopt),
             "episode create");
    const auto driven =
        must(workflow->execute_run(created.run_id, plain_context()), "episode drive");
    MIRA_CHECK(driven.state == WorkflowRunState::Completed);
    MIRA_CHECK(memory->stored_records() == 2);
    const auto records = memory->snapshot();
    MIRA_CHECK(std::any_of(records.begin(), records.end(), [](const MemoryRecord &record) {
        return record.kind == MemoryKind::Episode;
    }));
    const auto events =
        typed_events(*fixture.events_, fixture.session_id_, "WorkflowEpisodeRecorded");
    MIRA_CHECK(events.size() == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// G6: consumer closure surface (M7-TR2-G6; consumer binary run via ctest)
// ---------------------------------------------------------------------------

int g6_public_surface_closure() {
    // The new public vocabulary is self-describing and the degraded event
    // round trip is usable from a standalone TU (the full closure runs as the
    // mira_minimal_consumer test).
    MIRA_CHECK(kWorkflowToolRefsSchema == "mira.workflow.tool_refs.v1");
    MIRA_CHECK(kWorkflowToolCompatSchema == "mira.workflow.tool_compat.v1");
    MIRA_CHECK(kToolReferenceScheme == "toolref:");
    MIRA_CHECK(is_workflow_event_type("WorkflowToolCompatDegraded"));

    WorkflowRuntimeConfig defaults;
    MIRA_CHECK(defaults.max_mounted_tool_refs == 1024);
    MIRA_CHECK(defaults.max_skill_call_depth == 2);

    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::PinnedDigest) == "pinned_digest");
    MIRA_CHECK(tool_reference_mode_name(ToolReferenceMode::FollowLatest) == "follow_latest");
    MIRA_CHECK(parse_tool_reference_mode("follow_latest").has_value());

    // Degraded event round trip over a pure projection.
    const std::vector<ExposedToolSpec> view = hand_view();
    const WorkflowDefinition definition =
        make_v11_definition(kWorkflowAlpha, "closure v1.1", "toolref:delta.lookup");
    const auto refs = extract_or_abort(definition, view);
    const auto projection =
        must(project_workflow_tool_compatibility(definition, refs, view), "closure projection");
    MIRA_CHECK(projection.state == WorkflowToolCompatState::Runnable);
    WorkflowToolCompatDegradedEvent event;
    event.run_id = WorkflowRunId::generate();
    event.workflow_id = definition.workflow_id;
    event.ir_digest = workflow_definition_digest(definition);
    event.projection = workflow_tool_compat_to_json(projection);
    const auto payload = to_event_payload(event);
    MIRA_CHECK(payload.type == "WorkflowToolCompatDegraded");
    const auto parsed = must(parse_workflow_tool_compat_degraded(payload), "closure parse");
    MIRA_CHECK(parsed.run_id == event.run_id);
    const auto decoded = must(workflow_tool_compat_from_json(parsed.projection), "closure inverse");
    MIRA_CHECK(projections_equal(decoded, projection));

    // Procedures-synced payload round trip over the public surface.
    MIRA_CHECK(is_workflow_event_type("WorkflowProceduresSynced"));
    WorkflowProceduresSyncedEvent synced;
    synced.procedure_index_digest = digest_string("m7-tr2/closure/sync");
    synced.published_count = 3;
    const auto synced_payload = to_event_payload(synced);
    MIRA_CHECK(synced_payload.type == "WorkflowProceduresSynced");
    MIRA_CHECK(synced_payload.classification == EventClass::State);
    const auto synced_parsed =
        must(parse_workflow_procedures_synced(synced_payload), "synced closure parse");
    MIRA_CHECK(synced_parsed.procedure_index_digest == synced.procedure_index_digest);
    MIRA_CHECK(synced_parsed.published_count == 3);
    // Unknown fields fail closed.
    {
        JsonValue mutated = must(parse_json(synced_payload.data), "synced payload parse");
        mutated.set("extra", JsonValue{true});
        EventPayload tampered;
        tampered.type = synced_payload.type;
        tampered.data = to_json_string(mutated);
        MIRA_CHECK(!parse_workflow_procedures_synced(tampered).has_value());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    // --report [path]: canonical report of the fixed scenarios for
    // cross-process byte comparison; no other output in this mode.
    if (argc > 1 && std::string(argv[1]) == "--report") {
        const std::string report = build_skill_wiring_report();
        if (argc > 2) {
            std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "cannot open report path: " << argv[2] << '\n';
                return 2;
            }
            out << report << '\n';
            return out.good() ? 0 : 2;
        }
        std::cout << report << '\n';
        return 0;
    }

    struct Gate {
        const char *name;
        int (*run)();
    };
    const Gate gates[] = {
        {"G4 registration surface and identity gate", g4_registration_surface_and_identity_gate},
        {"G4 skill execution outcomes", g4_skill_execution_outcomes},
        {"G4 skill depth matrix", g4_skill_depth_matrix},
        {"G4 skill child passes compat gate", g4_skill_child_passes_compat_gate},
        {"G5 procedure sync wiring", g5_procedure_sync_wiring},
        {"G5 episode path intact", g5_episode_path_intact},
        {"G6 public surface closure", g6_public_surface_closure},
    };

    std::cout << "M7 TR2 skill and procedure wiring verification ("
              << sizeof(gates) / sizeof(gates[0]) << " gates)\n";
    for (const Gate &gate : gates) {
        if (gate.run() != 0) {
            std::cerr << "FAILED gate: " << gate.name << '\n';
            return 1;
        }
        std::cout << "[ok] " << gate.name << '\n';
    }
    std::cout << "all gates passed\n";
    return 0;
}
