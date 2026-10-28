// M28 (DEC-068) context ownership - gate M28-G5
// (milestone plan §6.1: an inserted correction updates the target task
// only, and an unrelated input never pollutes the constraints of another
// task).
//
// The frozen surface pinned here:
//   - the latest user constraints belong to one work item (DEC-068 §6);
//   - a correction is delivered to the target task only;
//   - the prepared context block of a task carries its own goal and its own
//   constraints;
//   - a task created through a ResumeTask decision inherits the constraints
//   of the settled task it resumes, and no other task's constraints.

#include "m28_support.hpp"

#include <mira/interaction_coordinator.hpp>

#include <string>
#include <vector>

namespace mira::m28 {

int run() {
    CoordinatorFixture fixture;
    const auto started = fixture.start();
    if (!started) {
        return 1;
    }
    const auto principal = make_principal();

    // --- two independent work items ---------------------------------------
    const auto first = fixture.submit(fixture.envelope("input-1", "write the summary", principal));
    if (!first) {
        return 1;
    }
    const auto first_task = first.value().task_id.value();

    fixture.resolver = [](const InputEnvelope &,
                          const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::NewTask;
        return suggestion;
    };
    const auto second =
        fixture.submit(fixture.envelope("input-2", "draft the cover letter", principal));
    if (!second) {
        return 1;
    }
    const auto second_task = second.value().task_id.value();
    if (second_task == first_task) {
        return 1;
    }

    // --- a correction updates the target task only ------------------------
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::SupplementTask;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto correction =
        fixture.submit(fixture.envelope("input-3", "keep it under one page", principal));
    if (!correction || correction.value().stage != InputStage::Routed) {
        return 1;
    }
    if (correction.value().task_id.value() != first_task) {
        return 1;
    }

    const auto state = fixture.coordinator_->principal_state(principal);
    if (!state) {
        return 1;
    }
    const auto *corrected = find_task(state.value(), first_task);
    const auto *untouched = find_task(state.value(), second_task);
    if (corrected == nullptr || untouched == nullptr) {
        return 1;
    }
    if (corrected->constraints.size() != 1) {
        std::cerr << "the corrected task must carry exactly the new constraint\n";
        return 1;
    }
    if (corrected->constraints.front() != "keep it under one page") {
        return 1;
    }
    if (!untouched->constraints.empty()) {
        std::cerr << "an unrelated input must not pollute another task's constraints\n";
        return 1;
    }
    // The correction is recorded on the target task only.
    if (corrected->input_ids.size() != 2 || corrected->input_ids.front() != "input-1" ||
        corrected->input_ids.back() != "input-3") {
        return 1;
    }
    if (untouched->input_ids.size() != 1 || untouched->input_ids.front() != "input-2") {
        return 1;
    }

    // --- the prepared context block belongs to its own task ----------------
    if (corrected->context_block.find("keep it under one page") == std::string::npos) {
        std::cerr << "the prepared context of the target task carries its constraint\n";
        return 1;
    }
    if (corrected->context_block.find("draft the cover letter") != std::string::npos) {
        return 1;
    }
    if (untouched->context_block.find("keep it under one page") != std::string::npos) {
        std::cerr << "the context of an unrelated task must not carry the correction\n";
        return 1;
    }

    // --- the correction reaches the target task only -----------------------
    auto sink = std::make_shared<RecordingSink>();
    const auto registered = fixture.coordinator_->register_sink(first_task, sink);
    if (!registered) {
        return 1;
    }
    // The corrections that arrived before the sink was bound are delivered on
    // the registration, so the target task never loses an input.
    if (sink->deliveries.size() != 2 || sink->deliveries.front().input_id != "input-1" ||
        sink->deliveries.back().input_id != "input-3") {
        std::cerr << "the pending corrections must be delivered on the registration\n";
        return 1;
    }
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::SupplementTask;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto second_correction =
        fixture.submit(fixture.envelope("input-4", "and use the summary", principal));
    if (!second_correction) {
        return 1;
    }
    if (sink->deliveries.size() != 3) {
        std::cerr << "the correction must be delivered to the target task\n";
        return 1;
    }
    if (sink->deliveries.back().input_id != "input-4") {
        return 1;
    }
    if (sink->deliveries.back().task_id != first_task) {
        return 1;
    }
    for (const auto &delivery : sink->deliveries) {
        if (delivery.task_id != first_task) {
            std::cerr << "a delivery must never reach another task\n";
            return 1;
        }
    }

    // --- the constraint list is bounded ------------------------------------
    for (int index = 0; index < 20; ++index) {
        const auto id = "input-bulk-" + std::to_string(index);
        const auto bulk = fixture.submit(fixture.envelope(id, "another constraint", principal));
        if (!bulk) {
            return 1;
        }
    }
    const auto bounded = fixture.coordinator_->principal_state(principal);
    const auto *bounded_task = find_task(bounded.value(), first_task);
    if (bounded_task == nullptr) {
        return 1;
    }
    if (bounded_task->constraints.size() > 8) {
        std::cerr << "the constraint list must stay bounded\n";
        return 1;
    }

    // --- a resumed task inherits the constraints of the task it resumes ---
    const auto cancelled = fixture.coordinator_->cancel_work(first_task);
    if (!cancelled) {
        return 1;
    }
    (void)cancelled.value().outcome(std::chrono::seconds(2));
    fixture.resolver = [first_task](const InputEnvelope &,
                                    const PrincipalState &) -> std::optional<RouteSuggestion> {
        RouteSuggestion suggestion;
        suggestion.intent = RouteIntent::ResumeTask;
        suggestion.task_id = first_task;
        return suggestion;
    };
    const auto resumed = fixture.submit(fixture.envelope("input-5", "carry on later", principal));
    if (!resumed || resumed.value().stage != InputStage::Routed) {
        return 1;
    }
    const auto resumed_task = resumed.value().task_id.value();
    if (resumed_task == first_task) {
        std::cerr << "a resumed task must be a new task\n";
        return 1;
    }
    const auto resumed_state = fixture.coordinator_->principal_state(principal);
    const auto *resumed_entry = find_task(resumed_state.value(), resumed_task);
    if (resumed_entry == nullptr) {
        return 1;
    }
    if (!resumed_entry->resumed_from.has_value() ||
        resumed_entry->resumed_from.value() != first_task) {
        return 1;
    }
    if (resumed_entry->constraints != bounded_task->constraints) {
        std::cerr << "the resumed task inherits the constraints of the task it resumes\n";
        return 1;
    }
    // The task created through the runtime carries the constraints of its
    // creation.
    const auto snapshot = fixture.runtime_->task_snapshot(resumed_task);
    if (!snapshot) {
        return 1;
    }
    if (snapshot.value().user_constraints != resumed_entry->constraints) {
        return 1;
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
