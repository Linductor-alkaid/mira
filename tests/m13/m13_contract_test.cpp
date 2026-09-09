// M13-01/M13-02/M13-03 (contract half): the four-domain organization, the
// episode/lesson/signature contracts and the failure-retrieval query builder
// (DEC-029). Round trips are lossless, decoding fails closed, digests and
// derived ids are deterministic, and the pure conversions produce valid
// memory records.

#include "support/m13_support.hpp"

#include <mira/workflow_learning.hpp>

#include <chrono>

namespace {

using namespace mira;
using namespace mira::testing;

const WorkflowLearningLimits kLimits{};

int domain_mapping_is_total_and_matches_the_architecture_table() {
    // DEC-014 §10.2, line by line.
    MIRA_CHECK(memory_domain_of(MemoryKind::EnvironmentFact) ==
               MemoryDomain::EnvironmentModel);
    MIRA_CHECK(memory_domain_of(MemoryKind::ApplicationFact) ==
               MemoryDomain::EnvironmentModel);
    MIRA_CHECK(memory_domain_of(MemoryKind::Preference) == MemoryDomain::UserModel);
    MIRA_CHECK(memory_domain_of(MemoryKind::Procedure) == MemoryDomain::ProceduralMemory);
    MIRA_CHECK(memory_domain_of(MemoryKind::SkillHint) == MemoryDomain::ProceduralMemory);
    MIRA_CHECK(memory_domain_of(MemoryKind::RecoveryLesson) ==
               MemoryDomain::ProceduralMemory);
    MIRA_CHECK(memory_domain_of(MemoryKind::Episode) == MemoryDomain::EpisodicMemory);

    const auto environment = memory_kinds_of_domain(MemoryDomain::EnvironmentModel);
    MIRA_CHECK(environment.size() == 2);
    const auto user = memory_kinds_of_domain(MemoryDomain::UserModel);
    MIRA_CHECK(user.size() == 1 && user[0] == MemoryKind::Preference);
    const auto procedural = memory_kinds_of_domain(MemoryDomain::ProceduralMemory);
    MIRA_CHECK(procedural.size() == 3);
    const auto episodic = memory_kinds_of_domain(MemoryDomain::EpisodicMemory);
    MIRA_CHECK(episodic.size() == 1 && episodic[0] == MemoryKind::Episode);

    // Every kind maps back into its own domain's inverse set (total and
    // consistent in both directions).
    for (const auto kind : {MemoryKind::Preference, MemoryKind::EnvironmentFact,
                            MemoryKind::ApplicationFact, MemoryKind::Episode,
                            MemoryKind::Procedure, MemoryKind::SkillHint,
                            MemoryKind::RecoveryLesson}) {
        const auto kinds = memory_kinds_of_domain(memory_domain_of(kind));
        MIRA_CHECK(std::find(kinds.begin(), kinds.end(), kind) != kinds.end());
    }
    return 0;
}

int domain_names_round_trip_and_fail_closed() {
    for (const auto &[domain, name] :
         {std::pair{MemoryDomain::EnvironmentModel, "environment"},
          std::pair{MemoryDomain::UserModel, "user"},
          std::pair{MemoryDomain::ProceduralMemory, "procedural"},
          std::pair{MemoryDomain::EpisodicMemory, "episodic"}}) {
        MIRA_CHECK(memory_domain_name(domain) == name);
        auto parsed = parse_memory_domain(name);
        MIRA_CHECK(parsed.has_value() && parsed.value() == domain);
    }
    MIRA_CHECK(!parse_memory_domain("unknown").has_value());
    MIRA_CHECK(!parse_memory_domain("").has_value());
    return 0;
}

WorkflowEpisodeRecord sample_episode() {
    WorkflowEpisodeRecord episode;
    episode.run_id = WorkflowRunId::generate().to_string();
    episode.workflow_id = "m13-flow";
    episode.ir_digest = digest_string("m13").to_string();
    episode.policy = "recoverable";
    episode.outcome = "failed";
    episode.failed_step_id = "step-2";
    episode.failure_reason_code = "mira.workflow:6";
    episode.escalations = 1;
    episode.checkpoint_handoffs = 2;
    episode.recorded_at_ms = 1'700'000'000'123ULL;
    return episode;
}

WorkflowRecoveryLesson sample_lesson() {
    WorkflowRecoveryLesson lesson;
    lesson.lesson_id = WorkflowRunId::generate().to_string();
    lesson.workflow_id = "m13-flow";
    lesson.ir_digest = digest_string("m13").to_string();
    lesson.recovered_run_id = lesson.lesson_id;
    lesson.failure.workflow_id = "m13-flow";
    lesson.failure.step_id = "step-2";
    lesson.failure.step_kind = "tool_call";
    lesson.failure.reason_code = "mira.workflow:6";
    WorkflowRecoveryAction action;
    action.patch_id = WorkflowPatchId::generate().to_string();
    action.patch_digest = digest_string("patch").to_string();
    action.targets = {"run_parameters", "skip"};
    lesson.recovery = {std::move(action)};
    lesson.outcome = "recovered";
    lesson.recorded_at_ms = 1'700'000'000'456ULL;
    return lesson;
}

int episode_contract_round_trips_losslessly() {
    const auto episode = sample_episode();
    auto reparsed = workflow_episode_from_json(workflow_episode_to_json(episode));
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(reparsed.value() == episode);
    // A completed episode carries no failure fields.
    auto completed = sample_episode();
    completed.outcome = "completed";
    completed.failed_step_id.reset();
    completed.failure_reason_code.reset();
    MIRA_CHECK(workflow_episode_from_json(workflow_episode_to_json(completed)).value() ==
               completed);
    return 0;
}

int episode_decoding_fails_closed() {
    const auto json = workflow_episode_to_json(sample_episode());

    auto unknown = parse_json(canonical_json_string(json)).value();
    unknown.set("surprise", JsonValue{true});
    MIRA_CHECK(!workflow_episode_from_json(unknown).has_value());

    auto bad_version = parse_json(canonical_json_string(json)).value();
    bad_version.find("schema_version")->set("major", JsonValue{static_cast<std::int64_t>(2)});
    MIRA_CHECK(!workflow_episode_from_json(bad_version).has_value());

    auto bad_outcome = parse_json(canonical_json_string(json)).value();
    *bad_outcome.find("outcome") = JsonValue{"exploded"};
    MIRA_CHECK(!workflow_episode_from_json(bad_outcome).has_value());

    auto missing_reason = parse_json(canonical_json_string(json)).value();
    *missing_reason.find("failure_reason_code") = JsonValue{"has spaces!"};
    MIRA_CHECK(!workflow_episode_from_json(missing_reason).has_value());

    // failed outcome requires the failure signature fields.
    auto no_signature = parse_json(canonical_json_string(json)).value();
    *no_signature.find("failed_step_id") = JsonValue{};
    *no_signature.find("failure_reason_code") = JsonValue{};
    MIRA_CHECK(!workflow_episode_from_json(no_signature).has_value());

    auto bad_policy = parse_json(canonical_json_string(json)).value();
    *bad_policy.find("policy") = JsonValue{"turbo"};
    MIRA_CHECK(!workflow_episode_from_json(bad_policy).has_value());

    auto negative_counter = parse_json(canonical_json_string(json)).value();
    *negative_counter.find("escalations") = JsonValue{static_cast<std::int64_t>(-1)};
    MIRA_CHECK(!workflow_episode_from_json(negative_counter).has_value());

    auto bad_digest = parse_json(canonical_json_string(json)).value();
    *bad_digest.find("ir_digest") = JsonValue{"not-hex"};
    MIRA_CHECK(!workflow_episode_from_json(bad_digest).has_value());

    auto oversized = json;
    WorkflowLearningLimits tiny = kLimits;
    tiny.max_id_bytes = 2;
    MIRA_CHECK(!workflow_episode_from_json(oversized, tiny).has_value());
    return 0;
}

int episode_digest_is_deterministic() {
    const auto episode = sample_episode();
    MIRA_CHECK(workflow_episode_digest(episode) == workflow_episode_digest(episode));
    auto changed = episode;
    changed.escalations += 1;
    MIRA_CHECK(workflow_episode_digest(changed) != workflow_episode_digest(episode));
    return 0;
}

int lesson_contract_round_trips_and_fails_closed() {
    const auto lesson = sample_lesson();
    auto reparsed = recovery_lesson_from_json(recovery_lesson_to_json(lesson));
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(reparsed.value() == lesson);

    auto json = parse_json(canonical_json_string(recovery_lesson_to_json(lesson))).value();
    json.set("surprise", JsonValue{1});
    MIRA_CHECK(!recovery_lesson_from_json(json).has_value());

    auto bad_outcome = parse_json(canonical_json_string(recovery_lesson_to_json(lesson))).value();
    *bad_outcome.find("outcome") = JsonValue{"failed"};
    MIRA_CHECK(!recovery_lesson_from_json(bad_outcome).has_value());

    auto bad_target = parse_json(canonical_json_string(recovery_lesson_to_json(lesson))).value();
    auto *first_action = const_cast<JsonValue *>(&(*bad_target.find("recovery")->as_array())[0]);
    first_action->set("extra", JsonValue{});
    MIRA_CHECK(!recovery_lesson_from_json(bad_target).has_value());
    auto bad_target_name = parse_json(canonical_json_string(recovery_lesson_to_json(lesson)))
                               .value();
    auto *bad_target_entry =
        const_cast<JsonValue *>(&(*bad_target_name.find("recovery")->as_array())[0]);
    auto *bad_targets = const_cast<JsonValue *>(bad_target_entry->find("targets"));
    (*const_cast<JsonValue::Array *>(bad_targets->as_array()))[0] =
        JsonValue{"reformat_disk"};
    MIRA_CHECK(!recovery_lesson_from_json(bad_target_name).has_value());

    // Neither an empty recovery without the resume mark nor the resume mark
    // with actions: exactly one form per lesson.
    auto empty_recovery = parse_json(canonical_json_string(recovery_lesson_to_json(lesson)))
                              .value();
    *empty_recovery.find("recovery") = JsonValue{JsonValue::Array{}};
    MIRA_CHECK(!recovery_lesson_from_json(empty_recovery).has_value());
    auto both_forms = sample_lesson();
    both_forms.resumed_without_patch = true;
    MIRA_CHECK(recovery_lesson_from_json(recovery_lesson_to_json(both_forms)).has_value());

    MIRA_CHECK(recovery_lesson_digest(lesson) == recovery_lesson_digest(lesson));
    auto changed = lesson;
    changed.resumed_without_patch = true;
    MIRA_CHECK(recovery_lesson_digest(changed) != recovery_lesson_digest(lesson));
    return 0;
}

int failure_signature_sanitizes_and_round_trips() {
    WorkflowFailureSignature signature;
    signature.workflow_id = "m13-flow";
    signature.step_id = "step-2";
    signature.step_kind = "tool_call";
    signature.reason_code = "mira.workflow:6";
    auto reparsed = failure_signature_from_json(failure_signature_to_json(signature));
    MIRA_CHECK(reparsed.has_value() && reparsed.value() == signature);

    auto spacey = signature;
    spacey.reason_code = "oops it broke";
    MIRA_CHECK(!failure_signature_from_json(failure_signature_to_json(spacey)).has_value());

    auto missing = parse_json(canonical_json_string(failure_signature_to_json(signature)))
                       .value();
    missing.set("extra", JsonValue{});
    MIRA_CHECK(!failure_signature_from_json(missing).has_value());
    return 0;
}

int conversions_produce_valid_verified_memory_records() {
    const auto episode = sample_episode();
    const EventId evidence = EventId::generate();
    const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    const auto scope = learning_scope();
    auto record =
        episode_to_memory_record(episode, scope, {evidence}, now);
    MIRA_CHECK(record.validate().has_value());
    MIRA_CHECK(record.kind == MemoryKind::Episode);
    MIRA_CHECK(record.scope == scope);
    MIRA_CHECK(record.verification == MemoryVerification::Verified);
    MIRA_CHECK(record.confidence == 1.0F);
    MIRA_CHECK(record.provenance.size() == 1 && record.provenance[0] == evidence);
    MIRA_CHECK(record.validity.valid_from == now);
    MIRA_CHECK(record.status == MemoryStatus::Active);
    // Statement is the canonical episode JSON: it parses back losslessly.
    MIRA_CHECK(workflow_episode_from_json(parse_json(record.statement).value()).value() ==
               episode);

    MemoryMutation mutation;
    mutation.id = workflow_episode_mutation_id(episode.run_id);
    mutation.type = MemoryMutationType::Add;
    mutation.scope = scope;
    mutation.proposed = record;
    mutation.evidence = {evidence};
    mutation.reason = MutationReasonCode::VerifiedEvent;
    MIRA_CHECK(mutation.validate().has_value());

    const auto lesson = sample_lesson();
    auto lesson_record = recovery_lesson_to_memory_record(lesson, scope, {evidence}, now);
    MIRA_CHECK(lesson_record.validate().has_value());
    MIRA_CHECK(lesson_record.kind == MemoryKind::RecoveryLesson);
    MIRA_CHECK(lesson_record.verification == MemoryVerification::Verified);
    // Pure functions: same inputs, same outputs.
    MIRA_CHECK(episode_to_memory_record(episode, scope, {evidence}, now).statement ==
               record.statement);
    MIRA_CHECK(recovery_lesson_to_memory_record(lesson, scope, {evidence}, now).statement ==
               lesson_record.statement);
    return 0;
}

int lesson_record_parsing_accepts_only_canonical_statements() {
    const auto lesson = sample_lesson();
    const EventId evidence = EventId::generate();
    const auto scope = learning_scope();
    const auto record = recovery_lesson_to_memory_record(
        lesson, scope, {evidence}, std::chrono::system_clock::now());
    auto reparsed = recovery_lesson_from_record(record);
    MIRA_CHECK(reparsed.has_value());
    MIRA_CHECK(reparsed.value() == lesson);

    // Same JSON with a non-canonical text form (whitespace inside) fails
    // closed instead of being guessed at.
    auto padded = record;
    padded.statement = " " + record.statement + " ";
    MIRA_CHECK(!recovery_lesson_from_record(padded).has_value());
    auto foreign = record;
    foreign.statement = "{\"schema_version\":{\"major\":1,\"minor\":0}}";
    MIRA_CHECK(!recovery_lesson_from_record(foreign).has_value());
    return 0;
}

int derived_ids_are_deterministic_and_distinct() {
    const std::string run_a = WorkflowRunId::generate().to_string();
    const std::string run_b = WorkflowRunId::generate().to_string();
    MIRA_CHECK(workflow_episode_memory_id(run_a) == workflow_episode_memory_id(run_a));
    MIRA_CHECK(workflow_episode_memory_id(run_a) != workflow_episode_memory_id(run_b));
    MIRA_CHECK(recovery_lesson_memory_id(run_a) != workflow_episode_memory_id(run_a));
    MIRA_CHECK(workflow_episode_mutation_id(run_a) != recovery_lesson_mutation_id(run_a));
    MIRA_CHECK(!workflow_episode_memory_id(run_a).is_nil());
    return 0;
}

int failure_retrieval_query_is_deterministic_and_bounded() {
    WorkflowFailureSignature signature;
    signature.workflow_id = "m13-flow";
    signature.step_id = "step-2";
    signature.reason_code = "mira.workflow:6";
    const auto scope = learning_scope();

    auto query = failure_retrieval_query(signature, scope);
    MIRA_CHECK(query.has_value());
    // Determinism without operator==: every governing field must match.
    const auto again = failure_retrieval_query(signature, scope);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(query.value().text == again.value().text);
    MIRA_CHECK(query.value().exact_terms == again.value().exact_terms);
    MIRA_CHECK(query.value().scopes == again.value().scopes);
    MIRA_CHECK(query.value().kinds == again.value().kinds);
    MIRA_CHECK(query.value().max_results == again.value().max_results);
    MIRA_CHECK(query.value().token_budget == again.value().token_budget);
    MIRA_CHECK(query.value().deadline == again.value().deadline);
    MIRA_CHECK(query.value().scopes.size() == 1 && query.value().scopes[0] == scope);
    MIRA_CHECK(query.value().kinds.has_value() && query.value().kinds->size() == 2);
    MIRA_CHECK(std::find(query.value().kinds->begin(), query.value().kinds->end(),
                         MemoryKind::Episode) != query.value().kinds->end());
    MIRA_CHECK(std::find(query.value().kinds->begin(), query.value().kinds->end(),
                         MemoryKind::RecoveryLesson) != query.value().kinds->end());
    MIRA_CHECK(query.value().exact_terms.size() == 2);
    MIRA_CHECK(query.value().query_embedding.empty());
    MIRA_CHECK(query.value().max_results == kLimits.max_retrieval_results);
    MIRA_CHECK(query.value().token_budget == kLimits.retrieval_token_budget);
    MIRA_CHECK(query.value().deadline == kLimits.retrieval_deadline);
    MIRA_CHECK(query.value().validate().has_value());

    // Without a step id the term narrows to the workflow only.
    auto stepless = signature;
    stepless.step_id.reset();
    auto narrowed = failure_retrieval_query(stepless, scope);
    MIRA_CHECK(narrowed.has_value() && narrowed.value().exact_terms.size() == 1);

    auto no_workflow = signature;
    no_workflow.workflow_id.clear();
    MIRA_CHECK(!failure_retrieval_query(no_workflow, scope).has_value());
    auto unsanitized = signature;
    unsanitized.reason_code = "not sanitized";
    MIRA_CHECK(!failure_retrieval_query(unsanitized, scope).has_value());

    WorkflowLearningLimits broken = kLimits;
    broken.max_retrieval_results = 0;
    MIRA_CHECK(!failure_retrieval_query(signature, scope, broken).has_value());
    broken = kLimits;
    broken.retrieval_deadline = std::chrono::milliseconds{-1};
    MIRA_CHECK(!failure_retrieval_query(signature, scope, broken).has_value());
    MIRA_CHECK(!broken.validate().has_value());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {domain_mapping_is_total_and_matches_the_architecture_table,
         domain_names_round_trip_and_fail_closed, episode_contract_round_trips_losslessly,
         episode_decoding_fails_closed, episode_digest_is_deterministic,
         lesson_contract_round_trips_and_fails_closed, failure_signature_sanitizes_and_round_trips,
         conversions_produce_valid_verified_memory_records,
         lesson_record_parsing_accepts_only_canonical_statements,
         derived_ids_are_deterministic_and_distinct,
         failure_retrieval_query_is_deterministic_and_bounded});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    std::cout << "m13 contract tests passed\n";
    return 0;
}
