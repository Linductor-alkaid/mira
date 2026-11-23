#include "support/m36_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int event_slice_reopens_and_preserves_the_scan_window() {
    MemoryAgentFixture fixture;
    auto &store = *fixture.events;
    const auto first = fixture.binding();
    auto second = fixture.binding();
    second.work.session_id = first.work.session_id;
    const auto session = first.work.session_id;

    const auto append = [&](SessionId owner, std::optional<TaskId> task, std::string type) {
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = fixture.runtime;
        request.session_id = owner;
        request.task_id = task;
        request.payload = {std::move(type), "fixture", EventClass::State};
        return request;
    };
    auto a = append(session, first.work.task_id, "TaskChanged");
    auto foreign = append(second.work.session_id, second.work.task_id, "TaskChanged");
    auto b = append(session, first.work.task_id, "TaskChecked");
    auto c = append(session, first.work.task_id, "TaskChanged");
    auto orphan = append(session, std::nullopt, "TaskChanged");
    MIRA_CHECK(store.append(a).value().session_sequence == 1);
    MIRA_CHECK(store.append(foreign).value().session_sequence == 2);
    MIRA_CHECK(store.append(b).value().session_sequence == 3);
    MIRA_CHECK(store.append(c).value().session_sequence == 4);
    MIRA_CHECK(store.append(orphan).value().session_sequence == 5);
    MIRA_CHECK(store.append(a).value().session_sequence == 1);

    EventSliceQuery query;
    query.session_id = session;
    query.task_id = first.work.task_id;
    query.event_type = "TaskChanged";
    query.limit = 1;
    const auto initial = store.read_slice(query);
    MIRA_CHECK(initial && initial.value().events.size() == 1);
    MIRA_CHECK(initial.value().events.front().event_id == a.event_id);
    MIRA_CHECK(initial.value().snapshot_through == 5 && initial.value().scanned_through == 1 &&
               initial.value().has_more);

    const auto later = append(session, first.work.task_id, "TaskChanged");
    MIRA_CHECK(store.append(later).value().session_sequence == 6);
    const auto different_session = append(SessionId::generate(), first.work.task_id, "TaskChanged");
    MIRA_CHECK(store.append(different_session).value().task_sequence == 5);
    const auto stable_bytes = store.log_bytes().value();
    // Read from the recovered projection of the same durable prefix.
    fixture.events.reset();
    FileEventStore reopened(fixture.root / "events");
    query.after_sequence = initial.value().scanned_through;
    query.through_sequence = initial.value().snapshot_through;
    const auto tail = reopened.read_slice(query);
    MIRA_CHECK(tail && tail.value().events.size() == 1 &&
               tail.value().events.front().event_id == c.event_id);
    MIRA_CHECK(tail.value().events.front().timestamp_origin == EventTimestampOrigin::PersistedWall);
    MIRA_CHECK(tail.value().snapshot_through == 5 && tail.value().scanned_through == 5 &&
               !tail.value().has_more);
    MIRA_CHECK(reopened.log_bytes().value() == stable_bytes);

    // An empty page can still advance a filtered scan. Timestamp filters
    // never reorder session sequences.
    query = {};
    query.session_id = session;
    query.task_id = first.work.task_id;
    query.excluded_type_prefix = "TaskChanged";
    query.scan_limit = 1;
    auto empty = reopened.read_slice(query);
    MIRA_CHECK(empty && empty.value().events.empty() && empty.value().has_more &&
               empty.value().scanned_through == 1);
    query.after_sequence = empty.value().scanned_through;
    const auto next = reopened.read_slice(query);
    MIRA_CHECK(next && next.value().events.size() == 1 &&
               next.value().events.front().event_id == b.event_id);
    const auto wall = next.value().events.front().timestamp.wall;
    query.scan_limit = 16;
    query.recorded_from = wall;
    query.recorded_before = wall + 1ns;
    const auto timed = reopened.read_slice(query);
    MIRA_CHECK(timed && timed.value().events.size() == 1 &&
               timed.value().events.front().event_id == b.event_id);
    query.recorded_before = wall;
    MIRA_CHECK(!reopened.read_slice(query));
    query.recorded_before.reset();
    query.recorded_from.reset();
    query.through_sequence = 7;
    MIRA_CHECK(!reopened.read_slice(query));
    query.through_sequence.reset();
    query.byte_limit = 1;
    const auto oversized = reopened.read_slice(query);
    MIRA_CHECK(!oversized && oversized.error().code == ErrorCode::ResourceExhausted);
    query = {};
    query.session_id = session;
    query.byte_limit = std::string_view("TaskChanged").size() + std::string_view("fixture").size();
    const auto bytes_bound = reopened.read_slice(query);
    MIRA_CHECK(bytes_bound && bytes_bound.value().events.size() == 1 &&
               bytes_bound.value().scanned_through == 1 && bytes_bound.value().has_more);
    return 0;
}

int task_record_skips_other_work_and_audit_events() {
    MemoryAgentFixture fixture;
    MemoryAgentConfig config;
    config.max_events = 1;
    fixture.start({}, config);
    const auto task = fixture.binding();
    MIRA_CHECK(fixture.service->bind(task));
    auto other = fixture.binding();
    other.work.session_id = task.work.session_id;
    for (int i = 0; i < 8; ++i)
        MIRA_CHECK(fixture.fact(other, "untargeted", std::to_string(i)));
    MIRA_CHECK(fixture.fact(task, "target-a", "a"));
    MIRA_CHECK(fixture.fact(other, "untargeted", "last"));
    MIRA_CHECK(fixture.fact(task, "target-b", "b"));
    MIRA_CHECK(fixture.service->retire(task.work.task_id));
    MemoryAgentRequest record;
    record.mode = MemoryAgentMode::Record;
    record.task = task.work.task_id;
    record.write_scope = task.scopes.front();
    const auto run = [&]() -> Result<MemoryAgentResult> {
        auto handle = fixture.service->submit(record, memory_context(task.work));
        if (!handle)
            return handle.error();
        if (handle.value().result.wait_for(3s) != std::future_status::ready) {
            Error timed_out;
            timed_out.code = ErrorCode::DeadlineExceeded;
            return timed_out;
        }
        return handle.value().result.get();
    };
    const auto first = run();
    MIRA_CHECK(first && first.value().through_event_sequence == 9 && first.value().more_pending &&
               first.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(fixture.fact(task, "target-c", "c"));
    const auto second = run();
    MIRA_CHECK(second && second.value().through_event_sequence == 11 &&
               second.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    const auto drained = run();
    MIRA_CHECK(drained && drained.value().through_event_sequence > 11 &&
               !drained.value().more_pending && drained.value().consolidation.entries.empty());
    MemoryQuery query;
    query.scopes = task.scopes;
    const auto memories = fixture.memory->query(query);
    MIRA_CHECK(memories && memories.value().records.size() == 2);
    const auto later = run();
    MIRA_CHECK(later && later.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
    MIRA_CHECK(fixture.memory->query(query).value().records.size() == 3);
    const auto erased = fixture.service->erase_scope(task.scopes.front());
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete);
    MIRA_CHECK(fixture.service->close());
    fixture.service.reset();
    fixture.events = std::make_shared<FileEventStore>(fixture.root / "events");
    fixture.start({}, config);
    MIRA_CHECK(!fixture.service->bind(task));
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    return 0;
}

class LegacyEventAdapter final : public IEventStore {
  public:
    explicit LegacyEventAdapter(FileEventStore &source, bool fail) : source_(source), fail_(fail) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        return source_.append(request);
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return source_.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return source_.read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return source_.recover(options);
    }
    Result<void> flush(Durability required) override { return source_.flush(required); }
    Result<EventSlicePage> read_slice(const EventSliceQuery &query) const override {
        if (!fail_)
            return IEventStore::read_slice(query);
        Error failure;
        failure.code = ErrorCode::Unavailable;
        return failure;
    }

  private:
    FileEventStore &source_;
    bool fail_;
};

int old_adapters_and_slice_errors() {
    for (const bool fail : {false, true}) {
        MemoryAgentFixture fixture;
        auto adapter = std::make_shared<LegacyEventAdapter>(*fixture.events, fail);
        MemoryAgentService service(fixture.executor, fixture.memory, adapter, fixture.runtime);
        MIRA_CHECK(service.initialize());
        const auto work = fixture.binding();
        MIRA_CHECK(service.bind(work) && fixture.fact(work, "target", "value"));
        MemoryAgentRequest request;
        request.mode = MemoryAgentMode::Record;
        request.task = work.work.task_id;
        request.write_scope = work.scopes.front();
        auto handle = service.submit(request, memory_context(work.work));
        MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
        const auto &settled = handle.value().result.get();
        if (fail)
            MIRA_CHECK(!settled && settled.error().code == ErrorCode::Unavailable);
        else
            MIRA_CHECK(settled &&
                       settled.value().consolidation.count_of(CandidateDisposition::Applied) == 1);
        MIRA_CHECK(service.close());
    }
    return 0;
}
} // namespace

int main() {
    for (const auto test :
         {event_slice_reopens_and_preserves_the_scan_window,
          task_record_skips_other_work_and_audit_events, old_adapters_and_slice_errors}) {
        if (const int code = test(); code != 0)
            return code;
    }
    return 0;
}
