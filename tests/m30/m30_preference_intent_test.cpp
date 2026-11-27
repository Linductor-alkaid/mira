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
    InteractionMemoryFixture host({}, config);
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
    MemoryQuery query;
    query.scopes = {scope};
    PreferenceIntentRequest remember{PreferenceIntent::Remember,
                                     {MutationId::generate(), "answer.style=brief", {}, {}}};
    auto unauthorized = host.principal;
    unauthorized.grants.pop_back();
    const auto denied = submit(unauthorized, remember);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::PermissionDenied);
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
    correct.proposal.target = original.value()->id;
    correct.proposal.expected_version = original.value()->version + 1;
    const auto stale = submit(host.principal, correct);
    MIRA_CHECK(!stale &&
               stale.error().domain_code == static_cast<int>(MemoryDomainCode::VersionConflict));
    correct.proposal.expected_version = original.value()->version;
    const auto correction = submit(host.principal, correct);
    MIRA_CHECK(correction && correction.value().mutation &&
               correction.value().mutation->type == MemoryMutationType::Supersede &&
               correction.value().mutation->reason == MutationReasonCode::HumanCorrection);
    MIRA_CHECK(host.storage.memory->query(query).value().records.front().id ==
               original.value()->id);
    const auto applied = approve(correction.value());
    MIRA_CHECK(applied && applied.value().applied);
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
} // namespace

int main() {
    if (const auto result = coordinator_resolves_explicit_preference_intents(); result != 0)
        return result;
    return reopens_the_typed_proposal_without_losing_idempotency();
}
