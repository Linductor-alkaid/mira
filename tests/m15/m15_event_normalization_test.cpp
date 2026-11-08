#include "support/eval_event_normalization.hpp"
#include "support/test.hpp"

namespace {
using namespace mira::testing;

std::vector<EvalEventPayload> recorded(bool renamed = false) {
    const std::string run = renamed ? std::string(32, 'd') : std::string(32, 'a');
    const std::string step = renamed ? std::string(32, 'e') : std::string(32, 'b');
    const std::string task = renamed ? std::string(32, 'f') : std::string(32, 'c');
    return {{"WorkflowRunStarted", "{\"run_id\":\"" + run + "\"}"},
            {"WorkflowStepStarted",
             "{\"run_id\":\"" + run + "\",\"step_id\":\"" + step + "\",\"attempt\":2}"},
            {"WorkflowRecoveryAttempted",
             "{\"run_id\":\"" + run + "\",\"task_id\":\"" + task + "\",\"ordinal\":1}"},
            {"WorkflowStepSettled",
             "{\"run_id\":\"" + run + "\",\"step_id\":\"" + step + "\",\"attempt\":2}"}};
}

int canonical_identity_assignment_precedes_multiset_comparison() {
    const auto baseline = recorded();
    auto interleaved = recorded(true);
    std::swap(interleaved[1], interleaved[2]);
    // Same identities/relationships under a different random naming and
    // legal audit/worker interleaving must pass the workflow G4 gate.
    auto old_baseline = normalize_eval_events(baseline, false);
    auto old_interleaved = normalize_eval_events(interleaved, false);
    std::sort(old_baseline.begin(), old_baseline.end());
    std::sort(old_interleaved.begin(), old_interleaved.end());
    MIRA_CHECK(old_baseline != old_interleaved); // Reproduces the old normalize-then-sort bug.
    MIRA_CHECK(normalize_eval_events(baseline, true) == normalize_eval_events(interleaved, true));
    MIRA_CHECK(normalize_eval_events(baseline, false) != normalize_eval_events(interleaved, false));
    auto renamed = recorded(true);
    MIRA_CHECK(normalize_eval_events(baseline, false) == normalize_eval_events(renamed, false));
    return 0;
}

int semantic_drift_still_fails() {
    const auto baseline = normalize_eval_events(recorded(), true);
    auto changed = recorded(true);
    changed.back().second.replace(changed.back().second.find(std::string(32, 'e')), 32,
                                  std::string(32, 'f')); // Wrong step/task relationship.
    MIRA_CHECK(baseline != normalize_eval_events(changed, true));
    changed = recorded();
    changed.back().second.replace(changed.back().second.find("attempt\":2"), 10, "attempt\":3");
    MIRA_CHECK(baseline != normalize_eval_events(changed, true));
    changed = recorded();
    changed.pop_back();
    MIRA_CHECK(baseline != normalize_eval_events(changed, true));
    changed = recorded();
    changed.push_back(changed.back());
    MIRA_CHECK(baseline != normalize_eval_events(changed, true));
    return 0;
}

int mismatch_diagnostics_are_bounded_at_both_ends() {
    MIRA_CHECK(first_eval_event_difference({}, {}).empty());
    MIRA_CHECK(first_eval_event_difference({}, {"extra"}) ==
               "event 0 baseline [<end>] repeat [extra]");
    MIRA_CHECK(first_eval_event_difference({"extra"}, {}) ==
               "event 0 baseline [extra] repeat [<end>]");
    MIRA_CHECK(first_eval_event_difference({"same"}, {"same", "extra"}) ==
               "event 1 baseline [<end>] repeat [extra]");
    MIRA_CHECK(first_eval_event_difference({"a"}, {"b"}) == "event 0 baseline [a] repeat [b]");
    const auto long_message = first_eval_event_difference({std::string(1000, 'z')}, {});
    MIRA_CHECK(long_message.size() < 400);
    return 0;
}
} // namespace

int main() {
    for (const auto test :
         {canonical_identity_assignment_precedes_multiset_comparison, semantic_drift_still_fails,
          mismatch_diagnostics_are_bounded_at_both_ends}) {
        if (const auto code = test(); code != 0)
            return code;
    }
    return 0;
}
