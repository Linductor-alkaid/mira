// M28 (DEC-068) input admission and stage separation - gate M28-G1
// (milestone plan §6.1: the four stages are each observable, the stages do
// not jump implicitly, and a rejection carries a stable error code).
//
// The stage machine under test:
//
//   Accepted -> Routed -> Answered -> TaskSettled
//                  \-> Rejected (explicit)
//
// The frozen surface pinned here:
//   - the closed InputStage set and its names;
//   - the durable ledger partition and the mira.interaction.*.v1 event
//     family, which carries the stage transitions in order;
//   - the mira.interaction error domain and its stable domain codes.

#include "m28_support.hpp"

#include <mira/json.hpp>

#include <string>
#include <vector>

namespace mira::m28 {
namespace {

// The stage names of the closed set.
[[nodiscard]] bool stage_names_are_frozen() {
    return input_stage_name(InputStage::Accepted) == "Accepted" &&
           input_stage_name(InputStage::Routed) == "Routed" &&
           input_stage_name(InputStage::Answered) == "Answered" &&
           input_stage_name(InputStage::TaskSettled) == "TaskSettled" &&
           input_stage_name(InputStage::Rejected) == "Rejected";
}

} // namespace

int run() {
    if (!stage_names_are_frozen()) {
        return 1;
    }

    CoordinatorFixture fixture;
    const auto started = fixture.start();
    if (!started) {
        return 1;
    }
    const auto principal = make_principal();

    // --- the accepted stage is registered before the routing stage --------
    const auto first =
        fixture.submit(fixture.envelope("input-1", "summarise the weekly report", principal));
    if (!first) {
        return 1;
    }
    if (first.value().stage != InputStage::Routed) {
        std::cerr << "the first input must be routed\n";
        return 1;
    }
    if (first.value().input_id != "input-1") {
        return 1;
    }
    if (first.value().ingress_id.is_nil()) {
        return 1;
    }
    if (!first.value().task_id.has_value() || !first.value().session_id.has_value()) {
        return 1;
    }

    const auto types = fixture.ledger_event_types();
    if (types.size() != 2 || types[0] != "InputAccepted" || types[1] != "InputRouted") {
        std::cerr << "the ledger must record the acceptance before the routing\n";
        return 1;
    }

    // --- the answered and the settled stage are separately observable ----
    const auto answered = fixture.coordinator_->record_answer("input-1", "the report is ready");
    if (!answered || answered.value().stage != InputStage::Answered) {
        return 1;
    }
    const auto settled = fixture.coordinator_->record_settlement("input-1", TaskState::Completed);
    if (!settled || settled.value().stage != InputStage::TaskSettled) {
        return 1;
    }
    const auto stages = fixture.ledger_event_types();
    if (stages.size() != 4) {
        return 1;
    }
    if (stages[0] != "InputAccepted" || stages[1] != "InputRouted" ||
        stages[2] != "InputAnswered" || stages[3] != "InputTaskSettled") {
        std::cerr << "the stages must not jump: " << stages[0] << ',' << stages[1] << ','
                  << stages[2] << ',' << stages[3] << '\n';
        return 1;
    }

    // --- a rejection carries a stable error code -------------------------
    const auto denied = fixture.submit(
        fixture.envelope("input-2", "do something else", make_ungranted_principal()));
    if (!denied) {
        return 1;
    }
    if (denied.value().stage != InputStage::Rejected) {
        std::cerr << "an unpermitted input must be rejected\n";
        return 1;
    }
    if (!denied.value().error.has_value()) {
        return 1;
    }
    const auto &error = denied.value().error.value();
    if (error.domain != std::string(kInteractionErrorDomain)) {
        return 1;
    }
    if (error.domain_code != static_cast<std::int32_t>(InteractionDomainCode::PermissionDenied)) {
        return 1;
    }
    if (error.code != ErrorCode::PermissionDenied) {
        return 1;
    }

    // --- the rejection is durable and the stage never jumps --------------
    const auto record = fixture.coordinator_->input_record("input-2");
    if (!record || record.value().stage != InputStage::Rejected) {
        return 1;
    }
    const auto rejected_types = fixture.ledger_event_types();
    if (rejected_types.size() != 6) {
        return 1;
    }
    if (rejected_types[4] != "InputAccepted" || rejected_types[5] != "InputRejected") {
        std::cerr << "a rejection is recorded as an acceptance followed by a rejection\n";
        return 1;
    }

    // --- the payload of every event carries its own version marker -------
    for (const auto &payload : fixture.ledger_payloads()) {
        const auto parsed = parse_json(payload.data);
        if (!parsed || !parsed.value().is_object()) {
            return 1;
        }
        const auto *schema = parsed.value().find("schema");
        if (schema == nullptr || !schema->is_string()) {
            return 1;
        }
        const auto name = *schema->as_string();
        if (name.rfind("mira.interaction.", 0) != 0 || name.size() < 4 ||
            name.compare(name.size() - 3, 3, ".v1") != 0) {
            std::cerr << "every interaction event carries its own version marker\n";
            return 1;
        }
        if (parse_interaction_event_schema(name) == std::nullopt) {
            return 1;
        }
        if (!is_interaction_event_type(payload.type)) {
            return 1;
        }
    }

    // --- an empty input id is refused before anything is registered ------
    const auto empty = fixture.submit(fixture.envelope("", "no id", principal));
    if (empty.has_value()) {
        return 1;
    }
    if (empty.error().domain != std::string(kInteractionErrorDomain)) {
        return 1;
    }

    return 0;
}

} // namespace mira::m28

int main() { return mira::m28::run(); }
