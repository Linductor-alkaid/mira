// Offline explicit host: native observation -> evidence -> verified fact ->
// checkpoint -> existing SQLite consolidation/query. No model makes the fact.
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/runtime.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/task_checkpoint.hpp>
#include <mira/verified_fact.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>

namespace {
using namespace mira;
using namespace std::chrono_literals;

struct HostAuthority final {
    std::mutex gate;
    std::atomic<bool> cancelled{false};
    MiraRuntime &runtime;
    std::shared_ptr<adapters::simulator::SimulatorEnvironment> environment;
    TaskId task;
    EventId evidence;
    MemoryScope scope;
    std::uint32_t observed_width = 0;
    explicit HostAuthority(MiraRuntime &owner) : runtime(owner) {}
};

class NativeLease final : public IVerifiedFactLease {
  public:
    NativeLease(std::shared_ptr<HostAuthority> host, std::unique_lock<std::mutex> lock)
        : host_(std::move(host)), lock_(std::move(lock)) {}
    Result<void> validate(const PublishedTaskFact &fact, const EventEnvelope &evidence,
                          const OperationContext &context) override {
        const auto task = host_->runtime.task_snapshot(host_->task);
        const auto session = host_->runtime.session_snapshot(context.session);
        if (!lock_.owns_lock() || !task || !session || host_->cancelled ||
            host_->runtime.state() != RuntimeState::Running ||
            session.value().state != SessionState::Autonomous || is_terminal(task.value().state) ||
            task.value().state == TaskState::Paused || fact.claim.task != host_->task ||
            task.value().epoch != fact.claim.task_epoch ||
            fact.claim.environment_epoch != host_->environment->environment_epoch() ||
            fact.scope != host_->scope || evidence.event_id != host_->evidence ||
            context.cancelled_or_expired(Timestamp::now())) {
            Error error;
            error.code = ErrorCode::InvalidState;
            error.domain = "mira.example";
            error.safe_message = "native fact authority frame was withdrawn";
            return error;
        }
        // Independently check the proposition against the immutable native
        // observation, not against a model's Satisfied/done declaration.
        if (fact.claim.verifier != "simulator.screen-width.v1" ||
            fact.claim.key != "screen.width" ||
            fact.claim.value != std::to_string(host_->observed_width) ||
            host_->observed_width == 0) {
            Error error;
            error.code = ErrorCode::InvalidObservation;
            error.domain = "mira.example";
            error.safe_message = "native observation does not prove this fact";
            return error;
        }
        return {};
    }

  private:
    std::shared_ptr<HostAuthority> host_;
    std::unique_lock<std::mutex> lock_;
};

int failed(int line) {
    std::cerr << "verified fact host validation failed at line " << line << '\n';
    return 1;
}

int run(kairo::Executor &executor, const std::filesystem::path &root) {
    MiraRuntime runtime;
    if (!runtime.initialize())
        return failed(__LINE__);
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>(
        adapters::simulator::SimulatorSetup::single_display());
    auto session = runtime.open_session(environment);
    if (!session || !session.value().command.outcome(2s))
        return failed(__LINE__);
    auto task = runtime.submit_task(
        session.value().id, TaskSpec{.goal = "verify screen width", .user_constraints = {}});
    if (!task || !task.value().command.outcome(2s))
        return failed(__LINE__);
    OperationContext context;
    context.session = session.value().id;
    context.task = task.value().id;
    context.task_epoch = runtime.task_snapshot(context.task).value().epoch;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + 10s;
    auto host = std::make_shared<HostAuthority>(runtime);
    host->environment = environment;
    host->task = context.task;
    host->scope = MemoryScope{MemoryScopeKind::Environment, "offline-simulator", "fixture-tenant"};
    context.cancellation_requested = [host] { return host->cancelled.load(); };
    ObservationRequest request;
    request.required.screen = true;
    auto observation_future =
        executor.submit_auto([&] { return environment->observe(request, context); });
    auto observation = observation_future.get();
    if (!observation || !observation.value().screen)
        return failed(__LINE__);
    host->observed_width = observation.value().screen->value.width_pixels;
    auto events = std::make_shared<FileEventStore>(root / "events");
    if (!events->recover({}))
        return failed(__LINE__);
    const auto stream_runtime = RuntimeId::generate();
    host->evidence = EventId::generate();
    const auto evidence_data = to_json_string(JsonValue::Object{
        {"observation_id", observation.value().id.to_string()},
        {"observed_at", wall_nanos(observation.value().screen->capture.normalized_end.wall)},
        {"task_epoch", static_cast<std::int64_t>(context.task_epoch)},
        {"environment_epoch", static_cast<std::int64_t>(observation.value().environment_epoch)},
        {"width", static_cast<std::int64_t>(host->observed_width)}});
    if (!events->append({host->evidence,
                         stream_runtime,
                         context.session,
                         context.task,
                         {"ObservationRecorded", evidence_data, EventClass::State}}))
        return failed(__LINE__);
    VerifiedFactClaim claim;
    claim.publication_id = EventId::generate();
    claim.task = context.task;
    claim.task_epoch = context.task_epoch;
    claim.environment_epoch = observation.value().environment_epoch;
    claim.key = "screen.width";
    claim.value = std::to_string(host->observed_width);
    claim.evidence_event = host->evidence;
    claim.evidence_digest = digest_string(evidence_data);
    claim.verifier = "simulator.screen-width.v1";
    claim.valid_from = Timestamp::now().wall;
    claim.valid_until = claim.valid_from + 5min;
    VerifiedFactLeaseAcquirer acquire =
        [host](const PublishedTaskFact &, const EventEnvelope &,
               const OperationContext &) -> Result<std::unique_ptr<IVerifiedFactLease>> {
        std::unique_lock lock(host->gate, std::try_to_lock);
        if (!lock.owns_lock()) {
            Error error;
            error.code = ErrorCode::ResourceExhausted;
            error.domain = "mira.example";
            error.safe_message = "native fact authority is busy";
            return error;
        }
        return std::unique_ptr<IVerifiedFactLease>(
            std::make_unique<NativeLease>(host, std::move(lock)));
    };
    VerifiedFactPublisher publisher(stream_runtime, context.session, host->scope, events, acquire);
    auto publication = executor.submit_auto([&] { return publisher.publish(claim, context); });
    auto receipt = publication.get();
    if (!receipt)
        return failed(__LINE__);
    // Recreate the publisher and retry the same identity across a file reopen.
    auto reopened = std::make_shared<FileEventStore>(root / "events");
    if (!reopened->recover({}))
        return failed(__LINE__);
    VerifiedFactPublisher retry(stream_runtime, context.session, host->scope, reopened, acquire);
    auto duplicate = retry.publish(claim, context);
    if (!duplicate || duplicate.value().session_sequence != receipt.value().session_sequence)
        return failed(__LINE__);
    auto page = reopened->read({context.session, std::nullopt, 32});
    if (!page || page.value().events.size() != 2)
        return failed(__LINE__);
    auto checkpoint =
        CheckpointBuilder{}.build(std::nullopt, page.value().events, Timestamp::now());
    if (checkpoint.checkpoint.verified_facts.size() != 1 ||
        checkpoint.checkpoint.verified_facts.front().value != claim.value)
        return failed(__LINE__);
    SqliteMemoryStoreOptions options;
    options.path = root / "memory.db";
    auto memory = SqliteMemoryStore::open(executor, options);
    if (!memory)
        return failed(__LINE__);
    auto report = MemoryConsolidator{}.consolidate(*memory.value(), page.value().events,
                                                   host->scope, Timestamp::now());
    MemoryQuery query;
    query.scopes = {host->scope};
    auto found = memory.value()->query(query);
    if (!report || !found || found.value().records.size() != 1 ||
        found.value().records.front().statement != claim.key + "=" + claim.value ||
        !found.value().records.front().validity.valid_until)
        return failed(__LINE__);
    {
        std::lock_guard lock(host->gate);
        auto takeover = runtime.request_human_takeover(context.session);
        if (!takeover || !takeover.value().outcome(2s))
            return failed(__LINE__);
    }
    auto during_takeover = claim;
    during_takeover.publication_id = EventId::generate();
    if (publisher.publish(during_takeover, context))
        return failed(__LINE__);

    // Cancellation is owned by the same gate. Once it settles, late fact
    // publication cannot produce another event, even with still-valid evidence.
    {
        std::lock_guard lock(host->gate);
        auto cancelled = runtime.cancel_task(context.task);
        if (!cancelled || !cancelled.value().outcome(2s))
            return failed(__LINE__);
        host->cancelled = true;
    }
    auto late = claim;
    late.publication_id = EventId::generate();
    if (publisher.publish(late, context))
        return failed(__LINE__);
    if (!memory.value()->close())
        return failed(__LINE__);
    auto stop = runtime.request_shutdown();
    if (!stop || !stop.value().outcome(2s) || !runtime.finish_shutdown().clean)
        return failed(__LINE__);
    std::cout << "verified fact: native evidence -> checkpoint -> scoped memory; late publication "
                 "rejected\n";
    return 0;
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-verified-fact-host-" + mira::SessionId::generate().to_string());
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    if (!executor.initialize(config))
        return 1;
    int result = 1;
    try {
        result = run(executor, root);
    } catch (...) {
        std::cerr << "verified fact host failed\n";
    }
    if (executor.shutdown(true) != kairo::ShutdownResult::Completed)
        result = 1;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}
