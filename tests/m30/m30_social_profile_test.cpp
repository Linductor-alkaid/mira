#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/social_profile.hpp>
#include <mira/sqlite_social_directory.hpp>

#include <algorithm>

namespace {
using namespace mira;
using namespace mira::testing;
using Source = SocialAttributeSource;

Result<ConsolidationReport> write(MemoryAgentFixture &fixture, const MemoryRecord &record,
                                  bool model_assisted = false) {
    return fixture.executor
        .submit_auto([&] {
            MemoryCandidate candidate;
            candidate.proposed = record;
            candidate.evidence = record.provenance;
            candidate.model_assisted = model_assisted;
            return MemoryConsolidator{}.consolidate_candidates(*fixture.memory, {candidate},
                                                               record.scope, Timestamp::now());
        })
        .get();
}

bool includes(const MemoryQueryResult &result, const MemoryRecord &record) {
    return std::ranges::any_of(result.records, [&](const auto &found) {
        return memory_record_to_json(found) == memory_record_to_json(record);
    });
}

int source_independence_and_temporal_recall() {
    MemoryAgentFixture fixture;
    const SocialObjectKey object{{fixture.principal.tenant_id, Id128::generate()},
                                 Id128::generate()};
    const auto now = std::chrono::system_clock::now();
    const ValidityInterval current{now - 4h, now + 4h};
    const auto define = [&](std::string field, Source source, std::string value,
                            ValidityInterval interval = {}) {
        if (interval.valid_from == std::chrono::system_clock::time_point{})
            interval = current;
        return make_social_attribute(object, {std::move(field), source}, std::move(value),
                                     EventId::generate(), interval, now - 2h);
    };
    auto self = define("role", Source::SelfReported, "Development");
    auto confirmed = define("role", Source::UserConfirmed, "Development");
    auto inferred = define("role", Source::Inferred, "Operations");
    auto extra = define("role", Source::SelfReported, "Developer");
    auto historical = define("role", Source::SelfReported, "Intern", {now - 8h, now - 4h});
    auto scheduled = define("role", Source::UserConfirmed, "Coordinator", {now + 4h, now + 8h});
    auto another_field = define("role_history", Source::SelfReported, "Volunteer");
    MIRA_CHECK(self && confirmed && inferred && extra && historical && scheduled && another_field);
    const auto &record = self.value();
    MIRA_CHECK(record.verification == MemoryVerification::Observed &&
               confirmed.value().verification == MemoryVerification::HumanConfirmed &&
               inferred.value().verification == MemoryVerification::Unverified);
    for (const auto *assertion :
         {&self, &confirmed, &inferred, &extra, &historical, &scheduled, &another_field}) {
        auto written = write(fixture, assertion->value());
        MIRA_CHECK(written && written.value().count_of(CandidateDisposition::Applied) == 1);
        MIRA_CHECK(written.value().entries.front().key.find(assertion->value().statement) ==
                   std::string::npos);
    }
    auto replay = write(fixture, record);
    MIRA_CHECK(replay && replay.value().entries.front().idempotent_replay);
    auto conflicting = record;
    conflicting.statement = "DEVELOPMENT";
    auto collision = write(fixture, conflicting);
    MIRA_CHECK(collision &&
               collision.value().count_of(CandidateDisposition::RejectedConflict) == 1);
    conflicting = record;
    conflicting.validity.valid_until = now + 5h;
    collision = write(fixture, conflicting);
    MIRA_CHECK(collision &&
               collision.value().count_of(CandidateDisposition::RejectedConflict) == 1);

    MemoryQuery query;
    query.scopes = {record.scope};
    query.social_attribute_field = "role";
    const auto matching = fixture.memory->query(query);
    MIRA_CHECK(matching && matching.value().records.size() == 4);
    MIRA_CHECK(
        includes(matching.value(), record) && includes(matching.value(), confirmed.value()) &&
        includes(matching.value(), inferred.value()) && includes(matching.value(), extra.value()));
    query.max_sensitivity = Sensitivity::Internal;
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    query.max_sensitivity.reset();
    query.as_of_valid = now - 5h;
    auto previous = fixture.memory->query(query);
    MIRA_CHECK(previous && previous.value().records.size() == 1 &&
               includes(previous.value(), historical.value()));
    query.as_of_recorded = now - 3h;
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    query.as_of_recorded.reset();
    query.as_of_valid = now + 4h;
    auto next = fixture.memory->query(query);
    MIRA_CHECK(next && next.value().records.size() == 1 &&
               includes(next.value(), scheduled.value()));
    query.as_of_valid = now - 4h;
    MIRA_CHECK(fixture.memory->query(query).value().records.size() == 4);
    query.as_of_valid.reset();
    query.as_of_recorded = now - 1h;
    MIRA_CHECK(fixture.memory->query(query).value().records.size() == 4);

    MemoryMutation retirement;
    retirement.id = MutationId::generate();
    retirement.type = MemoryMutationType::Tombstone;
    retirement.scope = record.scope;
    retirement.target = record.id;
    retirement.expected_version = 1;
    retirement.proposed = record;
    retirement.reason = MutationReasonCode::RetentionExpiry;
    MIRA_CHECK(fixture.memory->apply(retirement));
    MIRA_CHECK(fixture.memory->query(query).value().records.size() == 4);
    query.as_of_recorded.reset();
    MIRA_CHECK(fixture.memory->query(query).value().records.size() == 3);
    replay = write(fixture, record);
    MIRA_CHECK(replay && replay.value().count_of(CandidateDisposition::RejectedConflict) == 1);
    return 0;
}

int scoped_host_only_access_and_durable_erasure() {
    MemoryAgentFixture fixture;
    const SocialObjectKey object{{fixture.principal.tenant_id, Id128::generate()},
                                 Id128::generate()};
    const SocialObjectKey other_env{{object.environment.tenant, Id128::generate()}, object.object};
    const SocialObjectKey other_tenant{{TenantId::generate(), object.environment.environment},
                                       object.object};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "social.db";
    auto directory = SqliteSocialDirectory::open(fixture.executor, options);
    MIRA_CHECK(directory && directory.value()->activate(object, {1, {"Member"}}));
    const auto match = directory.value()->resolve(object.environment, "Member");
    MIRA_CHECK(match && match.value().matches.front().key == object);
    const auto now = std::chrono::system_clock::now();
    const auto source_event = EventId::generate();
    const auto local = make_social_attribute(object, {"team", Source::Inferred}, "Alpha",
                                             source_event, {now - 1h, {}}, now);
    const auto remote = make_social_attribute(other_env, {"team", Source::Inferred}, "Beta",
                                              source_event, {now - 1h, {}}, now);
    const auto foreign = make_social_attribute(other_tenant, {"team", Source::Inferred}, "Gamma",
                                               source_event, {now - 1h, {}}, now);
    MIRA_CHECK(local && remote && foreign);
    const auto &record = local.value();
    MIRA_CHECK(record.id != remote.value().id && record.id != foreign.value().id);
    auto model = write(fixture, record, true);
    MIRA_CHECK(model && model.value().count_of(CandidateDisposition::RejectedInvalid) == 1);
    auto forged = record;
    forged.verification = MemoryVerification::HumanConfirmed;
    MIRA_CHECK(!forged.validate());
    forged = record;
    forged.statement = default_forbidden_content_markers().front();
    auto forbidden = write(fixture, forged);
    MIRA_CHECK(forbidden &&
               forbidden.value().count_of(CandidateDisposition::RejectedForbidden) == 1);
    forged.statement = default_injection_content_markers().front();
    auto injection = write(fixture, forged);
    MIRA_CHECK(injection &&
               injection.value().count_of(CandidateDisposition::RejectedInjection) == 1);

    for (const auto *assertion : {&local, &remote, &foreign}) {
        auto written = write(fixture, assertion->value());
        MIRA_CHECK(written && written.value().count_of(CandidateDisposition::Applied) == 1);
    }
    MIRA_CHECK(fixture.memory->close());
    SqliteMemoryStoreOptions memory_options;
    memory_options.path = fixture.root / "memory.db";
    auto reopened = SqliteMemoryStore::open(fixture.executor, memory_options);
    MIRA_CHECK(reopened);
    fixture.memory = std::move(reopened).value();
    MemoryQuery query;
    query.scopes = {record.scope};
    MIRA_CHECK(!fixture.memory->query(query));
    query.social_attribute_field = "team";
    const auto stored = fixture.memory->query(query);
    MIRA_CHECK(stored && stored.value().records.size() == 1 && includes(stored.value(), record));
    query.scopes.push_back(remote.value().scope);
    MIRA_CHECK(!fixture.memory->query(query));
    query.scopes = {record.scope};
    MIRA_CHECK(!fixture.memory->get(remote.value().id, record.scope).value());

    MIRA_CHECK(directory.value()->erase_object(object));
    MIRA_CHECK(!directory.value()->read(object).value());
    ErasureRequest erase;
    erase.scope = record.scope;
    erase.reason = "host-profile-erasure";
    auto erased = fixture.memory->erase(erase);
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete);
    query.as_of_valid = now;
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    query.scopes = {foreign.value().scope};
    MIRA_CHECK(includes(fixture.memory->query(query).value(), foreign.value()));

    fixture.start();
    auto binding = fixture.binding();
    binding.scopes = {record.scope};
    const auto bound = fixture.service->bind(binding);
    MIRA_CHECK(!bound && bound.error().code == ErrorCode::PermissionDenied);
    return 0;
}
} // namespace

int main() {
    if (const int result = source_independence_and_temporal_recall(); result != 0)
        return result;
    return scoped_host_only_access_and_durable_erasure();
}
