#include "m30_preference_support.hpp"
#include "m30_support.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int coordinator_resolves_explicit_preference_intents() {
    struct TemporaryArtifacts final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("mira-preference-intent-" + Id128::generate().to_string());
        ~TemporaryArtifacts() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } artifacts;
    MemoryAgentConfig config;
    config.artifacts = std::make_shared<FileArtifactStore>(artifacts.root);
    InteractionMemoryFixture host(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.mode == MemoryAgentMode::Compress)
                proposal.summary = {{"Prior answer preference", {0}}};
            return proposal;
        },
        config);
    CapabilityGrant grant;
    grant.capability = "mira.memory.manage";
    grant.version = host.principal.identity_version;
    const auto scope = memory_user_scope(host.principal);
    grant.resources.type = "memory";
    grant.resources.scope = scope.subject_id;
    host.principal.grants.push_back(grant);
    const auto input = host.coordinator->submit_input(host.input("preference-intent"));
    MIRA_CHECK(input && input.value().task_id && input.value().session_id);
    const auto task = *input.value().task_id;
    MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
    MIRA_CHECK(host.finish(task));
    const auto spec = host.spec(input.value());
    const auto submit = [&](const PrincipalContext &principal,
                            const PreferenceIntentRequest &request) {
        return host.storage.executor
            .submit_auto([&] {
                return host.coordinator->submit_preference_intent(principal, request,
                                                                  memory_context(spec));
            })
            .get();
    };
    const auto approve = [&](const PreferenceProposal &proposal) {
        return host.storage.executor
            .submit_auto([&] {
                return host.service->decide_preference(host.principal, proposal.id, proposal.digest,
                                                       ConfirmationDecision::Approve,
                                                       memory_context(spec));
            })
            .get();
    };
    const auto resolve = [&](const PrincipalContext &principal, const MemoryTargetSelector &target,
                             OperationContext operation) {
        return host.storage.executor
            .submit_auto([&] {
                return host.coordinator->resolve_memory_target(principal, target, operation);
            })
            .get();
    };
    const MemoryTargetSelector answer_style{MemoryScopeKind::User, MemoryKind::Preference,
                                            " ANSWER.Style "};
    const auto missing = resolve(host.principal, answer_style, memory_context(spec));
    MIRA_CHECK(missing && !missing.value());
    auto stopped = memory_context(spec);
    stopped.cancellation_requested = [] { return true; };
    const auto cancelled = resolve(host.principal, answer_style, stopped);
    MIRA_CHECK(!cancelled && cancelled.error().code == ErrorCode::Cancelled);
    stopped.cancellation_requested = {};
    stopped.deadline = std::chrono::steady_clock::now();
    const auto timed_out = resolve(host.principal, answer_style, stopped);
    MIRA_CHECK(!timed_out && timed_out.error().code == ErrorCode::DeadlineExceeded);
    MemoryQuery query;
    query.scopes = {scope};
    PreferenceIntentRequest remember{PreferenceIntent::Remember,
                                     {MutationId::generate(), "answer.style=brief", {}, {}}};
    auto unauthorized = host.principal;
    unauthorized.grants.pop_back();
    const auto denied = submit(unauthorized, remember);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
    const auto lookup_denied = resolve(unauthorized, answer_style, memory_context(spec));
    MIRA_CHECK(!lookup_denied && lookup_denied.error().code == ErrorCode::PermissionDenied);
    unauthorized = host.principal;
    unauthorized.user_id = UserId::generate();
    const auto foreign = submit(unauthorized, remember);
    MIRA_CHECK(!foreign && foreign.error().code == ErrorCode::PermissionDenied);
    unauthorized = host.principal;
    unauthorized.grants.back().expires_at = std::chrono::system_clock::now();
    const auto expired = submit(unauthorized, remember);
    MIRA_CHECK(!expired && expired.error().code == ErrorCode::PermissionDenied);
    unauthorized = host.principal;
    unauthorized.grants.back().resources.scope = "foreign-user";
    const auto wrong_scope = submit(unauthorized, remember);
    MIRA_CHECK(!wrong_scope && wrong_scope.error().code == ErrorCode::PermissionDenied);

    const auto proposed = submit(host.principal, remember);
    MIRA_CHECK(proposed && proposed.value().scope == scope &&
               proposed.value().state == PreferenceProposalState::Pending &&
               proposed.value().mutation->type == MemoryMutationType::Add);
    MIRA_CHECK(host.storage.memory->query(query).value().records.empty());
    const auto decided = approve(proposed.value());
    MIRA_CHECK(decided && decided.value().applied);
    const auto original = host.storage.memory->get(decided.value().applied->record, scope);
    MIRA_CHECK(original && original.value());
    const auto original_target = resolve(host.principal, answer_style, memory_context(spec));
    MIRA_CHECK(original_target && original_target.value() &&
               original_target.value()->record == original.value()->id &&
               original_target.value()->version == original.value()->version);
    remember.proposal.id = MutationId::generate();
    const auto repeated = submit(host.principal, remember);
    MIRA_CHECK(repeated && repeated.value().state == PreferenceProposalState::NoChange);
    const auto after_repeat = host.storage.memory->get(original.value()->id, scope);
    MIRA_CHECK(after_repeat && after_repeat.value() &&
               after_repeat.value()->version == original.value()->version &&
               after_repeat.value()->provenance == original.value()->provenance);

    remember.proposal.id = MutationId::generate();
    remember.proposal.statement = "answer.style=verbose";
    const auto conflict = submit(host.principal, remember);
    MIRA_CHECK(!conflict &&
               conflict.error().domain_code == static_cast<int>(MemoryDomainCode::VersionConflict));
    auto correct = remember;
    correct.intent = PreferenceIntent::Correct;
    const auto incomplete = submit(host.principal, correct);
    MIRA_CHECK(!incomplete && incomplete.error().code == ErrorCode::InvalidArgument);
    correct.proposal.target = original_target.value()->record;
    correct.proposal.expected_version = original_target.value()->version + 1;
    const auto stale = submit(host.principal, correct);
    MIRA_CHECK(!stale &&
               stale.error().domain_code == static_cast<int>(MemoryDomainCode::VersionConflict));
    correct.proposal.expected_version = original_target.value()->version;
    const auto correction = submit(host.principal, correct);
    MIRA_CHECK(correction && correction.value().mutation &&
               correction.value().mutation->type == MemoryMutationType::Supersede &&
               correction.value().mutation->reason == MutationReasonCode::HumanCorrection);
    MIRA_CHECK(host.storage.memory->query(query).value().records.front().id ==
               original.value()->id);
    const auto applied = approve(correction.value());
    MIRA_CHECK(applied && applied.value().applied);
    const auto corrected_target = resolve(host.principal, answer_style, memory_context(spec));
    MIRA_CHECK(corrected_target && corrected_target.value() &&
               corrected_target.value()->record == applied.value().applied->record);
    const auto replayed = submit(host.principal, correct);
    MIRA_CHECK(replayed && replayed.value().applied &&
               replayed.value().applied->record == applied.value().applied->record);
    const auto current = host.storage.memory->query(query);
    MIRA_CHECK(current && current.value().records.size() == 1 &&
               current.value().records.front().statement == "answer.style=verbose" &&
               current.value().records.front().supersedes == original.value()->id);

    auto second = current.value().records.front();
    second.id = MemoryId::generate();
    second.statement = "answer.style=balanced";
    second.validity.valid_from -= 1h;
    second.supersedes.reset();
    second.provenance = {EventId::generate()};
    const auto added_alternative =
        host.storage.executor
            .submit_auto([&] {
                return MemoryConsolidator{}.consolidate_candidates(
                    *host.storage.memory, {{second, second.provenance}}, scope, Timestamp::now());
            })
            .get();
    MIRA_CHECK(added_alternative &&
               added_alternative.value().entries.front().reason_code == "alternative-preserved");
    correct.proposal.id = MutationId::generate();
    correct.proposal.target = current.value().records.front().id;
    correct.proposal.expected_version = current.value().records.front().version;
    const auto ambiguous = submit(host.principal, correct);
    MIRA_CHECK(!ambiguous && ambiguous.error().domain_code ==
                                 static_cast<int>(MemoryDomainCode::VersionConflict));
    const auto ambiguous_target = resolve(host.principal, answer_style, memory_context(spec));
    MIRA_CHECK(!ambiguous_target && ambiguous_target.error().domain_code ==
                                        static_cast<int>(MemoryDomainCode::VersionConflict));

    PreferenceIntentRequest independent{PreferenceIntent::Remember,
                                        {MutationId::generate(), "timezone=UTC", {}, {}}};
    const auto unrelated = submit(host.principal, independent);
    MIRA_CHECK(unrelated && unrelated.value().state == PreferenceProposalState::Pending);
    MemoryAgentRequest compress;
    compress.mode = MemoryAgentMode::Compress;
    compress.task = task;
    compress.history = {history_text("Use brief replies"), history_text("Thanks")};
    auto compressed = host.service->submit(compress, memory_context(spec));
    MIRA_CHECK(compressed);
    const auto &projection = compressed.value().result.get();
    MIRA_CHECK(projection && projection.value().projection_artifact);
    const auto &artifact = *projection.value().projection_artifact;
    const auto snapshot =
        host.service->context_supplier()(memory_request(spec), memory_context(spec));
    MIRA_CHECK(snapshot && snapshot.value().current() && config.artifacts->open(artifact));

    const auto old = host.storage.memory->get(original.value()->id, scope);
    MIRA_CHECK(old && old.value() && old.value()->status == MemoryStatus::Superseded);
    const MemoryForgetRequest forget{OperationId::generate(), MemoryScopeKind::User,
                                     old.value()->id, old.value()->version};
    const auto run_forget = [&](const PrincipalContext &principal,
                                const MemoryForgetRequest &request) {
        return host.storage.executor
            .submit_auto([&] {
                return host.coordinator->submit_forget_intent(principal, request,
                                                              memory_context(spec));
            })
            .get();
    };
    const auto no_permission = run_forget(unauthorized, forget);
    MIRA_CHECK(!no_permission && no_permission.error().code == ErrorCode::PermissionDenied);
    auto wrong_version = forget;
    ++wrong_version.expected_version;
    MIRA_CHECK(!run_forget(host.principal, wrong_version) && snapshot.value().current());
    const auto forgotten = run_forget(host.principal, forget);
    MIRA_CHECK(forgotten && forgotten.value().erasure.status == ErasureStatus::Complete &&
               forgotten.value().erasure.counts.records_removed == 1 &&
               !snapshot.value().current());
    MIRA_CHECK(!config.artifacts->open(artifact));
    MIRA_CHECK(!host.storage.memory->get(forget.record, scope).value());
    MIRA_CHECK(host.storage.memory->get(applied.value().applied->record, scope).value());
    const auto preview = [&](MutationId id) {
        return host.storage.executor
            .submit_auto([&] {
                return host.service->preference_proposal(host.principal, id, memory_context(spec));
            })
            .get();
    };
    const auto erased_preference = preview(correction.value().id);
    MIRA_CHECK(erased_preference &&
               erased_preference.value().state == PreferenceProposalState::Erased &&
               !erased_preference.value().previous && erased_preference.value().statement.empty());
    MIRA_CHECK(preview(unrelated.value().id).value().statement == "timezone=UTC");
    MIRA_CHECK(approve(unrelated.value()));
    const auto repeated_forget = run_forget(host.principal, forget);
    MIRA_CHECK(repeated_forget && repeated_forget.value().idempotent_replay);

    const auto session_binding = host.binding(input.value());
    const auto &session_scope = session_binding.scopes.front();
    MIRA_CHECK(host.storage.seed(session_binding, "network=local"));
    MemoryQuery session_query;
    session_query.scopes = {session_scope};
    session_query.claim_key = "network";
    const auto session_records = host.storage.memory->query(session_query);
    MIRA_CHECK(session_records && session_records.value().records.size() == 1);
    const auto &session_record = session_records.value().records.front();
    const MemoryTargetSelector network{MemoryScopeKind::Session, MemoryKind::EnvironmentFact,
                                       "Network"};
    const auto wrong_grant = resolve(host.principal, network, memory_context(spec));
    MIRA_CHECK(!wrong_grant && wrong_grant.error().code == ErrorCode::PermissionDenied);
    auto session_principal = host.principal;
    session_principal.grants.back().resources.scope = session_scope.subject_id;
    const auto found_network = resolve(session_principal, network, memory_context(spec));
    MIRA_CHECK(found_network && found_network.value() &&
               found_network.value()->record == session_record.id);
    auto wrong_kind = network;
    wrong_kind.kind = MemoryKind::ApplicationFact;
    const auto other_kind = resolve(session_principal, wrong_kind, memory_context(spec));
    MIRA_CHECK(other_kind && !other_kind.value());
    auto duplicate = session_record;
    duplicate.id = MemoryId::generate();
    const auto added_duplicate = host.storage.executor
                                     .submit_auto([&] {
                                         MemoryMutation add;
                                         add.id = MutationId::generate();
                                         add.type = MemoryMutationType::Add;
                                         add.scope = session_scope;
                                         add.proposed = duplicate;
                                         add.evidence = duplicate.provenance;
                                         return host.storage.memory->apply(add);
                                     })
                                     .get();
    MIRA_CHECK(added_duplicate);
    MIRA_CHECK(host.storage.memory->query(session_query).value().records.size() == 1);
    const auto ambiguous_network = resolve(session_principal, network, memory_context(spec));
    MIRA_CHECK(!ambiguous_network && ambiguous_network.error().domain_code ==
                                         static_cast<int>(MemoryDomainCode::VersionConflict));

    const MemoryForgetRequest forget_session{OperationId::generate(), MemoryScopeKind::Session,
                                             found_network.value()->record,
                                             found_network.value()->version};
    const auto forget_wrong_grant = run_forget(host.principal, forget_session);
    MIRA_CHECK(!forget_wrong_grant &&
               forget_wrong_grant.error().code == ErrorCode::PermissionDenied);
    const auto erased_session = run_forget(session_principal, forget_session);
    MIRA_CHECK(erased_session && erased_session.value().identity.scope == session_scope &&
               erased_session.value().erasure.status == ErasureStatus::Complete);
    const auto surviving = resolve(session_principal, network, memory_context(spec));
    MIRA_CHECK(surviving && surviving.value() && surviving.value()->record == duplicate.id);
    return 0;
}

int reopens_the_typed_proposal_without_losing_idempotency() {
    PreferenceFixture host;
    const PreferenceIntentRequest request{PreferenceIntent::Remember,
                                          {MutationId::generate(), "timezone=UTC", {}, {}}};
    const auto submit = [&] {
        return host.run([&] {
            return host.service->propose_preference_intent(host.storage.principal, request,
                                                           host.context());
        });
    };
    const auto first = submit();
    MIRA_CHECK(first && first.value().state == PreferenceProposalState::Pending);
    MIRA_CHECK(host.reopen());
    const auto replay = submit();
    MIRA_CHECK(replay && replay.value().digest == first.value().digest);
    const auto untyped = host.propose(request.proposal.statement, request.proposal.id);
    MIRA_CHECK(!untyped && untyped.error().code == ErrorCode::PermissionDenied);
    return 0;
}

int continues_forget_after_lost_control_acknowledgements() {
    PreferenceFixture host;
    const auto proposal = host.propose("theme=dark");
    MIRA_CHECK(proposal);
    const auto applied = host.decide(proposal.value());
    MIRA_CHECK(applied && applied.value().applied);
    const auto &mutation = *applied.value().applied;
    const MemoryForgetRequest request{OperationId::generate(), MemoryScopeKind::User,
                                      mutation.record, mutation.new_version};
    const auto forget = [&] {
        return host.run([&] {
            return host.service->forget_record(host.storage.principal, request, host.context());
        });
    };
    const auto recall = [&] {
        MemoryAgentRequest req;
        req.task = host.binding.work.task_id;
        req.mode = MemoryAgentMode::Recall;
        return host.service->submit(req, host.context());
    };
    const auto correction_id = MutationId::generate();
    host.fault->fail_type = "mira.memory-agent.preference-proposed.v1";
    host.fault->after_write = true;
    const auto lost_proposal = host.propose("theme=light", correction_id);
    MIRA_CHECK(!lost_proposal && lost_proposal.error().code == ErrorCode::DataLoss);
    const auto blocked_forget = forget();
    MIRA_CHECK(!blocked_forget && blocked_forget.error().code == ErrorCode::DataLoss &&
               host.query().value().records.size() == 1);
    host.storage.runtime = RuntimeId::generate();
    MIRA_CHECK(host.reopen() && host.preview(correction_id).value().previous);

    host.fault->fail_type = "mira.memory-agent.record-forget-prepared.v1";
    host.fault->after_write = true;
    const auto lost_prepare = forget();
    MIRA_CHECK(!lost_prepare && lost_prepare.error().code == ErrorCode::DataLoss && !recall());
    const auto hidden_preview = host.preview(correction_id);
    MIRA_CHECK(!hidden_preview && hidden_preview.error().domain_code ==
                                      static_cast<int>(MemoryDomainCode::ErasurePending));
    host.storage.runtime = RuntimeId::generate();
    MIRA_CHECK(host.query().value().records.size() == 1 && host.reopen());
    const auto held = recall();
    MIRA_CHECK(!held &&
               held.error().domain_code == static_cast<int>(MemoryDomainCode::ErasurePending));
    const auto erased_preview = host.preview(proposal.value().id);
    MIRA_CHECK(erased_preview && erased_preview.value().state == PreferenceProposalState::Erased);
    MIRA_CHECK(host.preview(correction_id).value().state == PreferenceProposalState::Erased);

    host.fault->fail_type = "mira.memory-agent.record-forget-completed.v1";
    const auto lost_completion = forget();
    MIRA_CHECK(!lost_completion && lost_completion.error().code == ErrorCode::DataLoss &&
               host.query().value().records.empty() && !recall());
    host.storage.runtime = RuntimeId::generate();
    MIRA_CHECK(host.reopen() && !recall());
    const auto completed = forget();
    MIRA_CHECK(completed && completed.value().idempotent_replay &&
               completed.value().erasure.counts.records_removed == 1);
    MIRA_CHECK(host.reopen());
    const auto allowed = recall();
    MIRA_CHECK(allowed && allowed.value().result.get());
    const auto replay = forget();
    MIRA_CHECK(replay && replay.value().idempotent_replay);
    return 0;
}
} // namespace

int main() {
    if (const auto result = coordinator_resolves_explicit_preference_intents(); result != 0)
        return result;
    if (const auto result = reopens_the_typed_proposal_without_losing_idempotency(); result != 0)
        return result;
    return continues_forget_after_lost_control_acknowledgements();
}
