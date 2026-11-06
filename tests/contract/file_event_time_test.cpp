#include "support/test.hpp"
#include <mira/json.hpp>
#include <mira/memory_consolidation.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace mira;
struct Fixture final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("mira-event-time-" + SessionId::generate().to_string());
    RuntimeId runtime = RuntimeId::generate();
    SessionId session = SessionId::generate();
    TaskId task = TaskId::generate();
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    AppendRequest request(std::string data = "fixture") const {
        return {EventId::generate(),
                runtime,
                session,
                task,
                {"Fixture", std::move(data), EventClass::State}};
    }
    std::string bytes() const {
        std::ifstream file(root / "events.log", std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
    void write(const std::string &data) {
        std::ofstream file(root / "events.log", std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
};

std::string legacy_line(const AppendRequest &request) {
    MemoryEventStore memory;
    auto receipt = memory.append(request).value();
    const auto event = memory.find(request.event_id).value();
    std::ostringstream line;
    line << receipt.session_sequence << ' ' << request.event_id.to_string() << ' '
         << request.runtime_id.to_string() << ' ' << request.session_id.to_string() << ' '
         << (request.task_id ? request.task_id->to_string() : "-") << ' ' << request.cause << ' '
         << static_cast<unsigned>(request.payload.classification) << ' '
         << std::quoted(request.payload.type) << ' ' << std::quoted(request.payload.data) << ' '
         << event.integrity.to_string();
    return line.str();
}

int stable_wall_and_idempotency() {
    Fixture f;
    auto request = f.request(std::string{"line\nwith\rcontrol\0bytes", 23});
    request.cause = std::numeric_limits<std::uint64_t>::max();
    WallTimePoint original;
    {
        FileEventStore events(f.root);
        MIRA_CHECK(events.append(request));
        const auto page = events.read({f.session, {}, 8});
        MIRA_CHECK(page && page.value().events.size() == 1);
        const auto &event = page.value().events.front();
        MIRA_CHECK(event.timestamp_origin == EventTimestampOrigin::AppendProcess);
        MIRA_CHECK(event.timestamp.monotonic.time_since_epoch().count() != 0);
        original = event.timestamp.wall;
    }
    auto stable_bytes = f.bytes();
    MIRA_CHECK(stable_bytes.starts_with("MIRA_EVENT_LOG 2\nMIRA_EVENT_RECORDS\nE2 "));
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        FileEventStore events(f.root);
        MIRA_CHECK(events.recover({}).value().status == RecoveryStatus::Clean);
        const auto page = events.read({f.session, {}, 8});
        MIRA_CHECK(page && page.value().events.front().timestamp.wall == original);
        const auto &event = page.value().events.front();
        MIRA_CHECK(event.timestamp_origin == EventTimestampOrigin::PersistedWall);
        MIRA_CHECK(event.timestamp.monotonic == std::chrono::steady_clock::time_point{});
        MIRA_CHECK(event.payload.data == request.payload.data &&
                   event.cause_control_sequence == request.cause);
        MIRA_CHECK(events.append(request).value().session_sequence == 1);
        auto changed = request;
        changed.payload.data += "changed";
        MIRA_CHECK(!events.append(changed));
        MIRA_CHECK(f.bytes() == stable_bytes);
    }
    {
        // Complete record without final newline: preserve it and separate the next append.
        f.write(stable_bytes.substr(0, stable_bytes.size() - 1));
        FileEventStore events(f.root);
        MIRA_CHECK(events.recover({}).value().status == RecoveryStatus::Clean);
        auto other = f.request();
        other.session_id = SessionId::generate();
        const auto receipt = events.append(other);
        MIRA_CHECK(receipt && receipt.value().session_sequence == 1 &&
                   receipt.value().task_sequence == 2);
        auto no_task = f.request();
        no_task.task_id.reset();
        MIRA_CHECK(events.append(no_task));
    }
    FileEventStore reopened(f.root);
    MIRA_CHECK(reopened.recover({}).value().events_recovered == 3);
    auto original_page = reopened.read({f.session, {}, 8});
    MIRA_CHECK(original_page && original_page.value().events.front().timestamp.wall == original);
    MIRA_CHECK(!original_page.value().events.back().task_sequence);
    return 0;
}

int legacy_transition_preserves_unknown_time() {
    Fixture f;
    auto old = f.request();
    const auto prefix = legacy_line(old); // Deliberately no final newline.
    f.write(prefix);
    WallTimePoint new_wall;
    {
        FileEventStore events(f.root);
        MIRA_CHECK(events.recover({}).value().events_recovered == 1);
        auto page = events.read({f.session, {}, 8});
        MIRA_CHECK(page && page.value().events.front().timestamp_origin ==
                               EventTimestampOrigin::UnavailableLegacy);
        MIRA_CHECK(wall_nanos(page.value().events.front().timestamp) == 0);
        MIRA_CHECK(events.append(old).value().session_sequence == 1 && f.bytes() == prefix);
        MIRA_CHECK(events.append(f.request()));
        page = events.read({f.session, {}, 8});
        new_wall = page.value().events.back().timestamp.wall;
    }
    MIRA_CHECK(f.bytes().starts_with(prefix + "\nMIRA_EVENT_LOG 2\nMIRA_EVENT_RECORDS\n"));
    FileEventStore events(f.root);
    MIRA_CHECK(events.recover({}).value().events_recovered == 2);
    const auto page = events.read({f.session, {}, 8});
    MIRA_CHECK(page && page.value().events.size() == 2);
    MIRA_CHECK(page.value().events.front().timestamp_origin ==
               EventTimestampOrigin::UnavailableLegacy);
    MIRA_CHECK(page.value().events.back().timestamp_origin == EventTimestampOrigin::PersistedWall);
    MIRA_CHECK(page.value().events.back().timestamp.wall == new_wall);
    return 0;
}

int opaque_payload_roundtrips_beyond_model_json_defaults() {
    Fixture f;
    auto request = f.request(std::string(JsonLimits{}.max_string_bytes + 1, '\0'));
    {
        FileEventStore events(f.root);
        MIRA_CHECK(events.append(request));
    }
    MIRA_CHECK(f.bytes().size() > JsonLimits{}.max_document_bytes);
    FileEventStore recovered(f.root);
    MIRA_CHECK(recovered.recover({}).value().status == RecoveryStatus::Clean);
    auto page = recovered.read({f.session, {}, 8});
    MIRA_CHECK(page && page.value().events.front().payload.data == request.payload.data);
    return 0;
}

std::string replace_record(const std::string &file, const std::function<void(JsonValue &)> &change,
                           bool resign) {
    const auto start = file.find("E2 ");
    auto body = parse_json(file.substr(start + 3)).value();
    change(body);
    if (resign) {
        auto fields = *body.as_object();
        std::erase_if(fields, [](const auto &entry) { return entry.first == "record_digest"; });
        body.set("record_digest",
                 digest_string(canonical_json_string(JsonValue{std::move(fields)})).to_string());
    }
    return file.substr(0, start) + "E2 " + canonical_json_string(body) + "\n";
}

int corrupt_and_future_records_do_not_repair() {
    struct Change final {
        std::function<void(JsonValue &)> apply;
        RecoveryStatus expected;
        bool resign = true;
    };
    const Change changes[] = {
        {[](auto &j) { j.set("wall_ns", *j.find("wall_ns")->as_integer() + 1); },
         RecoveryStatus::DataLoss, false},
        {[](auto &j) { j.set("wall_ns", "invalid"); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("classification", 256); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("classification", -1); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("session_sequence", "2"); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("task_sequence", "2"); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("cause", "-1"); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("schema", "mira.file-event.v3"); },
         RecoveryStatus::ReadOnlyDiagnostic},
        {[](auto &j) { j.set("future_member", true); }, RecoveryStatus::ReadOnlyDiagnostic},
        {[](auto &j) { j.set("task_id", JsonValue{}); }, RecoveryStatus::DataLoss},
        {[](auto &j) { j.set("integrity", std::string(64, '0')); }, RecoveryStatus::DataLoss},
    };
    for (const auto &change : changes) {
        Fixture f;
        {
            FileEventStore events(f.root);
            MIRA_CHECK(events.append(f.request()));
        }
        const auto bad = replace_record(f.bytes(), change.apply, change.resign);
        f.write(bad);
        FileEventStore events(f.root);
        auto recovered = events.recover({});
        MIRA_CHECK(recovered && recovered.value().status == change.expected && events.read_only());
        MIRA_CHECK(!events.append(f.request()) && !events.read({f.session, {}, 8}));
        MIRA_CHECK(f.bytes() == bad);
    }
    return 0;
}

int exact_wire_nanoseconds_and_invalid_appends() {
    for (std::int64_t nanos : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{1},
                               std::int64_t{1'777'777'777'123'456'789}}) {
        Fixture f;
        {
            FileEventStore events(f.root);
            MIRA_CHECK(events.append(f.request()));
        }
        f.write(replace_record(f.bytes(), [nanos](auto &j) { j.set("wall_ns", nanos); }, true));
        FileEventStore events(f.root);
        MIRA_CHECK(events.recover({}).value().status == RecoveryStatus::Clean);
        auto page = events.read({f.session, {}, 8});
        MIRA_CHECK(page && wall_nanos(page.value().events.front().timestamp) == nanos);
        MIRA_CHECK(page.value().events.front().timestamp_origin ==
                   EventTimestampOrigin::PersistedWall);
    }
    Fixture f;
    FileEventStore events(f.root);
    auto invalid = f.request();
    invalid.payload.classification = static_cast<EventClass>(255);
    MIRA_CHECK(!events.append(invalid));
    invalid.payload.classification = EventClass::State;
    invalid.task_id = TaskId{};
    MIRA_CHECK(!events.append(invalid));
    MIRA_CHECK(!std::filesystem::exists(f.root / "events.log"));
    return 0;
}

int tail_and_format_transitions() {
    for (const std::string &tail :
         {std::string{"E2 {"}, std::string{"truncated"}, std::string{"E"}, std::string{"E2"}}) {
        for (bool repair : {false, true}) {
            Fixture f;
            {
                FileEventStore events(f.root);
                MIRA_CHECK(events.append(f.request()));
            }
            const auto good = f.bytes();
            f.write(good + tail);
            FileEventStore events(f.root);
            const auto report = events.recover({repair});
            MIRA_CHECK(report &&
                       report.value().status == (repair ? RecoveryStatus::TailTruncated
                                                        : RecoveryStatus::ReadOnlyDiagnostic));
            MIRA_CHECK(f.bytes() == (repair ? good : good + tail));
            if (repair) {
                MIRA_CHECK(report.value().bytes_discarded == tail.size());
                MIRA_CHECK(events.append(f.request()));
            }
        }
    }
    for (const auto &header : {std::string{"MIRA_EVENT_LOG"}, std::string{"MIRA_EVENT_LOG 2\n"},
                               std::string{"MIRA_EVENT_LOG 2\nMIRA_EVENT_REC"}}) {
        Fixture f;
        f.write(header);
        FileEventStore events(f.root);
        auto recovered = events.recover({});
        MIRA_CHECK(recovered && recovered.value().status == RecoveryStatus::TailTruncated);
        MIRA_CHECK(f.bytes().empty() && events.append(f.request()));
    }
    for (unsigned kind = 0; kind < 8; ++kind) {
        Fixture f;
        auto request = f.request();
        {
            FileEventStore events(f.root);
            MIRA_CHECK(events.append(request));
        }
        auto bad = f.bytes();
        if (kind == 0)
            bad.replace(0, std::string("MIRA_EVENT_LOG 2").size(), "MIRA_EVENT_LOG 3");
        if (kind == 1)
            bad += "MIRA_EVENT_LOG 2\nMIRA_EVENT_RECORDS\n";
        if (kind == 2)
            bad += legacy_line(request) + "\n"; // No downgrade after v2.
        if (kind == 3)
            bad.replace(bad.find("E2 "), 2, "E3");
        if (kind == 4)
            bad.insert(bad.find("E2 "), "E2 {\n"); // Incomplete middle, not a repairable tail.
        if (kind == 5)
            bad += "E2 {\n"; // Newline-terminated corrupt JSON is never silently truncated.
        if (kind == 6)
            bad.insert(bad.find("E2 ") + 4, "\"schema\":\"mira.file-event.v2\",");
        if (kind == 7)
            bad.erase(0, bad.find("E2 ")); // A v2 record without its guard is not a legacy tail.
        f.write(bad);
        FileEventStore events(f.root);
        auto recovered = events.recover({});
        MIRA_CHECK(recovered && events.read_only() && f.bytes() == bad);
        MIRA_CHECK(!events.append(f.request()));
    }
    return 0;
}

int missing_legacy_time_never_becomes_now() {
    class Memory final : public IMemory {
      public:
        mutable unsigned queries = 0;
        unsigned writes = 0;
        Result<MemoryQueryResult> query(const MemoryQuery &) const override {
            ++queries;
            return MemoryQueryResult{};
        }
        Result<std::optional<MemoryRecord>> get(MemoryId, const MemoryScope &) const override {
            return std::optional<MemoryRecord>{};
        }
        Result<MemoryMutationResult> apply(const MemoryMutation &) override {
            ++writes;
            return MemoryMutationResult{};
        }
        Result<MemoryCompactionResult> compact(const MemoryScope &) override { return Error{}; }
        Result<ErasureResult> erase(const ErasureRequest &) override { return Error{}; }
    } memory;
    MemoryScope scope{MemoryScopeKind::Environment, "fixture", {}};
    for (const auto &payload :
         {EventPayload{"TaskFactVerified", R"({"key":"screen.width","value":"240"})"},
          EventPayload{"LoopSettled", R"({"outcome":"Completed"})"}}) {
        Fixture f;
        auto request = f.request();
        request.payload = payload;
        f.write(legacy_line(request) + "\n");
        FileEventStore events(f.root);
        const auto page = events.read({f.session, {}, 8});
        MIRA_CHECK(page && page.value().events.front().timestamp_origin ==
                               EventTimestampOrigin::UnavailableLegacy);
        auto result =
            MemoryConsolidator{}.consolidate(memory, page.value().events, scope, Timestamp::now());
        MIRA_CHECK(!result && result.error().domain == "mira.memory");
        MIRA_CHECK(memory.queries == 0 && memory.writes == 0);
    }
    return 0;
}

int duplicate_identity_compares_fields_not_only_legacy_digest() {
    Fixture f;
    auto original = f.request("c");
    original.payload.type = "a|b";
    auto different = original;
    different.payload.type = "a";
    different.payload.data = "b|c"; // Same delimiter-based digest input, different request.
    MemoryEventStore memory;
    MIRA_CHECK(memory.append(original));
    auto rejected = memory.append(different);
    MIRA_CHECK(!rejected && rejected.error().code == ErrorCode::DataLoss);
    {
        FileEventStore events(f.root);
        MIRA_CHECK(events.append(original));
        const auto bytes = f.bytes();
        rejected = events.append(different);
        MIRA_CHECK(!rejected && rejected.error().code == ErrorCode::DataLoss && f.bytes() == bytes);
    }
    FileEventStore recovered(f.root);
    MIRA_CHECK(recovered.recover({}));
    MIRA_CHECK(!recovered.append(different) && recovered.append(original));
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    // Emits one current-format file for an independently compiled v1 reader probe.
    if (argc == 3 &&
        (std::string_view(argv[1]) == "--emit-v2" || std::string_view(argv[1]) == "--emit-mixed")) {
        if (std::filesystem::exists(argv[2]))
            return 2;
        AppendRequest request{EventId::generate(),
                              RuntimeId::generate(),
                              SessionId::generate(),
                              {},
                              {"Fixture", "v2"}};
        if (std::string_view(argv[1]) == "--emit-mixed") {
            std::filesystem::create_directories(argv[2]);
            std::ofstream legacy(std::filesystem::path(argv[2]) / "events.log");
            legacy << legacy_line(request) << '\n';
            request.event_id = EventId::generate();
        }
        FileEventStore events(argv[2]);
        return events.append(request) ? 0 : 1;
    }
    MIRA_CHECK(stable_wall_and_idempotency() == 0);
    MIRA_CHECK(legacy_transition_preserves_unknown_time() == 0);
    MIRA_CHECK(opaque_payload_roundtrips_beyond_model_json_defaults() == 0);
    MIRA_CHECK(corrupt_and_future_records_do_not_repair() == 0);
    MIRA_CHECK(exact_wire_nanoseconds_and_invalid_appends() == 0);
    MIRA_CHECK(tail_and_format_transitions() == 0);
    MIRA_CHECK(missing_legacy_time_never_becomes_now() == 0);
    MIRA_CHECK(duplicate_identity_compares_fields_not_only_legacy_digest() == 0);
    return 0;
}
