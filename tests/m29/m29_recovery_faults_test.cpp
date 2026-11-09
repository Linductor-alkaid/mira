#include "m29_recovery_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

namespace {
int delivery_windows() {
    for (const auto marker : {"Dispatching", "Delivered"}) {
        for (const bool lost_ack : {false, true}) {
            RecoveryRoot root;
            const auto principal = make_principal();
            TaskId task;
            auto sink = std::make_shared<RecordingSink>();
            {
                RecoveryHost host(root.path);
                auto fault = std::make_shared<FaultEventStore>(host.events);
                MIRA_CHECK(host.start(fault));
                const auto input = host.create("goal", "do not repeat delivery", principal);
                MIRA_CHECK(input);
                task = *input.value().task_id;
                fault->after_write = lost_ack;
                fault->fail_when = [marker](const auto &request) {
                    return routed_with(request, marker);
                };
                MIRA_CHECK(!host.coordinator().register_sink(task, sink));
                const auto expected_deliveries = std::string_view(marker) == "Delivered" ? 1U : 0U;
                MIRA_CHECK(sink->deliveries.size() == expected_deliveries);
                MIRA_CHECK(!host.coordinator().register_sink(task, sink));
                MIRA_CHECK(sink->deliveries.size() == expected_deliveries);
                const auto fenced = host.coordinator().submit_input(
                    host.input("fenced", "later input", principal, task));
                MIRA_CHECK(fenced && fenced.value().stage == InputStage::Rejected);
                MIRA_CHECK(host.coordinator().close());
            }
            {
                RecoveryHost host(root.path);
                MIRA_CHECK(host.start());
                const auto entry = host.coordinator().input_record("goal");
                MIRA_CHECK(entry);
                const bool possibly_delivered = std::string_view(marker) == "Delivered" || lost_ack;
                if (possibly_delivered)
                    MIRA_CHECK(entry.value().delivery == InputDeliveryState::Dispatching ||
                               entry.value().delivery == InputDeliveryState::Delivered);
                else
                    MIRA_CHECK(entry.value().delivery == InputDeliveryState::NotDispatched);
                MIRA_CHECK(!host.coordinator().resume_input(principal, "goal"));
                const auto report = host.coordinator().recover_work(
                    principal, task, host.core.environment_, recovery_context());
                MIRA_CHECK(report && report.value().ready_for_new_task != possibly_delivered);
                MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
            }
        }
    }
    return 0;
}

int route_ack_loss_and_binding_fence() {
    RecoveryRoot root;
    const auto principal = make_principal();
    {
        RecoveryHost host(root.path);
        auto fault = std::make_shared<FaultEventStore>(host.events);
        MIRA_CHECK(host.start(fault));
        fault->after_write = true;
        fault->fail_when = [](const auto &request) {
            return routed_with(request, "NotDispatched", "route");
        };
        MIRA_CHECK(!host.create("route", "one durable binding", principal));
        const auto retry = host.create("route", "one durable binding", principal);
        MIRA_CHECK(retry && retry.value().stage == InputStage::Rejected);
        MIRA_CHECK(host.coordinator().close());
    }
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto page = host.coordinator().replay_inputs(principal);
        MIRA_CHECK(page && page.value().entries.size() == 1);
        MIRA_CHECK(page.value().entries.front().stage == InputStage::Routed &&
                   page.value().entries.front().task_id);
        const auto task = *page.value().entries.front().task_id;
        MIRA_CHECK(host.coordinator().principal_state(principal).value().tasks.front().historical);
        const auto report = host.coordinator().recover_work(principal, task, host.core.environment_,
                                                            recovery_context());
        MIRA_CHECK(report && report.value().ready_for_new_task);
        const auto resumed =
            host.coordinator().submit_input(host.input("new-work", "continue", principal, task));
        MIRA_CHECK(resumed && resumed.value().task_id != task);
    }
    return 0;
}

int clarification_joint_commit_repair() {
    for (bool lost_ack : {false, true}) {
        RecoveryRoot root;
        const auto principal = make_principal();
        TaskId task;
        InputEnvelope answer;
        {
            RecoveryHost host(root.path);
            auto fault = std::make_shared<FaultEventStore>(host.events);
            MIRA_CHECK(host.start(fault));
            const auto a = host.create("a", "first work", principal);
            MIRA_CHECK(a && host.create("b", "second work", principal));
            task = *a.value().task_id;
            MIRA_CHECK(host.coordinator()
                           .submit_input(host.input("question", "original details", principal))
                           .value()
                           .clarification);
            answer = host.input("answer", "first work", principal, task);
            answer.clarification_for = "question";
            fault->after_write = lost_ack;
            fault->fail_when = [](const auto &request) {
                return routed_with(request, "NotRequired", "answer");
            };
            MIRA_CHECK(!host.coordinator().submit_input(answer));
            MIRA_CHECK(host.coordinator().input_record("question").value().resolved_by == "answer");
            MIRA_CHECK(host.coordinator().close());
        }
        {
            RecoveryHost host(root.path);
            MIRA_CHECK(host.start());
            const auto repaired = host.coordinator().submit_input(answer);
            MIRA_CHECK(repaired && repaired.value().duplicate &&
                       repaired.value().stage == InputStage::Routed);
            MIRA_CHECK(host.coordinator().input_record("answer").value().delivery ==
                       InputDeliveryState::NotRequired);
            MIRA_CHECK(!host.core.runtime_->task_snapshot(task)); // No implicit recreation.
            const auto report = host.coordinator().recover_work(
                principal, task, host.core.environment_, recovery_context());
            MIRA_CHECK(report && report.value().ready_for_new_task);
            const auto new_work =
                host.coordinator().submit_input(host.input("resume", "continue", principal, task));
            MIRA_CHECK(new_work && new_work.value().stage == InputStage::Routed);
            auto sink = std::make_shared<RecordingSink>();
            MIRA_CHECK(host.coordinator().register_sink(*new_work.value().task_id, sink));
            MIRA_CHECK(sink->deliveries.size() == 1);
            MIRA_CHECK(sink->deliveries.front().content.find("original details") !=
                       std::string::npos);
            MIRA_CHECK(host.coordinator().submit_input(answer).value().duplicate);
            MIRA_CHECK(sink->deliveries.size() == 1);
        }
    }
    return 0;
}
int accepted_input_resume_and_lost_acceptance_ack() {
    for (bool lost_acceptance : {false, true}) {
        RecoveryRoot root;
        const auto principal = make_principal();
        {
            RecoveryHost host(root.path);
            auto fault = std::make_shared<FaultEventStore>(host.events);
            MIRA_CHECK(host.start(fault));
            fault->after_write = lost_acceptance;
            fault->fail_when = [lost_acceptance](const auto &request) {
                return lost_acceptance ? request.payload.type == "InputAccepted"
                                       : routed_with(request, "NotDispatched", "accepted");
            };
            const auto failed = host.create("accepted", "preserve this accepted body", principal);
            MIRA_CHECK(!failed || failed.value().stage == InputStage::Rejected);
            MIRA_CHECK(host.coordinator().close());
        }
        {
            RecoveryHost host(root.path);
            MIRA_CHECK(host.start());
            const auto entry = host.coordinator().input_record("accepted");
            MIRA_CHECK(entry && entry.value().stage == InputStage::Accepted &&
                       entry.value().input_ref);
            const auto resumed = host.coordinator().resume_input(principal, "accepted");
            MIRA_CHECK(resumed && resumed.value().stage == InputStage::Routed &&
                       resumed.value().task_id);
            auto sink = std::make_shared<RecordingSink>();
            MIRA_CHECK(host.coordinator().register_sink(*resumed.value().task_id, sink));
            MIRA_CHECK(sink->deliveries.size() == 1);
            MIRA_CHECK(sink->deliveries.front().content == "preserve this accepted body");
            MIRA_CHECK(!host.coordinator().resume_input(principal, "accepted"));
            MIRA_CHECK(sink->deliveries.size() == 1);
        }
    }
    return 0;
}
int reducer_guards_the_commit_window() {
    for (const auto marker : {"RecoveryObservationRecorded", "RecoveryVerificationResult"}) {
        RecoveryRoot root;
        const auto principal = make_principal();
        TaskId task;
        SessionId session;
        {
            RecoveryHost host(root.path);
            MIRA_CHECK(host.start());
            const auto initial = host.create("goal", "guard the source prefix", principal);
            MIRA_CHECK(initial);
            task = *initial.value().task_id;
            session = *initial.value().session_id;
            MIRA_CHECK(seed_uncertain(host, task, session));
            MIRA_CHECK(host.coordinator().close());
        }
        RecoveryHost host(root.path);
        auto proxy = std::make_shared<FaultEventStore>(host.events);
        MIRA_CHECK(host.start(proxy));
        bool injected = false;
        bool once = true;
        proxy->before_append = [&](const AppendRequest &request) {
            if (once && request.payload.type == marker) {
                once = false;
                injected = seed_uncertain(host, task, session).has_value();
            }
        };
        const auto stale = host.coordinator().recover_work(
            principal, task, host.core.environment_, recovery_context(),
            [](const WorkRecoveryReport &, const Observation &,
               const OperationContext &) -> Result<bool> { return true; });
        MIRA_CHECK(injected && !stale &&
                   stale.error().domain_code ==
                       static_cast<std::int32_t>(InteractionDomainCode::RecoveryStale));
        RecoveryPlanner planner(*host.events, *host.checkpoints);
        const auto pinned = planner.plan(task, session, Timestamp::now());
        MIRA_CHECK(pinned && pinned.value().pending_side_effects.size() == 2);
        MIRA_CHECK(pinned.value().checkpoint->verification_pending);
        MIRA_CHECK(host.checkpoints->put(*pinned.value().checkpoint));
        const auto verified = host.coordinator().recover_work(
            principal, task, host.core.environment_, recovery_context(),
            [](const WorkRecoveryReport &, const Observation &,
               const OperationContext &) -> Result<bool> { return true; });
        MIRA_CHECK(verified && verified.value().ready_for_new_task);
        MIRA_CHECK(verified.value().recovery.checkpoint->recovery_verification_sequence != 0);
        const auto json = checkpoint_to_json(*verified.value().recovery.checkpoint);
        const auto restored = checkpoint_from_json(json);
        MIRA_CHECK(restored && restored.value().projection_digest() ==
                                   verified.value().recovery.checkpoint->projection_digest());
        MIRA_CHECK(host.core.environment_->executed_event_count() == 0);
    }
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(delivery_windows() == 0);
    MIRA_CHECK(reducer_guards_the_commit_window() == 0);
    MIRA_CHECK(accepted_input_resume_and_lost_acceptance_ack() == 0);
    MIRA_CHECK(route_ack_loss_and_binding_fence() == 0);
    MIRA_CHECK(clarification_joint_commit_repair() == 0);
    return 0;
}
