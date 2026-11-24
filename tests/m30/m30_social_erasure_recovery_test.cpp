#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/social_profile.hpp>
#include <mira/sqlite_social_directory.hpp>
#include <sqlite3.h>

namespace {
using namespace mira;
using namespace mira::testing;

constexpr auto kRejectMemoryErase =
    "CREATE TRIGGER block_social_erase BEFORE DELETE ON memory_records"
    " BEGIN SELECT RAISE(ABORT, 'fixture memory not writable'); END;";
constexpr auto kRepairMemory = "DROP TRIGGER block_social_erase;";

bool change_db(const std::filesystem::path &path, const char *sql) {
    sqlite3 *raw = nullptr;
    const bool opened = sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK;
    const bool changed = opened && sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(raw);
    return changed;
}

bool reopen_memory(MemoryAgentFixture &fixture, const char *sql,
                   std::chrono::milliseconds operation_timeout = 30s) {
    if (!fixture.memory->close())
        return false;
    SqliteMemoryStoreOptions options;
    options.path = fixture.root / "memory.db";
    options.operation_timeout = operation_timeout;
    if (!change_db(options.path, sql))
        return false;
    auto opened = SqliteMemoryStore::open(fixture.executor, options);
    if (!opened)
        return false;
    fixture.memory = std::move(opened).value();
    return true;
}

Result<MemoryRecord> attribute(const SocialObjectKey &key) {
    const auto now = std::chrono::system_clock::now();
    return make_social_attribute(key, {"team", SocialAttributeSource::UserConfirmed},
                                 "Project team", EventId::generate(), {now - 1h, {}}, now);
}

Result<MemoryRecord> relationship(const SocialObjectKey &from, const SocialObjectKey &to) {
    const auto now = std::chrono::system_clock::now();
    return make_social_relationship({from, to}, {"mentors", SocialAttributeSource::UserConfirmed},
                                    "Project mentors", EventId::generate(), {now - 1h, {}}, now);
}

bool seed(MemoryAgentFixture &fixture, const MemoryRecord &record) {
    return fixture.executor
        .submit_auto([&] {
            MemoryCandidate candidate;
            candidate.proposed = record;
            candidate.evidence = record.provenance;
            auto report = MemoryConsolidator{}.consolidate_candidates(
                *fixture.memory, {candidate}, record.scope, Timestamp::now());
            return report && report.value().count_of(CandidateDisposition::Applied) == 1;
        })
        .get();
}

MemoryQuery historical_query(const MemoryRecord &record) {
    MemoryQuery query;
    query.scopes = {record.scope};
    query.as_of_valid = record.validity.valid_from;
    if (record.social_attribute)
        query.social_attribute_field = record.social_attribute->field;
    if (record.social_relationship)
        query.social_relationship_predicate = record.social_relationship->predicate;
    return query;
}

bool absent(SqliteMemoryStore &store, const MemoryRecord &record) {
    const auto historical = store.query(historical_query(record));
    const auto direct = store.get(record.id, record.scope);
    return historical && historical.value().records.empty() && direct && !direct.value();
}

int object_erasure_before_knowledge_completion() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey env{fixture.principal.tenant_id, Id128::generate()};
    const SocialObjectKey target{env, Id128::generate()};
    const SocialObjectKey neighbor{env, Id128::generate()};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "directory.db";
    {
        auto legacy = SqliteSocialDirectory::open(fixture.executor, options);
        MIRA_CHECK(legacy && legacy.value()->activate(target, {1, {"Target"}}));
    }
    MIRA_CHECK(change_db(options.path,
                         "DROP TABLE social_erasure_pending;"
                         "DROP TABLE social_accounts;"
                         "DROP TABLE social_messages;"
                         "UPDATE store_meta SET value='1.0' WHERE key='schema_version';"));

    const auto profile = attribute(target);
    const auto inbound = relationship(neighbor, target);
    const auto retained = attribute(neighbor);
    MIRA_CHECK(profile && inbound && retained);
    MIRA_CHECK(seed(fixture, profile.value()) && seed(fixture, inbound.value()) &&
               seed(fixture, retained.value()));
    MIRA_CHECK(reopen_memory(fixture, kRejectMemoryErase));
    auto opened = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
    MIRA_CHECK(opened);
    auto directory = std::move(opened).value();
    const auto legacy_alias = directory->resolve(env, "target");
    MIRA_CHECK(legacy_alias && legacy_alias.value().resolution == SocialAliasResolution::Unique);
    const SocialAccountKey account{"qq", Id128::generate()};
    const SocialAccountKey other_source{"bilibili", account.account};
    const auto evidence = EventId::generate();
    MIRA_CHECK(directory->observe_account(env, account, evidence, target));
    MIRA_CHECK(directory->observe_account(env, other_source, evidence, target));
    const auto recall = directory->recall_account_attribute(env, other_source, "team");
    MIRA_CHECK(recall && recall.value() && recall.value()->binding.object == target);
    MIRA_CHECK(recall.value()->assertions.records.size() == 1 &&
               recall.value()->assertions.records.front().id == profile.value().id);
    MIRA_CHECK(!directory->recall_account_attribute(env, other_source, "", 0));
    const SocialEnvironmentKey foreign{TenantId::generate(), env.environment};
    MIRA_CHECK(!directory->recall_account_attribute(foreign, other_source, "team").value());
    AppendRequest event;
    event.event_id = EventId::generate();
    event.session_id = SessionId::generate();
    event.runtime_id = fixture.runtime;
    event.payload = {"mira.social.message.received.v1", "message", EventClass::State};
    const auto committed = fixture.events->append(event);
    MIRA_CHECK(committed);
    const SocialMessageLocator locator{event.session_id, committed.value().session_sequence,
                                       event.event_id};
    MIRA_CHECK(directory->index_message(env, account, locator, *fixture.events).value().speaker ==
               target);
    auto failed = directory->erase_object(target);
    MIRA_CHECK(!failed && failed.error().domain_code ==
                              static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));
    MIRA_CHECK(!directory->resolve(env, "target"));
    MIRA_CHECK(!directory->resolve_account(env, account));
    MIRA_CHECK(!directory->recall_account_attribute(env, other_source, "team"));
    MIRA_CHECK(!directory->read_message_evidence(env, locator.session, 0, locator.sequence));
    MIRA_CHECK(!directory->activate(neighbor, {1, {"Neighbor"}}));
    directory.reset();

    MIRA_CHECK(!SqliteSocialDirectory::open(fixture.executor, options));
    SqliteMemoryStoreOptions unrelated_options;
    unrelated_options.path = fixture.root / "slipped-store.db";
    auto unrelated = SqliteMemoryStore::open(fixture.executor, unrelated_options);
    MIRA_CHECK(unrelated);
    std::shared_ptr<SqliteMemoryStore> wrong_store{std::move(unrelated).value()};
    MIRA_CHECK(!SqliteSocialDirectory::open(fixture.executor, options, wrong_store));
    MIRA_CHECK(!SqliteSocialDirectory::open(fixture.executor, options, fixture.memory));

    MIRA_CHECK(reopen_memory(fixture, kRepairMemory));
    auto recovered = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
    MIRA_CHECK(recovered);
    MIRA_CHECK(absent(*fixture.memory, profile.value()) &&
               absent(*fixture.memory, inbound.value()));
    MIRA_CHECK(fixture.memory->get(retained.value().id, retained.value().scope).value());
    MIRA_CHECK(recovered.value()->resolve(env, "target").value().matches.empty());
    const auto replay = recovered.value()->observe_account(env, account, evidence);
    MIRA_CHECK(!replay && replay.error().code == ErrorCode::PermissionDenied);
    const auto source_page =
        recovered.value()->read_message_evidence(env, locator.session, 0, locator.sequence);
    MIRA_CHECK(source_page && source_page.value().evidence.empty());
    const auto source_replay =
        recovered.value()->index_message(env, account, locator, *fixture.events);
    MIRA_CHECK(!source_replay && source_replay.error().code == ErrorCode::PermissionDenied);
    const auto late = recovered.value()->activate(target, {2, {"Late"}});
    MIRA_CHECK(!late && late.error().code == ErrorCode::PermissionDenied);
    MIRA_CHECK(recovered.value()->erase_object(target));
    return 0;
}

int environment_erasure_survives_final_ack_failure() {
    MemoryAgentFixture fixture;
    const SocialEnvironmentKey env{fixture.principal.tenant_id, Id128::generate()};
    const SocialEnvironmentKey adjacent{env.tenant, Id128::generate()};
    const SocialEnvironmentKey foreign{TenantId::generate(), env.environment};
    const SocialObjectKey registered{env, Id128::generate()};
    const SocialObjectKey memory_only{env, Id128::generate()};
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "directory.db";
    {
        auto opened = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
        MIRA_CHECK(opened && opened.value()->activate(registered, {1, {"Member"}}));
        MIRA_CHECK(opened.value()->activate({adjacent, registered.object}, {1, {"Member"}}));
    }
    MIRA_CHECK(change_db(options.path,
                         "CREATE TRIGGER reject_ack BEFORE DELETE ON social_erasure_pending"
                         " BEGIN SELECT RAISE(ABORT, 'fixture cannot acknowledge'); END;"));

    const auto left = attribute(registered);
    const auto right = attribute(memory_only);
    const auto both = relationship(registered, memory_only);
    const auto next_room = attribute({adjacent, registered.object});
    const auto next_tenant = attribute({foreign, registered.object});
    for (const auto *record : {&left, &right, &both, &next_room, &next_tenant}) {
        MIRA_CHECK(*record && seed(fixture, record->value()));
    }
    auto broad = historical_query(left.value());
    broad.scopes = {social_environment_scope(env).value()};
    MIRA_CHECK(!fixture.memory->query(broad));

    MIRA_CHECK(reopen_memory(fixture, kRejectMemoryErase));
    {
        auto opened = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
        MIRA_CHECK(opened && !opened.value()->erase_environment(env));
        for (const auto *record : {&left, &right, &both}) {
            const auto held = fixture.memory->query(historical_query(record->value()));
            MIRA_CHECK(!held && held.error().domain_code ==
                                    static_cast<std::int32_t>(MemoryDomainCode::ErasurePending));
            MIRA_CHECK(!fixture.memory->get(record->value().id, record->value().scope));
        }
        MIRA_CHECK(
            fixture.memory->query(historical_query(next_room.value())).value().records.size() == 1);
    }
    MIRA_CHECK(reopen_memory(fixture, kRepairMemory));
    MIRA_CHECK(!SqliteSocialDirectory::open(fixture.executor, options, fixture.memory));
    for (const auto *record : {&left, &right, &both}) {
        MIRA_CHECK(absent(*fixture.memory, record->value()));
    }
    MIRA_CHECK(change_db(options.path, "DROP TRIGGER reject_ack;"));
    auto recovered = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
    MIRA_CHECK(recovered && recovered.value()->resolve(env, "Member").value().matches.empty());
    MIRA_CHECK(recovered.value()->resolve(adjacent, "Member").value().matches.size() == 1);
    const auto late = recovered.value()->activate(memory_only, {1, {"Late"}});
    MIRA_CHECK(!late && late.error().code == ErrorCode::PermissionDenied);
    for (const auto *record : {&next_room, &next_tenant}) {
        MIRA_CHECK(
            fixture.memory->query(historical_query(record->value())).value().records.size() == 1);
    }
    return 0;
}

int accepted_erase_can_finish_after_caller_timeout() {
    MemoryAgentFixture fixture;
    const SocialObjectKey target{{fixture.principal.tenant_id, Id128::generate()},
                                 Id128::generate()};
    const auto record = attribute(target);
    MIRA_CHECK(record && seed(fixture, record.value()));
    MIRA_CHECK(reopen_memory(fixture, "SELECT 1;", 500ms));
    SqliteSocialDirectoryOptions options;
    options.path = fixture.root / "directory.db";
    {
        auto opened = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
        MIRA_CHECK(opened && opened.value()->activate(target, {1, {"Member"}}));
        fixture.memory->set_worker_paused(true);
        const auto timed_out = opened.value()->erase_object(target);
        const auto queued = fixture.memory->pending_requests();
        const auto fenced = opened.value()->read(target);
        fixture.memory->set_worker_paused(false);
        MIRA_CHECK(!timed_out && timed_out.error().code == ErrorCode::DeadlineExceeded &&
                   queued == 1 && !fenced);
    }
    MIRA_CHECK(reopen_memory(fixture, "SELECT 1;"));
    MIRA_CHECK(absent(*fixture.memory, record.value()));
    auto recovered = SqliteSocialDirectory::open(fixture.executor, options, fixture.memory);
    MIRA_CHECK(recovered && !recovered.value()->read(target).value());
    return 0;
}
} // namespace

int main() {
    if (const int result = object_erasure_before_knowledge_completion(); result != 0)
        return result;
    if (const int result = environment_erasure_survives_final_ack_failure(); result != 0)
        return result;
    return accepted_erase_can_finish_after_caller_timeout();
}
