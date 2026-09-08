#include "../support/test.hpp"

#include <mira/workflow_versioning.hpp>

#include <string>

namespace {

using namespace mira;

[[nodiscard]] WorkflowVersionRecord make_record(SemanticVersion version,
                                                Sha256Digest content_digest,
                                                Sha256Digest parent_digest,
                                                WorkflowValidationResult validation,
                                                bool with_evidence = true) {
    WorkflowVersionRecord record;
    record.version = version;
    record.actor = "mira-maintainers";
    record.reason = "app update invalidated the selector";
    record.content_digest = content_digest;
    record.parent_digest = parent_digest;
    record.validation = validation;
    if (with_evidence) {
        record.validation_evidence = digest_string("evidence");
    }
    record.created_at = Timestamp::now();
    return record;
}

int history_is_append_only_and_chained() {
    WorkflowVersionHistory history;
    history.workflow_id = WorkflowId::generate();

    const Sha256Digest v1 = digest_string("v1");
    const Sha256Digest v2 = digest_string("v2");

    // The first record must chain from the nil digest.
    MIRA_CHECK(append_workflow_version(history,
                                       make_record({2, 0, 0}, v1, digest_string("nope"),
                                                   WorkflowValidationResult::DryRunPassed))
                   .has_value() == false);
    MIRA_CHECK(
        append_workflow_version(
            history, make_record({1, 0, 0}, v1, Sha256Digest{},
                                 WorkflowValidationResult::DryRunPassed))
            .has_value());
    MIRA_CHECK(history.records.size() == 1);

    // Versions must increase and chain to the head digest.
    MIRA_CHECK(!append_workflow_version(
                   history, make_record({1, 0, 0}, v2, v1, WorkflowValidationResult::Validated))
                   .has_value());
    MIRA_CHECK(!append_workflow_version(
                   history, make_record({2, 0, 0}, v2, digest_string("wrong-parent"),
                                        WorkflowValidationResult::Validated))
                   .has_value());
    MIRA_CHECK(append_workflow_version(
                   history, make_record({2, 1, 0}, v2, v1, WorkflowValidationResult::Validated))
                   .has_value());
    MIRA_CHECK(history.records.size() == 2);

    // A nil content digest is never appendable.
    MIRA_CHECK(!append_workflow_version(
                   history,
                   make_record({3, 0, 0}, Sha256Digest{}, v2,
                               WorkflowValidationResult::NotValidated, false))
                   .has_value());

    // Validation evidence shape is enforced.
    MIRA_CHECK(!append_workflow_version(
                   history,
                   make_record({3, 0, 0}, digest_string("v3"), v2,
                               WorkflowValidationResult::Validated, false))
                   .has_value());
    MIRA_CHECK(!append_workflow_version(
                   history,
                   make_record({3, 0, 0}, digest_string("v3"), v2,
                               WorkflowValidationResult::NotValidated, true))
                   .has_value());

    // Oversize actor/reason strings are rejected.
    WorkflowVersionRecord bulky =
        make_record({3, 0, 0}, digest_string("v3"), v2, WorkflowValidationResult::NotValidated);
    bulky.actor = std::string(1024, 'x');
    MIRA_CHECK(!append_workflow_version(history, bulky).has_value());
    MIRA_CHECK(history.records.size() == 2);

    // A nil workflow id is not a history.
    WorkflowVersionHistory orphan;
    MIRA_CHECK(!append_workflow_version(
                   orphan,
                   make_record({1, 0, 0}, digest_string("o"), Sha256Digest{},
                               WorkflowValidationResult::NotValidated))
                   .has_value());
    return 0;
}

int runs_pin_their_creation_time_digest() {
    WorkflowVersionHistory history;
    history.workflow_id = WorkflowId::generate();
    const Sha256Digest v1 = digest_string("definition-v1");
    const Sha256Digest v2 = digest_string("definition-v2");
    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({1, 0, 0}, v1, Sha256Digest{},
                               WorkflowValidationResult::DryRunPassed))
                   .has_value());
    MIRA_CHECK(append_workflow_version(
                   history, make_record({2, 0, 0}, v2, v1, WorkflowValidationResult::Validated))
                   .has_value());

    // Replay resolves by digest, not by "latest": an old run keeps v1 even
    // after v2 exists (W-03).
    auto resolved = resolve_workflow_version(history, v1);
    MIRA_CHECK(resolved.has_value());
    MIRA_CHECK(resolved.value().content_digest == v1);
    MIRA_CHECK(resolved.value().version.major == 1 && resolved.value().version.minor == 0);

    // Unknown digests fail closed.
    MIRA_CHECK(!resolve_workflow_version(history, digest_string("missing")).has_value());
    MIRA_CHECK(resolve_workflow_version(history, digest_string("missing")).error().code ==
               ErrorCode::NotFound);
    return 0;
}

int only_validated_versions_are_runnable() {
    WorkflowVersionHistory history;
    history.workflow_id = WorkflowId::generate();
    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({1, 0, 0}, digest_string("draft"), Sha256Digest{},
                               WorkflowValidationResult::NotValidated, false))
                   .has_value());
    MIRA_CHECK(!latest_runnable_workflow_version(history).has_value());

    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({2, 0, 0}, digest_string("rejected"), digest_string("draft"),
                               WorkflowValidationResult::Rejected))
                   .has_value());
    MIRA_CHECK(!latest_runnable_workflow_version(history).has_value());

    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({3, 0, 0}, digest_string("dry-run-passed"),
                               digest_string("rejected"),
                               WorkflowValidationResult::DryRunPassed))
                   .has_value());
    auto runnable = latest_runnable_workflow_version(history);
    MIRA_CHECK(runnable.has_value());
    MIRA_CHECK(runnable.value().content_digest == digest_string("dry-run-passed"));

    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({4, 0, 0}, digest_string("fully-validated"),
                               digest_string("dry-run-passed"),
                               WorkflowValidationResult::Validated))
                   .has_value());
    auto latest = latest_runnable_workflow_version(history);
    MIRA_CHECK(latest.has_value());
    MIRA_CHECK(latest.value().content_digest == digest_string("fully-validated"));

    // A trailing unvalidated head does not hide the previous runnable one.
    MIRA_CHECK(append_workflow_version(
                   history,
                   make_record({5, 0, 0}, digest_string("wip"), digest_string("fully-validated"),
                               WorkflowValidationResult::NotValidated, false))
                   .has_value());
    auto still = latest_runnable_workflow_version(history);
    MIRA_CHECK(still.has_value());
    MIRA_CHECK(still.value().content_digest == digest_string("fully-validated"));

    // Validation result names round trip.
    for (const auto result : {WorkflowValidationResult::NotValidated,
                              WorkflowValidationResult::DryRunPassed,
                              WorkflowValidationResult::Validated,
                              WorkflowValidationResult::Rejected}) {
        auto parsed = parse_workflow_validation_result(workflow_validation_result_name(result));
        MIRA_CHECK(parsed.has_value() && parsed.value() == result);
    }
    MIRA_CHECK(!parse_workflow_validation_result("maybe").has_value());
    return 0;
}

} // namespace

int main() {
    if (history_is_append_only_and_chained() != 0) {
        return 1;
    }
    if (runs_pin_their_creation_time_digest() != 0) {
        return 1;
    }
    if (only_validated_versions_are_runnable() != 0) {
        return 1;
    }
    return 0;
}
