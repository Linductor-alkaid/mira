// M28 (DEC-068) result association - gate M28-G4
// (milestone plan §6.1: a reply, a follow-up question, a background result
// and a failure are all traceable to the input id and the task id, a late
// result never claims another input, and the new events carry their own
// version marker).
//
// The frozen surface pinned here:
//   - the mira.interaction.*.v1 event family is a closed set and every
//     member carries its own version marker;
//   - every interaction event carries the input id and, where it applies,
//     the task id and the session id;
//   - a late report of an earlier stage never rewinds the record;
//   - the interaction events live in their own principal-level partition and
//     do not disturb the conversation view or the checkpoint projection.

#include "m28_support.hpp"

#include <mira/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace mira::m28 {

int run() {
    // --- the event family is a closed set ---------------------------------
    const std::vector<std::pair<std::string, std::string>> kFamily{
        {"InputAccepted", "mira.interaction.input-accepted.v1"},
        {"InputRouted", "mira.interaction.input-routed.v1"},
        {"InputClarified", "mira.interaction.input-clarified.v1"},
        {"InputRejected", "mira.interaction.input-rejected.v1"},
        {"InputAnswered", "mira.interaction.input-answered.v1"},
        {"InputTaskSettled", "mira.interaction.input-task-settled.v1"},
    };
    for (std::size_t index = 0; index < kFamily.size(); ++index) {
        const auto type = static_cast<InteractionEventType>(index);
        if (interaction_event_schema(type) != kFamily[index].second) {
            return 1;
        }
        const auto parsed = parse_interaction_event_schema(kFamily[index].second);
        if (!parsed || *parsed != type) {
            return 1;
        }
        if (!is_interaction_event_type(kFamily[index].first)) {
            return 1;
        }
    }
    if (parse_interaction_event_schema("mira.workflow.run-started.v1").has_value()) {
        return 1;
    }
    if (is_interaction_event_type("WorkflowRunStarted")) {
        return 1;
    }

    CoordinatorFixture fixture;
    const auto started = fixture.start();
    if (!started) {
        return 1;
    }
    const auto principal = make_principal();

    // --- the reply is traceable to the input and the task -----------------
    const auto submitted =
        fixture.submit(fixture.envelope("input-1", "open the report", principal));
    if (!submitted) {
        return 1;
    }
    const auto task = submitted.value().task_id.value();
    const auto session = submitted.value().session_id.value();

    const auto answered = fixture.coordinator_->record_answer("input-1", "the report is open");
    if (!answered) {
        return 1;
    }
    if (answered.value().input_id != "input-1") {
        return 1;
    }
    if (answered.value().task_id.value() != task) {
        return 1;
    }
    if (answered.value().session_id.value() != session) {
        return 1;
    }

    // --- a late report of an earlier stage never rewinds the record -------
    const auto late = fixture.coordinator_->record_answer("input-1", "a late duplicate reply");
    if (!late) {
        return 1;
    }
    if (late.value().stage != InputStage::Answered) {
        std::cerr << "a late report must not rewind the recorded stage\n";
        return 1;
    }

    // --- a late result never claims another input -------------------------
    const auto unknown = fixture.coordinator_->record_answer("input-other", "not mine");
    if (unknown.has_value()) {
        std::cerr << "a result for an unknown input must be refused\n";
        return 1;
    }
    if (unknown.error().domain != std::string(kInteractionErrorDomain)) {
        return 1;
    }
    const auto unknown_settlement =
        fixture.coordinator_->record_settlement("input-other", TaskState::Completed);
    if (unknown_settlement.has_value()) {
        return 1;
    }

    // --- the background result is traceable too ----------------------------
    const auto settled = fixture.coordinator_->record_settlement("input-1", TaskState::Completed);
    if (!settled || settled.value().stage != InputStage::TaskSettled) {
        return 1;
    }
    if (settled.value().task_id.value() != task) {
        return 1;
    }

    // --- every interaction event carries its identity ----------------------
    for (const auto &payload : fixture.ledger_payloads()) {
        const auto parsed = parse_json(payload.data);
        if (!parsed || !parsed.value().is_object()) {
            return 1;
        }
        const auto *input_id = parsed.value().find("input_id");
        if (input_id == nullptr || !input_id->is_string()) {
            std::cerr << "every interaction event carries the input id\n";
            return 1;
        }
        if (input_id->as_string()->empty()) {
            return 1;
        }
        const auto *ingress = parsed.value().find("ingress_id");
        if (ingress == nullptr || !ingress->is_string()) {
            return 1;
        }
    }

    // --- the routed event carries the ownership ----------------------------
    const auto payloads = fixture.ledger_payloads();
    const auto routed =
        std::find_if(payloads.begin(), payloads.end(),
                     [](const EventPayload &payload) { return payload.type == "InputRouted"; });
    if (routed == payloads.end()) {
        return 1;
    }
    const auto routed_json = parse_json(routed->data);
    if (!routed_json) {
        return 1;
    }
    const auto *task_member = routed_json.value().find("task_id");
    const auto *session_member = routed_json.value().find("session_id");
    const auto *intent_member = routed_json.value().find("intent");
    if (task_member == nullptr || !task_member->is_string() ||
        *task_member->as_string() != task.to_string()) {
        return 1;
    }
    if (session_member == nullptr || !session_member->is_string() ||
        *session_member->as_string() != session.to_string()) {
        return 1;
    }
    if (intent_member == nullptr || !intent_member->is_string() ||
        intent_member->as_string()->empty()) {
        return 1;
    }

    // --- the interaction events live in their own partition ----------------
    // The session stream of the routed task stays empty of interaction
    // events: the interaction lifecycle is a principal-level stream, and the
    // conversation view and the checkpoint projection never see it.
    EventQuery query;
    query.session_id = session;
    const auto session_page = fixture.events_->read(query);
    if (!session_page || !session_page.value().events.empty()) {
        return 1;
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
