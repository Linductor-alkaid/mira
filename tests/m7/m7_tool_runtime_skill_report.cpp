#include "tr2_wiring_support.hpp"

namespace mira::testing {

// Canonical output for the test binary's --report mode.
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
}

} // namespace mira::testing
