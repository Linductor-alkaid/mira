#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/social_profile.hpp>
#include <sqlite3.h>

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

MemoryQuery by_predicate(const MemoryRecord &record) {
    MemoryQuery query;
    query.scopes = {record.scope};
    query.social_relationship_predicate = record.social_relationship->predicate;
    return query;
}

bool change_db_and_reopen(MemoryAgentFixture &fixture, const char *sql) {
    if (!fixture.memory->close())
        return false;
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    sqlite3 *raw = nullptr;
    const bool opened = sqlite3_open(options.path.string().c_str(), &raw) == SQLITE_OK;
    const bool changed = opened && sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(raw);
    if (!changed)
        return false;
    auto store = SqliteMemoryStore::open(fixture.executor, options);
    if (!store)
        return false;
    fixture.memory = std::move(store).value();
    return true;
}

int directed_scope_evidence_and_time() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey env{fixture.principal.tenant_id, Id128::generate()};
    const SocialObjectKey from{env, Id128::generate()};
    const SocialObjectKey to{env, Id128::generate()};
    const SocialObjectKey other_env{{env.tenant, Id128::generate()}, to.object};
    const SocialObjectKey other_tenant{{TenantId::generate(), env.environment}, to.object};
    const SocialRelationshipKey forward{from, to};
    const SocialRelationshipKey reverse{to, from};
    MIRA_CHECK(!social_relationship_scope({from, other_env}));
    MIRA_CHECK(!social_relationship_scope({from, other_tenant}));
    MIRA_CHECK(!social_relationship_scope({from, from}));

    const auto now = std::chrono::system_clock::now();
    const auto recorded = now - 2h;
    const auto event = EventId::generate();
    const ValidityInterval active{now - 4h, now + 2h};
    const auto asserted =
        make_social_relationship(forward, {"mentors", Source::SelfReported},
                                 "Mentor for the project", event, active, recorded);
    const auto confirmed =
        make_social_relationship(forward, {"mentors", Source::UserConfirmed},
                                 "Mentor for the project", event, active, recorded);
    const auto reversed = make_social_relationship(reverse, {"mentors", Source::Inferred},
                                                   "Reverse inference", event, active, recorded);
    const auto previous =
        make_social_relationship(forward, {"mentors", Source::SelfReported}, "Previous engagement",
                                 EventId::generate(), {now - 8h, now - 4h}, recorded);
    const auto another = make_social_relationship(forward, {"works_with", Source::UserConfirmed},
                                                  "A separate relation", event, active, recorded);
    MIRA_CHECK(asserted && confirmed && reversed && previous && another);
    MIRA_CHECK(!make_social_relationship(forward, {"Mentors", Source::SelfReported},
                                         "invalid predicate", event, active, recorded));
    MIRA_CHECK(social_relationship_endpoints(asserted.value().scope) == forward);
    MIRA_CHECK(asserted.value().id != confirmed.value().id &&
               asserted.value().scope != reversed.value().scope);
    auto round_trip = memory_record_from_json(memory_record_to_json(confirmed.value()));
    MIRA_CHECK(round_trip &&
               round_trip.value().verification == MemoryVerification::HumanConfirmed &&
               round_trip.value().social_relationship->source == Source::UserConfirmed &&
               round_trip.value().provenance == std::vector<EventId>{event});
    auto spoofed = asserted.value();
    spoofed.scope = reversed.value().scope;
    MIRA_CHECK(!spoofed.validate());
    const auto model = write(fixture, asserted.value(), true);
    MIRA_CHECK(model && model.value().count_of(CandidateDisposition::RejectedInvalid) == 1);

    for (const auto *record : {&asserted, &confirmed, &reversed, &previous, &another}) {
        auto report = write(fixture, record->value());
        MIRA_CHECK(report && report.value().count_of(CandidateDisposition::Applied) == 1);
    }
    auto replay = write(fixture, asserted.value());
    MIRA_CHECK(replay && replay.value().entries.front().idempotent_replay);
    auto conflict = asserted.value();
    conflict.statement = "Different statement from the same event";
    auto rejected = write(fixture, conflict);
    MIRA_CHECK(rejected && rejected.value().count_of(CandidateDisposition::RejectedConflict) == 1);

    auto query = by_predicate(asserted.value());
    auto current = fixture.memory->query(query);
    MIRA_CHECK(current && current.value().records.size() == 2);
    const auto returned = [&](MemoryId id) {
        return std::ranges::any_of(current.value().records,
                                   [&](const auto &record) { return record.id == id; });
    };
    MIRA_CHECK(returned(asserted.value().id) && returned(confirmed.value().id));
    auto backward = fixture.memory->query(by_predicate(reversed.value()));
    MIRA_CHECK(backward && backward.value().records.size() == 1 &&
               backward.value().records.front().id == reversed.value().id);
    auto wrong_scope = fixture.memory->get(asserted.value().id, reversed.value().scope);
    MIRA_CHECK(wrong_scope && !wrong_scope.value());
    query.as_of_valid = now - 6h;
    auto history = fixture.memory->query(query);
    MIRA_CHECK(history && history.value().records.size() == 1 &&
               history.value().records.front().id == previous.value().id);
    query.as_of_recorded = recorded - 1h;
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    query.scopes = {social_object_scope(from).value()};
    MIRA_CHECK(!fixture.memory->query(query));
    query.scopes = {asserted.value().scope};
    query.social_relationship_predicate.reset();
    MIRA_CHECK(!fixture.memory->query(query));

    fixture.start();
    auto binding = fixture.binding();
    binding.scopes = {asserted.value().scope};
    MIRA_CHECK(!fixture.service->bind(binding));
    return 0;
}

int object_erasure_removes_both_directions_and_holds_pending() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey env{fixture.principal.tenant_id, Id128::generate()};
    const SocialObjectKey left{env, Id128::generate()};
    const SocialObjectKey erased{env, Id128::generate()};
    const SocialObjectKey right{env, Id128::generate()};
    const SocialEnvironmentKey foreign{TenantId::generate(), env.environment};
    const auto now = std::chrono::system_clock::now();
    const auto relation = [&](const SocialRelationshipKey &pair) {
        return make_social_relationship(pair, {"works_with", Source::UserConfirmed}, "Project peer",
                                        EventId::generate(), {now - 1h, {}}, now);
    };
    const auto outgoing = relation({erased, right});
    const auto incoming = relation({left, erased});
    const auto unaffected = relation({left, right});
    const auto foreign_relation = relation({{foreign, left.object}, {foreign, erased.object}});
    const auto attribute = make_social_attribute(erased, {"team", Source::SelfReported}, "Blue",
                                                 EventId::generate(), {now - 1h, {}}, now);
    MIRA_CHECK(outgoing && incoming && unaffected && foreign_relation && attribute);
    auto written = write(fixture, attribute.value());
    MIRA_CHECK(written && written.value().count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(change_db_and_reopen(
        fixture,
        "DROP INDEX memory_records_by_social_predicate;"
        "DROP INDEX memory_records_by_social_from;"
        "DROP INDEX memory_records_by_social_to;"
        "UPDATE store_meta SET value='1.4' WHERE key='schema_version';"
        "UPDATE memory_records SET schema_minor=3, document=json_set(document, "
        "'$.schema_version.minor', 3);"
        "UPDATE memory_versions SET document=json_set(document, '$.schema_version.minor', 3);"));
    MIRA_CHECK(fixture.memory->diagnostics().disposition == StoreSchemaDisposition::Migrated);
    MemoryQuery profile;
    profile.scopes = {attribute.value().scope};
    profile.social_attribute_field = "team";
    auto migrated_profile = fixture.memory->query(profile);
    MIRA_CHECK(migrated_profile && migrated_profile.value().records.size() == 1 &&
               migrated_profile.value().records.front().id == attribute.value().id);
    for (const auto *record : {&outgoing, &incoming, &unaffected, &foreign_relation}) {
        auto report = write(fixture, record->value());
        MIRA_CHECK(report && report.value().count_of(CandidateDisposition::Applied) == 1);
    }

    MIRA_CHECK(change_db_and_reopen(
        fixture, "CREATE TRIGGER reject_relationship_removal"
                 " BEFORE DELETE ON memory_records WHEN OLD.kind = 'social-relationship'"
                 " BEGIN SELECT RAISE(ABORT, 'fixture failure'); END;"
                 "CREATE TRIGGER reject_hold BEFORE INSERT ON memory_scope_holds"
                 " BEGIN SELECT RAISE(ABORT, 'hold fixture failure'); END;"));
    ErasureRequest erase;
    erase.scope = attribute.value().scope;
    erase.reason = "host-profile-erasure";
    const auto unprotected = fixture.memory->erase(erase);
    MIRA_CHECK(!unprotected);
    MIRA_CHECK(change_db_and_reopen(fixture, "DROP TRIGGER reject_hold;"));
    auto pending = fixture.memory->erase(erase);
    MIRA_CHECK(pending && pending.value().status == ErasureStatus::Pending &&
               pending.value().held_scope == erase.scope);
    MIRA_CHECK(change_db_and_reopen(fixture, "DROP TRIGGER reject_relationship_removal;"));
    for (const auto *affected : {&incoming, &outgoing}) {
        const auto &record = affected->value();
        auto blocked = fixture.memory->query(by_predicate(record));
        MIRA_CHECK(!blocked && blocked.error().domain_code ==
                                   static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));
        auto get = fixture.memory->get(record.id, record.scope);
        MIRA_CHECK(!get && get.error().domain_code == blocked.error().domain_code);
        auto refused = write(fixture, record);
        MIRA_CHECK(refused &&
                   refused.value().count_of(CandidateDisposition::RejectedConflict) == 1);
    }
    MIRA_CHECK(fixture.memory->query(by_predicate(unaffected.value())).value().records.size() == 1);

    auto settled = fixture.memory->erase(erase);
    MIRA_CHECK(settled && settled.value().status == ErasureStatus::Complete &&
               settled.value().counts.records_removed == 3);
    for (const auto *affected : {&incoming, &outgoing}) {
        auto history = by_predicate(affected->value());
        history.as_of_valid = now;
        MIRA_CHECK(fixture.memory->query(history).value().records.empty());
    }
    MIRA_CHECK(!fixture.memory->get(attribute.value().id, *erase.scope).value());
    MIRA_CHECK(
        fixture.memory->query(by_predicate(foreign_relation.value())).value().records.size() == 1);
    MIRA_CHECK(fixture.memory->query(by_predicate(unaffected.value())).value().records.size() == 1);
    return 0;
}
} // namespace

int main() {
    if (const int result = directed_scope_evidence_and_time(); result != 0)
        return result;
    return object_erasure_removes_both_directions_and_holds_pending();
}
