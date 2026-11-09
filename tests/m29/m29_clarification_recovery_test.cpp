#include "m29_recovery_support.hpp"

using namespace mira;
using namespace mira::m28;
using namespace mira::m29;

int main() {
    RecoveryRoot root;
    const auto principal = make_principal();
    TaskId first, second;
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto a = host.create("first", "prepare report", principal);
        const auto b = host.create("second", "compare prices", principal);
        MIRA_CHECK(a && b);
        first = *a.value().task_id;
        second = *b.value().task_id;
        auto sink = std::make_shared<RecordingSink>();
        MIRA_CHECK(host.coordinator().register_sink(first, sink));
        const auto question = host.coordinator().submit_input(
            host.input("question", "include the original appendix", principal));
        MIRA_CHECK(question && question.value().clarification &&
                   question.value().candidates.size() == 2);
        auto answer = host.input("answer", "the report task", principal, first);
        answer.clarification_for = "question";
        const auto resolved = host.coordinator().submit_input(answer);
        MIRA_CHECK(resolved && resolved.value().stage == InputStage::Routed &&
                   resolved.value().task_id == first);
        MIRA_CHECK(sink->deliveries.size() == 2);
        MIRA_CHECK(sink->deliveries.back().input_id == "question");
        MIRA_CHECK(sink->deliveries.back().content.find("include the original appendix") !=
                   std::string::npos);
        MIRA_CHECK(sink->deliveries.back().content.find("the report task") != std::string::npos);
        const auto original = host.coordinator().input_record("question");
        const auto acknowledged = host.coordinator().input_record("answer");
        MIRA_CHECK(original && original.value().resolved_by == "answer");
        MIRA_CHECK(original.value().delivery == InputDeliveryState::Delivered);
        MIRA_CHECK(acknowledged &&
                   acknowledged.value().delivery == InputDeliveryState::NotRequired);
        MIRA_CHECK(host.coordinator().submit_input(answer).value().duplicate);
        MIRA_CHECK(sink->deliveries.size() == 2);
        answer.input_id = "competing-answer";
        answer.target_task = second;
        const auto competing = host.coordinator().submit_input(answer);
        MIRA_CHECK(competing && competing.value().stage == InputStage::Rejected);
        MIRA_CHECK(sink->deliveries.size() == 2);
        const auto restart_question = host.coordinator().submit_input(
            host.input("restart-question", "keep my original details after restart", principal));
        MIRA_CHECK(restart_question && restart_question.value().clarification);
        MIRA_CHECK(host.coordinator().close());
    }
    {
        RecoveryHost host(root.path);
        MIRA_CHECK(host.start());
        const auto restored = host.coordinator().principal_state(principal);
        MIRA_CHECK(restored && restored.value().tasks.size() == 2);
        MIRA_CHECK(restored.value().tasks[0].historical);
        MIRA_CHECK(!host.core.runtime_->task_snapshot(first));
        const auto question = host.coordinator().resume_input(principal, "restart-question");
        MIRA_CHECK(question && question.value().clarification &&
                   question.value().candidates.size() == 2);
        const auto report = host.coordinator().recover_work(
            principal, second, host.core.environment_, recovery_context());
        MIRA_CHECK(report && report.value().ready_for_new_task);
        auto answer = host.input("restart-answer", "the price comparison", principal, second);
        answer.clarification_for = "restart-question";
        const auto resumed = host.coordinator().submit_input(answer);
        MIRA_CHECK(resumed && resumed.value().stage == InputStage::Routed);
        MIRA_CHECK(resumed.value().task_id != second);
        auto sink = std::make_shared<RecordingSink>();
        MIRA_CHECK(host.coordinator().register_sink(*resumed.value().task_id, sink));
        MIRA_CHECK(sink->deliveries.size() == 1 &&
                   sink->deliveries.front().input_id == "restart-question");
        MIRA_CHECK(sink->deliveries.front().content.find(
                       "keep my original details after restart") != std::string::npos);
        MIRA_CHECK(host.coordinator().submit_input(answer).value().duplicate);
        MIRA_CHECK(sink->deliveries.size() == 1);
        MIRA_CHECK(!host.coordinator().resume_input(principal, "question"));
        auto invalid = host.input("foreign-answer", "report", make_principal(), first);
        invalid.clarification_for = "restart-question";
        MIRA_CHECK(host.coordinator().submit_input(invalid).value().stage == InputStage::Rejected);
    }
    return 0;
}
