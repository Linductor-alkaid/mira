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

class CountingSource final : public IEventStore {
  public:
    explicit CountingSource(IEventStore &source) : source_(source) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        return source_.append(request);
    }
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest> batch) override {
        return source_.append_batch(batch);
    }
    Result<EventPage> read(const EventQuery &query) const override { return source_.read(query); }
    Result<EventSlicePage> read_slice(const EventSliceQuery &query) const override {
        auto page = source_.read_slice(query);
        if (page)
            events_read += page.value().events.size();
        if (on_slice)
            on_slice(query);
        return page;
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return source_.recover(options);
    }
    Result<void> flush(Durability durability) override { return source_.flush(durability); }

    mutable std::size_t events_read = 0;
    std::function<void(const EventSliceQuery &)> on_slice;

  private:
    IEventStore &source_;
};

int incremental_batches_match_one_pass_and_keep_the_previous_snapshot_on_failure() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    CountingSource source(*fixture.events);
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "incremental.db";
    options.max_source_events = 4;
    options.segmentation.window_entries = 3;
    options.segmentation.max_segment_bytes = 80;
    std::size_t warnings = 0;
    options.capacity_warning = [&](const Error &error) {
        if (error.code == ErrorCode::ResourceExhausted)
            ++warnings;
    };
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::move(opened).value();
    const auto say = [&](std::string text) {
        return source.append(speech(fixture, work, "AssistantSpeech", std::move(text)));
    };
    MIRA_CHECK(say("cedar one") && say("cedar two"));
    MIRA_CHECK(rebuild(fixture, *index, work, source).value() == 2);
    MIRA_CHECK(index->close());
    index.reset();
    opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    index = std::move(opened).value();

    for (int i = 3; i <= 43; ++i) {
        if (i % 7 == 0) {
            MIRA_CHECK(source.append(speech(fixture, work, "TaskStateChanged", "noise")));
        } else if (i % 9 == 0) {
            MIRA_CHECK(say("cedar " + std::string(150, 's')));
        } else {
            MIRA_CHECK(say("cedar number " + std::to_string(i)));
        }
    }
    for (int retry = 0; retry < 2; ++retry) {
        auto overflow = rebuild(fixture, *index, work, source);
        MIRA_CHECK(!overflow && overflow.error().code == ErrorCode::ResourceExhausted);
    }
    MIRA_CHECK(warnings == 1);
    MIRA_CHECK(index->retrieve(query_for(work, "cedar"), {})
                   .value()
                   .candidates.front()
                   .through_event_sequence == 2);
    SessionSequence through = 2;
    while (through < 43) {
        source.events_read = 0;
        const auto next =
            fixture.executor
                .submit_auto([&] {
                    return index->advance(work.principal.tenant_id, work.work.session_id, source,
                                          memory_context(work.work));
                })
                .get();
        MIRA_CHECK(next && next.value().through > through && next.value().through - through <= 4 &&
                   next.value().snapshot_through == 43);
        MIRA_CHECK(source.events_read <= 6);
        through = next.value().through;
    }

    options.path = fixture.root / "one-pass.db";
    options.max_source_events = 128;
    auto full = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(full && rebuild(fixture, *full.value(), work, source).value() == 43);
    RetrievalBudget budget;
    budget.top_k = 64;
    for (const auto *text : {"", "cedar"}) {
        const auto query = query_for(work, text);
        const auto expected = full.value()->retrieve(query, budget);
        const auto actual = index->retrieve(query, budget);
        MIRA_CHECK(expected && actual && expected.value().candidates.size() > 8 &&
                   actual.value().candidates.size() == expected.value().candidates.size());
        for (std::size_t i = 0; i < expected.value().candidates.size(); ++i)
            MIRA_CHECK(context_candidate_to_json(actual.value().candidates[i]) ==
                       context_candidate_to_json(expected.value().candidates[i]));
    }
    MIRA_CHECK(say("cedar late"));
    bool published = false;
    source.on_slice = [&](const EventSliceQuery &query) {
        if (!query.through_sequence && query.after_sequence == 43) {
            const auto inner = index->rebuild(work.principal.tenant_id, work.work.session_id,
                                              *fixture.events, memory_context(work.work));
            published = inner && inner.value() == 44;
        }
    };
    const auto superseded = rebuild(fixture, *index, work, source);
    MIRA_CHECK(published && !superseded && superseded.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(index->retrieve(query_for(work, "late"), {}).value().candidates.size() == 1);

    MIRA_CHECK(say("cedar after competing publisher"));
    bool erased = false;
    source.on_slice = [&](const EventSliceQuery &query) {
        if (!query.through_sequence && query.after_sequence == 44) {
            const auto owner = memory_erasure_scope_digest(work.scopes.front());
            erased = index->erase_scope(owner).has_value();
        }
    };
    auto invalidated = rebuild(fixture, *index, work, source);
    MIRA_CHECK(erased && !invalidated && invalidated.error().code == ErrorCode::InvalidState);
    MIRA_CHECK(index->retrieve(query_for(work, "cedar"), {}).value().candidates.empty());
    MIRA_CHECK(!rebuild(fixture, *index, work, source));
    return 0;
}

int byte_budget_can_publish_the_prefix_before_an_oversized_event() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "bytes.db";
    auto small = speech(fixture, work, "AssistantSpeech", "cedar small");
    options.max_source_bytes = small.payload.type.size() + small.payload.data.size();
    MIRA_CHECK(fixture.events->append(small));
    MIRA_CHECK(
        fixture.events->append(speech(fixture, work, "AssistantSpeech", std::string(256, 'x'))));
    auto index = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(index);
    const auto advance = [&] {
        return fixture.executor
            .submit_auto([&] {
                return index.value()->advance(work.principal.tenant_id, work.work.session_id,
                                              *fixture.events, memory_context(work.work));
            })
            .get();
    };
    const auto prefix = advance();
    MIRA_CHECK(prefix && prefix.value().through == 1 && prefix.value().snapshot_through == 2);
    const auto blocked = advance();
    MIRA_CHECK(!blocked && blocked.error().code == ErrorCode::ResourceExhausted);
    const auto recalled = index.value()->retrieve(query_for(work, "cedar"), {});
    MIRA_CHECK(recalled && recalled.value().candidates.size() == 1 &&
               recalled.value().candidates.front().through_event_sequence == 1);
    return 0;
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
    const auto missing = index->rebuild(work.principal.tenant_id, work.work.session_id,
                                        *fixture.events, make_control_context());
    MIRA_CHECK(!missing && missing.error().code == ErrorCode::InvalidArgument &&
               missing.error().safe_message.find("context.session") != std::string::npos);
    const auto wrong_session = index->rebuild(work.principal.tenant_id, work.work.session_id,
                                              *fixture.events, memory_context(other.work));
    MIRA_CHECK(!wrong_session && wrong_session.error().code == ErrorCode::PermissionDenied);

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
    if (const auto result =
            incremental_batches_match_one_pass_and_keep_the_previous_snapshot_on_failure();
        result != 0)
        return result;
    if (const auto result = byte_budget_can_publish_the_prefix_before_an_oversized_event();
        result != 0)
        return result;
    if (const auto result = durable_recall_rebuild_and_scope_checks(); result != 0)
        return result;
    return joint_erasure_and_recovery();
}
