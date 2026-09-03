#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/artifact_store.hpp>
#include <mira/replay.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/stateful_replay.hpp>

#include <chrono>
#include <filesystem>
#include <random>

#include <executor/executor.hpp>

namespace {

using namespace mira;
using mira::testing::TaskEventLog;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-replay-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

[[nodiscard]] EnvironmentCapabilities no_side_effect_capabilities() {
    EnvironmentCapabilities capabilities;
    // Analysis replay never claims screen capture, input or tool execution.
    return capabilities;
}

struct Fixture final {
    Fixture() {
        executor_.initialize(executor::ExecutorConfig{});
        checkpoint_options.path = temp_dir() / "replay-checkpoints.db";
        memory_options.path = temp_dir() / "replay-memory.db";
        auto checkpoints = SqliteCheckpointStore::open(executor_, checkpoint_options);
        checkpoint_store = std::move(checkpoints).value();
        auto memory = SqliteMemoryStore::open(executor_, memory_options, &artifacts);
        memory_store = std::move(memory).value();
    }
    ~Fixture() {
        (void)memory_store->close();
        (void)checkpoint_store->close();
        (void)executor_.shutdown(true);
    }
    executor::Executor executor_;
    SqliteStoreOptions checkpoint_options;
    SqliteMemoryStoreOptions memory_options;
    MemoryEventStore events;
    MemoryArtifactStore artifacts;
    std::unique_ptr<SqliteCheckpointStore> checkpoint_store;
    std::unique_ptr<SqliteMemoryStore> memory_store;
};

[[nodiscard]] MemoryScope scope_of(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    return scope;
}

[[nodiscard]] MemoryMutation add_mutation(const MemoryRecord &record) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = record.scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return mutation;
}

int bitemporal_as_of_and_checkpoint_view() {
    Fixture fixture;
    const auto runtime = RuntimeId::generate();
    const auto session = SessionId::generate();
    const auto task = TaskId::generate();
    const auto scope = scope_of("device-42");

    TaskEventLog log(runtime, session, task);
    log.add("TaskGoalSet", JsonValue::Object{{"statement", "inspect the lab"}});
    log.add("TaskConstraintAdded",
            JsonValue::Object{{"key", "safety"}, {"requirement", "wear goggles"}, {"safety", true}});
    const auto action = ActionId::generate();
    log.add_pipe("ActionDispatchStarted",
                 action.to_string() + "|" + task.to_string() + "|1|1|tap|ui|node-1|screen",
                 EventClass::Critical);
    log.add_pipe("ActionExecutionUncertain",
                 action.to_string() + "|" + task.to_string() + "|1|1|tap|ui|node-1|screen|lost",
                 EventClass::Critical);
    log.append_to(fixture.events);

    CheckpointBuilder builder;
    auto built = builder.build(std::nullopt, log.envelopes(), Timestamp::now());
    MIRA_CHECK(fixture.checkpoint_store->put(built.checkpoint).has_value());

    // Memory: one record with an evidence artifact.
    MemoryRecord with_evidence;
    with_evidence.id = MemoryId::generate();
    with_evidence.scope = scope;
    with_evidence.statement = "microscope calibration due friday";
    with_evidence.recorded_at = std::chrono::system_clock::now();
    with_evidence.validity.valid_from = with_evidence.recorded_at;
    with_evidence.provenance = {EventId::generate()};
    with_evidence.verification = MemoryVerification::Verified;
    with_evidence.confidence = 0.9F;
    ArtifactWriteSpec spec;
    spec.media_type = "text/plain";
    auto writer_result = fixture.artifacts.begin(spec);
    MIRA_CHECK(writer_result.has_value());
    MIRA_CHECK(writer_result.value().write("calibration note", 17).has_value());
    auto committed = fixture.artifacts.commit(writer_result.value());
    MIRA_CHECK(committed.has_value());
    with_evidence.evidence = ArtifactRef{committed.value().id, committed.value().digest,
                                         committed.value().byte_size, committed.value().media_type,
                                         committed.value().sensitivity};
    MIRA_CHECK(fixture.memory_store->apply(add_mutation(with_evidence)).has_value());

    AnalysisReplay replay(fixture.events, fixture.checkpoint_store.get(),
                          fixture.memory_store.get(), &fixture.artifacts);
    auto report = replay.inspect(task, session, scope);
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().checkpoint_present);
    MIRA_CHECK(report.value().checkpoint->goal_statement == "inspect the lab");
    MIRA_CHECK(report.value().checkpoint->constraints.size() == 1);
    MIRA_CHECK(!report.value().memory.records.empty());
    MIRA_CHECK(report.value().missing_artifacts.empty());
    MIRA_CHECK(!report.value().memory_degraded);

    // As-of before anything existed: nothing, and the report says why.
    ReplayAsOf prehistory;
    prehistory.through_event_sequence = 0;
    prehistory.recorded_at = std::chrono::system_clock::now() - std::chrono::hours(48);
    auto ancient = replay.inspect(task, session, scope, prehistory);
    MIRA_CHECK(ancient.has_value());
    MIRA_CHECK(!ancient.value().checkpoint_present);
    MIRA_CHECK(ancient.value().memory.records.empty());

    // Replay never re-executes actions: the offline environment refuses.
    OfflineReplayEnvironment offline({}, {}, no_side_effect_capabilities());
    OperationContext context;
    // With no recorded receipts every execution request is refused; an empty
    // sequence is rejected before that. Either way nothing is dispatched.
    auto refused = offline.execute(InputSequence{}, context);
    MIRA_CHECK(!refused.has_value());
    MIRA_CHECK(refused.error().code == ErrorCode::InvalidArgument);
    return 0;
}

int deleted_artifact_degrades_explicitly() {
    Fixture fixture;
    const auto task = TaskId::generate();
    const auto scope = scope_of("lab-9");

    MemoryRecord referenced;
    referenced.id = MemoryId::generate();
    referenced.scope = scope;
    referenced.statement = "log book is on shelf b";
    referenced.recorded_at = std::chrono::system_clock::now();
    referenced.validity.valid_from = referenced.recorded_at;
    referenced.provenance = {EventId::generate()};
    referenced.verification = MemoryVerification::Verified;
    referenced.confidence = 0.8F;
    ArtifactWriteSpec spec;
    spec.media_type = "text/plain";
    auto writer_result = fixture.artifacts.begin(spec);
    MIRA_CHECK(writer_result.has_value());
    MIRA_CHECK(writer_result.value().write("log book", 8).has_value());
    auto committed = fixture.artifacts.commit(writer_result.value());
    MIRA_CHECK(committed.has_value());
    referenced.evidence = ArtifactRef{committed.value().id, committed.value().digest,
                                      committed.value().byte_size, committed.value().media_type,
                                      committed.value().sensitivity};
    MIRA_CHECK(fixture.memory_store->apply(add_mutation(referenced)).has_value());

    // Erase the artifact out from under the memory reference: the replay
    // must degrade explicitly instead of pretending the evidence exists.
    ArtifactErasureRequest artifact_erase;
    artifact_erase.id = committed.value().id;
    artifact_erase.reason = "retention";
    MIRA_CHECK(fixture.artifacts.erase(artifact_erase).has_value());

    AnalysisReplay replay(fixture.events, fixture.checkpoint_store.get(),
                          fixture.memory_store.get(), &fixture.artifacts);
    auto report = replay.inspect(task, SessionId::generate(), scope);
    MIRA_CHECK(report.has_value());
    MIRA_CHECK(report.value().missing_artifacts.size() == 1);
    MIRA_CHECK(!report.value().note.empty());

    // Unbound stores degrade with notes, never with fabricated content.
    AnalysisReplay bare(fixture.events, nullptr, nullptr, nullptr);
    auto minimal = bare.inspect(task, SessionId::generate(), scope);
    MIRA_CHECK(minimal.has_value());
    MIRA_CHECK(!minimal.value().checkpoint_present);
    MIRA_CHECK(minimal.value().memory.records.empty());
    MIRA_CHECK(minimal.value().note.find("not bound") != std::string::npos);
    return 0;
}

int replay_has_no_capability_side_channels() {
    // The analysis report structurally exposes no Network/Tool/Input
    // capability; this pins the invariant the design demands.
    MIRA_CHECK(AnalysisReplayReport::capability_mask == 0);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += bitemporal_as_of_and_checkpoint_view();
    failures += deleted_artifact_degrades_explicitly();
    failures += replay_has_no_capability_side_channels();
    std::filesystem::remove_all(temp_dir());
    return failures == 0 ? 0 : 1;
}
