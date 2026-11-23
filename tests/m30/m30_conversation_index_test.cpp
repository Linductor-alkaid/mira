#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/sqlite_conversation_index.hpp>

namespace {
using namespace mira;
using namespace mira::testing;

AppendRequest speech(const MemoryAgentFixture &fixture, const MemoryWorkBinding &work,
                     std::string type, std::string text) {
    AppendRequest event;
    event.event_id = EventId::generate();
    event.runtime_id = fixture.runtime;
    event.session_id = work.work.session_id;
    event.task_id = work.work.task_id;
    event.payload = {
        std::move(type),
        to_json_string(JsonValue::Object{{"detail", JsonValue::Object{{"text", std::move(text)}}}}),
        EventClass::State};
    return event;
}

Result<SessionSequence> rebuild(MemoryAgentFixture &fixture, SqliteConversationIndex &index,
                                const MemoryWorkBinding &work, const IEventStore &events) {
    return fixture.executor
        .submit_auto([&] {
            return index.rebuild(work.principal.tenant_id, work.work.session_id, events,
                                 memory_context(work.work));
        })
        .get();
}

ContextQuery query_for(const MemoryWorkBinding &work, std::string text = {}) {
    ContextQuery query;
    query.session = work.work.session_id;
    query.scopes = work.scopes;
    query.text = std::move(text);
    return query;
}

int durable_recall_rebuild_and_scope_checks() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    const auto other = fixture.binding();
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "history.db";
    options.segmentation.window_entries = 2;
    options.max_segments = 3;
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::move(opened).value();
    auto user = speech(fixture, work, "UserMessageInjected", "remember cedar volume");
    auto assistant = speech(fixture, work, "AssistantSpeech", "cedar confirmed");
    MIRA_CHECK(fixture.events->append(user));
    MIRA_CHECK(fixture.events->append(assistant));
    MIRA_CHECK(fixture.events->append(speech(fixture, other, "AssistantSpeech", "cedar other")));
    auto trailing = speech(fixture, work, "TaskStateChanged", "unprojected");
    MIRA_CHECK(fixture.events->append(trailing));
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 3);
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 3);
    MIRA_CHECK(rebuild(fixture, *index, other, *fixture.events));
    MIRA_CHECK(index->close());
    index.reset();
    opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    index = std::move(opened).value();

    auto query = query_for(work, "cedar");
    const auto found = index->retrieve(query, {});
    MIRA_CHECK(found && found.value().quality.fts_leg_ran && found.value().candidates.size() == 1);
    const auto &candidate = found.value().candidates.front();
    MIRA_CHECK(candidate.through_event_sequence == 2);
    MIRA_CHECK(candidate.source_events ==
               (std::vector<EventId>{user.event_id, assistant.event_id}));
    const auto rendered = context_items_from_candidate(candidate);
    MIRA_CHECK(rendered.size() == 2 && rendered[0].wire_role == ModelRole::User &&
               rendered[1].wire_role == ModelRole::Assistant);
    query.exact_terms = {"unmatched"};
    MIRA_CHECK(index->retrieve(query, {}).value().candidates.empty());
    query.exact_terms = {"VOLUME"};
    MIRA_CHECK(index->retrieve(query, {}).value().candidates.size() == 1);

    query = query_for(work, "cedar");
    query.scopes = other.scopes;
    MIRA_CHECK(index->retrieve(query, {}).value().candidates.empty());
    query.scopes.clear();
    MIRA_CHECK(index->retrieve(query, {}).value().candidates.empty());
    auto foreign_tenant = work;
    foreign_tenant.principal.tenant_id = TenantId::generate();
    foreign_tenant.scopes = {memory_session_scope(foreign_tenant.principal, work.work.session_id)};
    MIRA_CHECK(index->retrieve(query_for(foreign_tenant, "cedar"), {}).value().candidates.empty());
    const auto foreign_rebuild = rebuild(fixture, *index, foreign_tenant, *fixture.events);
    MIRA_CHECK(!foreign_rebuild && foreign_rebuild.error().code == ErrorCode::PermissionDenied);

    query = query_for(work, "cedar");
    query.query_embedding = ContextEmbedding{{1.0F}, ModelProfileId::generate()};
    const auto degraded = index->retrieve(query, {});
    MIRA_CHECK(degraded && degraded.value().quality.vector_degraded &&
               degraded.value().candidates.size() == 1);
    query.text.clear();
    MIRA_CHECK(index->retrieve(query, {}).value().candidates.empty());
    RetrievalBudget no_time;
    no_time.deadline = 0ms;
    MIRA_CHECK(
        index->retrieve(query_for(work, "cedar"), no_time).value().quality.deadline_exceeded);

    MemoryEventStore older;
    MIRA_CHECK(older.append(user));
    const auto stale = rebuild(fixture, *index, work, older);
    MIRA_CHECK(!stale && stale.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(older.append(speech(fixture, work, "AssistantSpeech", "different")));
    MIRA_CHECK(older.append(trailing));
    const auto changed = rebuild(fixture, *index, work, older);
    MIRA_CHECK(!changed && changed.error().code == ErrorCode::DataLoss);
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "AssistantSpeech", "birch followup")));
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 4);
    MIRA_CHECK(index->retrieve(query_for(work, "birch"), {}).value().candidates.size() == 1);
    MIRA_CHECK(
        fixture.events->append(speech(fixture, work, "AssistantSpeech", "redwood followup")));
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "AssistantSpeech", "elm followup")));
    const auto no_space = rebuild(fixture, *index, work, *fixture.events);
    MIRA_CHECK(!no_space && no_space.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(index->retrieve(query_for(work, "redwood"), {}).value().candidates.empty());
    MIRA_CHECK(index->retrieve(query_for(work, "birch"), {}).value().candidates.size() == 1);

    const auto marker = default_forbidden_content_markers().front();
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "AssistantSpeech", marker)));
    const auto unsafe = rebuild(fixture, *index, work, *fixture.events);
    MIRA_CHECK(!unsafe && unsafe.error().code == ErrorCode::SafetyRejected);
    MIRA_CHECK(index->retrieve(query_for(work, "birch"), {}).value().candidates.size() == 1);
    MIRA_CHECK(index->close());
    MIRA_CHECK(!index->retrieve(query_for(work, "cedar"), {}));
    return 0;
}

int joint_erasure_and_recovery() {
    MemoryAgentFixture fixture;
    const auto first = fixture.binding();
    const auto second = fixture.binding();
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "history.db";
    options.max_erasure_barriers = 1;
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::shared_ptr<SqliteConversationIndex>(std::move(opened).value());
    MemoryAgentConfig config;
    config.conversation_index = index;
    fixture.start({}, config);
    for (const auto &work : {first, second}) {
        MIRA_CHECK(fixture.service->bind(work));
        MIRA_CHECK(fixture.seed(work, "state=before"));
        MIRA_CHECK(
            fixture.events->append(speech(fixture, work, "AssistantSpeech", "forget payload")));
        MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events));
    }
    MIRA_CHECK(fixture.service->erase_scope(first.scopes.front()));
    MIRA_CHECK(index->retrieve(query_for(first, "payload"), {}).value().candidates.empty());
    MIRA_CHECK(index->retrieve(query_for(second, "payload"), {}).value().candidates.size() == 1);
    MIRA_CHECK(!rebuild(fixture, *index, first, *fixture.events));
    MemoryEventStore malformed;
    auto invalid = speech(fixture, first, "AssistantSpeech", "unused");
    invalid.payload.data = "{";
    MIRA_CHECK(malformed.append(invalid));
    const auto blocked = rebuild(fixture, *index, first, malformed);
    MIRA_CHECK(!blocked && blocked.error().code == ErrorCode::PermissionDenied);

    // The durable MemoryAgent intent survives an index-side capacity failure.
    const auto partial = fixture.service->erase_scope(second.scopes.front());
    MIRA_CHECK(!partial && partial.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(!index->retrieve(query_for(second, "payload"), {}));
    MemoryQuery memories;
    memories.scopes = second.scopes;
    MIRA_CHECK(fixture.memory->query(memories).value().records.empty());
    MIRA_CHECK(fixture.service->close());
    fixture.service.reset();
    config.conversation_index.reset();
    MIRA_CHECK(index->close());
    index.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");

    options.max_erasure_barriers = 2;
    opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    index = std::shared_ptr<SqliteConversationIndex>(std::move(opened).value());
    config.conversation_index = index;
    fixture.start({}, config);
    for (const auto &work : {first, second}) {
        MIRA_CHECK(!fixture.service->bind(work));
        const auto queried = index->retrieve(query_for(work, "payload"), {});
        MIRA_CHECK(queried && queried.value().candidates.empty());
        const auto replay = rebuild(fixture, *index, work, *fixture.events);
        MIRA_CHECK(!replay && replay.error().code == ErrorCode::PermissionDenied);
    }
    return 0;
}
} // namespace

int main() {
    if (const auto result = durable_recall_rebuild_and_scope_checks(); result != 0)
        return result;
    return joint_erasure_and_recovery();
}
