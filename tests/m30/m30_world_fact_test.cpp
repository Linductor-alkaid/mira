#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/world_fact.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

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

bool reads(SqliteMemoryStore &memory, const WorldFactQuery &query, const MemoryRecord &record) {
    const auto lookup = memory.lookup_world_fact(query);
    return lookup && lookup.value().state == WorldFactState::Current && lookup.value().record &&
           memory_record_to_json(*lookup.value().record) == memory_record_to_json(record);
}

int exact_scope_and_freshness() {
    MemoryAgentFixture fixture;
    const auto tenant = fixture.principal.tenant_id;
    const auto room = Id128::generate();
    const auto app = Id128::generate();
    const auto entity = Id128::generate();
    const WorldProperty property{entity, "display_scale"};
    const auto environment = world_environment_scope(tenant, room).value();
    const auto other_environment = world_environment_scope(tenant, Id128::generate()).value();
    const auto application = world_application_scope(tenant, room, app).value();
    const auto other_app = world_application_scope(tenant, room, Id128::generate()).value();
    const auto foreign = world_application_scope(TenantId::generate(), room, app).value();
    const auto now = std::chrono::system_clock::now();
    const auto event = EventId::generate();
    const auto old = make_world_observation(environment, property, "100%", event,
                                            {now - 4h, now + 4h}, now - 3h);
    const auto expired = make_world_observation(environment, property, "125%", EventId::generate(),
                                                {now - 2h, now - 1h}, now - 90min);
    const auto delayed = make_world_observation(environment, property, "110%", EventId::generate(),
                                                {now - 3h, now + 4h}, now - 20min);
    const auto legacy_remote = make_world_observation(other_environment, property, "200%", event,
                                                      {now - 1h, now + 1h}, now - 1h);
    const auto local_app = make_world_observation(application, property, "150%", event,
                                                  {now - 1h, now + 1h}, now - 1h);
    const auto remote_app =
        make_world_observation(other_app, property, "175%", event, {now - 1h, now + 1h}, now - 1h);
    const auto remote_tenant =
        make_world_observation(foreign, property, "300%", event, {now - 1h, now + 1h}, now - 1h);
    for (const auto *record :
         {&old, &expired, &delayed, &legacy_remote, &local_app, &remote_app, &remote_tenant}) {
        MIRA_CHECK(*record);
        const auto written = write(fixture, record->value());
        MIRA_CHECK(written && written.value().count_of(CandidateDisposition::Applied) == 1);
    }
    MIRA_CHECK(old.value().id != legacy_remote.value().id &&
               local_app.value().id != remote_tenant.value().id);

    WorldFactQuery query{environment, property, {}, {}};
    auto stale = fixture.memory->lookup_world_fact(query);
    MIRA_CHECK(stale && stale.value().state == WorldFactState::Expired &&
               stale.value().requires_observation() && !stale.value().record);
    query.as_of_valid = now - 90min;
    MIRA_CHECK(reads(*fixture.memory, query, expired.value()));
    query.as_of_recorded = now - 100min;
    MIRA_CHECK(reads(*fixture.memory, query, old.value()));
    query.as_of_recorded.reset();
    query.as_of_valid = expired.value().validity.valid_until;
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Expired);
    query.as_of_valid.reset();
    query.property.property = "color_scheme";
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.property = property;
    query.property.entity = Id128::generate();
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.property = property;
    for (const auto *record : {&legacy_remote, &local_app, &remote_app, &remote_tenant}) {
        query.scope = record->value().scope;
        MIRA_CHECK(reads(*fixture.memory, query, record->value()));
    }

    const auto refreshed = make_world_observation(
        environment, property, "125%", EventId::generate(), {now - 10min, now + 2h}, now - 5min);
    MIRA_CHECK(refreshed &&
               write(fixture, refreshed.value()).value().count_of(CandidateDisposition::Applied) ==
                   1);
    query.scope = environment;
    MIRA_CHECK(reads(*fixture.memory, query, refreshed.value()));
    const auto confirmed = write(fixture, refreshed.value());
    MIRA_CHECK(confirmed && confirmed.value().entries.front().idempotent_replay);
    auto conflicting = refreshed.value();
    conflicting.statement = "130%";
    auto rejection = write(fixture, conflicting);
    MIRA_CHECK(rejection &&
               rejection.value().count_of(CandidateDisposition::RejectedConflict) == 1);

    MIRA_CHECK(fixture.memory->close());
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    auto reopened = SqliteMemoryStore::open(fixture.executor, options);
    MIRA_CHECK(reopened);
    fixture.memory = std::move(reopened).value();
    MIRA_CHECK(reads(*fixture.memory, query, refreshed.value()));

    ErasureRequest erase;
    erase.scope = environment;
    erase.reason = "fixture world erasure";
    const auto erased = fixture.memory->erase(erase);
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete &&
               erased.value().counts.records_removed == 4);
    query.as_of_valid = now - 90min;
    MIRA_CHECK(fixture.memory->lookup_world_fact(query).value().state == WorldFactState::Missing);
    query.as_of_valid.reset();
    query.scope = application;
    MIRA_CHECK(reads(*fixture.memory, query, local_app.value()));
    return 0;
}

int ambiguous_and_untrusted_observation() {
    MemoryAgentFixture fixture;
    const auto scope =
        world_environment_scope(fixture.principal.tenant_id, Id128::generate()).value();
    const WorldProperty property{Id128::generate(), "orientation"};
    const auto now = std::chrono::system_clock::now();
    const auto build = [&](std::string value, ValidityInterval validity = {}) {
        if (validity.valid_from == std::chrono::system_clock::time_point{})
            validity = {now - 1h, now + 1h};
        return make_world_observation(scope, property, std::move(value), EventId::generate(),
                                      validity, now);
    };
    MIRA_CHECK(!build("Portrait", {now - 1h, {}}));
    const auto fact = build("Portrait");
    const auto tie = build("Landscape");
    MIRA_CHECK(fact && tie);
    auto untrusted = write(fixture, fact.value(), true);
    MIRA_CHECK(untrusted && untrusted.value().count_of(CandidateDisposition::RejectedInvalid) == 1);
    auto unsafe = build(default_forbidden_content_markers().front());
    MIRA_CHECK(
        unsafe &&
        write(fixture, unsafe.value()).value().count_of(CandidateDisposition::RejectedForbidden) ==
            1);
    MIRA_CHECK(write(fixture, fact.value()).value().count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(write(fixture, tie.value()).value().count_of(CandidateDisposition::Applied) == 1);
    auto conflict = fixture.memory->lookup_world_fact({scope, property, {}, {}});
    MIRA_CHECK(conflict && conflict.value().state == WorldFactState::Ambiguous &&
               conflict.value().requires_observation() && !conflict.value().record);

    MemoryQuery raw;
    raw.scopes = {scope};
    MIRA_CHECK(!fixture.memory->query(raw));
    fixture.start();
    auto binding = fixture.binding();
    binding.scopes = {scope};
    MIRA_CHECK(!fixture.service->bind(binding));
    return 0;
}
} // namespace

int main() {
    if (const int result = exact_scope_and_freshness(); result != 0)
        return result;
    return ambiguous_and_untrusted_observation();
}
