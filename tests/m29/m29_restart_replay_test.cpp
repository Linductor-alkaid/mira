#include "m29_recovery_support.hpp"
#include "support/conversation_loop_support.hpp"
#include <mira/model_context_admission.hpp>

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

int main() {
    RecoveryRoot root;
    const auto principal = make_principal();
    const auto foreign = make_principal();
    TaskId task;
    ArtifactDescriptor erased_body;
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto first = host.create("initial", "restore the original goal", principal);
        MIRA_CHECK(first);
        task = *first.value().task_id;
        MIRA_CHECK(host.coordinator().submit_input(
            host.input("correction", "keep the latest constraint", principal, task)));
        MIRA_CHECK(host.create("foreign", "private foreign work", foreign));
        erased_body = *host.coordinator().input_record("initial").value().input_ref;
        MIRA_CHECK(host.coordinator().close());
    }
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        auto state = host.coordinator().principal_state(principal);
        MIRA_CHECK(state && state.value().tasks.size() == 1);
        MIRA_CHECK(state.value().tasks.front().historical);
        MIRA_CHECK(state.value().tasks.front().context_block.find("keep the latest constraint") !=
                   std::string::npos);
        MIRA_CHECK(!host.coordinator().register_sink(task, std::make_shared<RecordingSink>()));
        const auto page = host.coordinator().replay_inputs(principal, 0, 1);
        MIRA_CHECK(page && page.value().entries.size() == 1 && page.value().has_more);
        MIRA_CHECK(page.value().entries.front().input_id == "initial");
        const auto next =
            host.coordinator().replay_inputs(principal, page.value().next_sequence, 1);
        MIRA_CHECK(next && next.value().entries.size() == 1 && !next.value().has_more);
        MIRA_CHECK(next.value().entries.front().input_id == "correction");
        MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
        auto revoked = principal;
        revoked.grants.clear();
        MIRA_CHECK(!host.coordinator().replay_inputs(revoked));
        MIRA_CHECK(!host.coordinator().replay_inputs(principal, 0, 65));
        MIRA_CHECK(!host.coordinator().resume_input(principal, "initial"));
        const auto needs = host.coordinator().submit_input(
            host.input("needs-recovery", "continue", principal, task));
        MIRA_CHECK(needs && needs.value().stage == InputStage::Rejected);
        MIRA_CHECK(needs.value().error->domain_code ==
                   static_cast<std::int32_t>(InteractionDomainCode::RecoveryRequired));
        const auto recovered = host.coordinator().recover_work(
            principal, task, host.core.environment_, recovery_context());
        MIRA_CHECK(recovered && recovered.value().ready_for_new_task);
        const auto continued = host.coordinator().submit_input(
            host.input("continued", "now finish it", principal, task));
        MIRA_CHECK(continued && continued.value().task_id != task);
        state = host.coordinator().principal_state(principal);
        const auto *created = find_task(state.value(), *continued.value().task_id);
        MIRA_CHECK(created && created->resumed_from == task && created->recovered_checkpoint);
        MIRA_CHECK(created->goal == "restore the original goal");
        MIRA_CHECK(created->context_block.find("keep the latest constraint") != std::string::npos);
        const auto candidates =
            host.coordinator().recovered_context_items(principal, *continued.value().task_id);
        MIRA_CHECK(candidates && candidates.value().size() == 1);
        MIRA_CHECK(!candidates.value().front().pinned &&
                   candidates.value().front().kind == ContextItemKind::CheckpointSummary);
        MIRA_CHECK(candidates.value().front().authority == ContextAuthority::UntrustedExternalData);
        {
            mira::testing::ConversationFixture model;
            model.spec_.task_id = *continued.value().task_id;
            model.spec_.session_id = *continued.value().session_id;
            model.spec_.task_epoch = 0;
            model.spec_.goal = created->goal;
            model.use_provider({mira::testing::text_response("restored")});
            auto loop = model.make_loop({}, false);
            model.gateway_->set_context_supplier(
                [&](const ModelRequest &request,
                    const OperationContext &) -> Result<ModelContextSnapshot> {
                    auto items =
                        host.coordinator().recovered_context_items(principal, request.task_id);
                    if (!items)
                        return items.error();
                    ModelContextSnapshot snapshot;
                    snapshot.candidates = std::move(items).value();
                    return snapshot;
                });
            MIRA_CHECK(host.coordinator().register_sink(
                model.spec_.task_id, std::make_shared<ConversationLoopInputSink>(loop)));
            const auto answer = loop.run(model.spec_, mira::testing::plain_loop_context());
            MIRA_CHECK(answer && answer.value().answer == "restored");
            const auto requests = model.provider_->requests();
            MIRA_CHECK(requests.size() == 1);
            MIRA_CHECK(model.provider_->request_text(0).find("Historical checkpoint") !=
                       std::string::npos);
            // The checkpoint is an optional P3 item, not pasted into the
            // protected primary user envelope. Layer 0 may discard it.
            ModelRequest base = requests.front();
            MIRA_CHECK(base.input.size() == 3);
            base.input.pop_back();
            ModelContextAdmissionConfig config;
            config.safety_margin_tokens = 0;
            auto profile = *model.profile_;
            const auto minimum =
                admit_model_request(base, profile, model.spec_.session_id, {}, config);
            MIRA_CHECK(minimum);
            profile.capabilities.limits.max_context_tokens =
                minimum.value().context.total_estimate.upper_bound +
                base.generation.max_output_tokens.value() +
                ConservativeTokenCounter{}.estimate_assembly_overhead(0, 0) + 1;
            ModelContextSnapshot optional;
            optional.candidates = candidates.value();
            const auto tight =
                admit_model_request(base, profile, model.spec_.session_id, optional, config);
            MIRA_CHECK(tight && tight.value().request.input.size() == base.input.size());
            MIRA_CHECK(tight.value().context.item_audit.back().disposition ==
                       ContextItemDisposition::Dropped);
        }
        const auto consumed =
            host.coordinator().submit_input(host.input("second-resume", "again", principal, task));
        MIRA_CHECK(consumed && consumed.value().stage == InputStage::Rejected);
        MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
        MIRA_CHECK(host.artifacts->erase({erased_body.id, "fixture erase original source"}));
        MIRA_CHECK(host.coordinator().close());
    }
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto state = host.coordinator().principal_state(principal);
        MIRA_CHECK(state);
        for (const auto &work : state.value().tasks)
            MIRA_CHECK(work.recovery_availability == RecoveryAvailability::MissingOrErased);
        MIRA_CHECK(!host.coordinator().recover_work(principal, task, host.core.environment_,
                                                    recovery_context()));
    }
    {
        RecoveryRoot expired_root;
        CoordinatorConfig config;
        config.idempotency_window = std::chrono::seconds(0);
        {
            RecoveryHost host(expired_root.path, config);
            MIRA_CHECK(host.start());
            MIRA_CHECK(host.create("expired", "expired history", principal));
            MIRA_CHECK(host.coordinator().close());
        }
        RecoveryHost host(expired_root.path, config);
        MIRA_CHECK(host.start());
        const auto state = host.coordinator().principal_state(principal);
        MIRA_CHECK(state && state.value().tasks.front().recovery_availability ==
                                RecoveryAvailability::Expired);
        const auto page = host.coordinator().replay_inputs(principal);
        MIRA_CHECK(page && page.value().entries.empty() && page.value().expired_gap);
        MIRA_CHECK(!host.coordinator().resume_input(principal, "expired"));
    }
    {
        RecoveryRoot bounded_root;
        CoordinatorConfig config;
        config.max_recovery_artifact_bytes = 128;
        RecoveryHost host(bounded_root.path, config);
        MIRA_CHECK(host.start());
        const auto large = host.create("large", std::string(1024, 'x'), principal);
        MIRA_CHECK(large && large.value().stage == InputStage::Rejected);
        MIRA_CHECK(host.coordinator().principal_state(principal).value().tasks.empty());
        MIRA_CHECK(host.coordinator().replay_inputs(principal).value().entries.empty());
    }
    return 0;
}
