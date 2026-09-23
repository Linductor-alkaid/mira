// Working Context host integration example (M25, DEC-045): the reference
// host for the AgentLoop snapshot supply seam. Runnable offline, no model —
// the model side is a scripted provider. The Loop itself stays context-blind
// automation-free: every Working Context lifecycle operation below is an
// explicit host call (plan §4.3), the seam only reads committed snapshots.
//
// Host operation sequence demonstrated (M25 §4.3, all explicit):
//   W3  trigger  — `WorkingContextAutoCurator::on_signal` between loop steps,
//                  the commit landing before the loop's next request; the
//                  task boundary awaits the `flush` future before the
//                  terminal flag (M22 §4.2);
//   W4  promote  — `submit<WorkingContextPromotionReport>(..., Deferrable, ...)`
//                  through the generic supervisor route, future consumed;
//   W5  fork     — child session baseline from the committed parent snapshot,
//                  the child Loop consuming its own chain through its own
//                  seam, delta projection and mechanical merge committed at a
//                  strictly advancing parent watermark through the existing
//                  commit pipeline (same-watermark conflict stays fail-closed).
// The seam consumption is asserted on the assembled requests: the first
// request is context-blind, the second carries the committed snapshot, the
// third carries the merged child finding.

#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/agent_loop.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/event_store.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_provider.hpp>
#include <mira/runtime.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::adapters::simulator;

// ---------------------------------------------------------------------------
// Scripted model side: responses in order, with an optional per-response hook
// so the host can run its orchestration while the loop waits inside infer.
// ---------------------------------------------------------------------------

class ScriptedProvider final : public IModelProvider {
  public:
    ScriptedProvider(std::shared_ptr<const ModelProfile> profile, std::vector<ModelResponse> script)
        : profile_(std::move(profile)), script_(std::move(script)) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &) override {
        if (context.cancelled()) {
            Error cancelled;
            cancelled.code = ErrorCode::Cancelled;
            cancelled.domain = "mira.example.wc";
            cancelled.safe_message = "scripted provider observed cancellation";
            return cancelled;
        }
        std::function<void()> hook;
        ModelResponse response;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
            const auto hooked = hooks_.find(cursor_);
            if (hooked != hooks_.end()) {
                hook = hooked->second;
            }
            if (cursor_ >= script_.size()) {
                Error exhausted;
                exhausted.code = ErrorCode::ResourceExhausted;
                exhausted.domain = "mira.example.wc";
                exhausted.safe_message = "script is exhausted";
                return exhausted;
            }
            response = script_[cursor_++];
        }
        if (hook) {
            hook();
        }
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        return response;
    }

    void on_after_response(std::size_t index, std::function<void()> hook) {
        const std::lock_guard lock(mutex_);
        hooks_[index] = std::move(hook);
    }

    [[nodiscard]] std::vector<ModelRequest> requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

  private:
    std::shared_ptr<const ModelProfile> profile_;
    std::vector<ModelResponse> script_;
    mutable std::mutex mutex_;
    std::map<std::size_t, std::function<void()>> hooks_;
    std::vector<ModelRequest> requests_;
    std::size_t cursor_ = 0;
};

// Handshake between a provider hook (running inside the loop's infer on an
// Executor worker) and the host driving the orchestration from the main
// thread: request ordering stays deterministic without sleeps.
class HostGate final {
  public:
    void enter_and_wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        entered_cv_.notify_all();
        done_cv_.wait(lock, [this] { return done_; });
    }

    void wait_entered() {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        done_cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable entered_cv_;
    std::condition_variable done_cv_;
    bool entered_ = false;
    bool done_ = false;
};

// Host-side deterministic curator: projects the checkpoint the host reports,
// with the snapshot identity taken from the checkpoint's own stamping. Real
// hosts would route this through `ProviderContextCurator` (DEC-036) — the
// automation surface (`WorkingContextAutoCurator`) is identical either way.
class DeterministicCurator final : public IContextCurator {
  public:
    [[nodiscard]] Result<WorkingContextSnapshot>
    curate(const WorkingContextSnapshot * /*previous*/, const ConversationCheckpoint &checkpoint,
           std::span<const ConversationSegmentEntry> /*recent_events*/,
           const ContextCurationOptions &) override {
        WorkingContextIdentity identity;
        identity.task = checkpoint.task_id;
        identity.task_epoch = checkpoint.task_epoch;
        identity.environment_epoch = checkpoint.environment_epoch;
        return working_context_from_checkpoint(checkpoint, identity);
    }
};

// Deterministic checkpoint the host reports to the curator: one constraint and
// one decision plus the summary, stamped for the live task frame.
[[nodiscard]] ConversationCheckpoint host_checkpoint(const SessionId &session, const TaskId &task,
                                                     std::uint64_t epoch, std::uint64_t watermark,
                                                     const std::string &revision) {
    ConversationCheckpoint checkpoint;
    checkpoint.id = conversation_checkpoint_id_from_seed(
        session.to_string() + "|" + std::to_string(watermark) + "|" + revision);
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = epoch;
    checkpoint.environment_epoch = 7;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = Timestamp::now();
    ConversationStatement constraint;
    constraint.content = "confirm before sending the report";
    constraint.source_events = {EventId::generate()};
    constraint.source_sequence = watermark - 1;
    constraint.confidence = 0.9;
    checkpoint.constraints.push_back(std::move(constraint));
    ConversationStatement decision;
    decision.content = "use the batch provider after the rate limit";
    decision.source_events = {EventId::generate()};
    decision.source_sequence = watermark - 1;
    decision.confidence = 0.9;
    checkpoint.decisions.push_back(std::move(decision));
    checkpoint.summary = "revision " + revision;
    checkpoint.source_events = {EventId::generate()};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] WorkingContextIdentity host_identity(const TaskId &task, std::uint64_t epoch) {
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = epoch;
    identity.environment_epoch = 7;
    return identity;
}

[[nodiscard]] WorkingContextCommitState host_live(const SessionId &session, const TaskId &task,
                                                  std::uint64_t epoch) {
    WorkingContextCommitState live;
    live.session = session;
    live.task = task;
    live.task_epoch = epoch;
    live.environment_epoch = 7;
    return live;
}

[[nodiscard]] std::shared_ptr<ModelProfile> example_profile() {
    auto profile = std::make_shared<ModelProfile>();
    profile->id = ModelProfileId::generate();
    profile->display_name = "wc-example-profile";
    profile->version = SemanticVersion{1, 0, 0};
    profile->dialect = ProtocolDialect::OpenAIResponsesV1;
    profile->endpoint_origin = "https://scripted.invalid";
    profile->api_prefix = "/v1";
    profile->model_selector = "scripted-model";
    profile->credential = SecretRef{"scripted"};
    auto verified = CapabilityEvidence::FixtureVerified;
    profile->capabilities.text = CapabilityFlag{true, verified, ""};
    profile->capabilities.image_input = CapabilityFlag{true, verified, ""};
    profile->capabilities.strict_json_schema = CapabilityFlag{true, verified, ""};
    profile->capabilities.function_tools = CapabilityFlag{true, verified, ""};
    profile->capabilities.parallel_tool_calls = CapabilityFlag{true, verified, ""};
    profile->capabilities.sse = CapabilityFlag{true, verified, ""};
    profile->capabilities.exact_token_count =
        CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    profile->capabilities.continuation = CapabilityFlag{true, verified, ""};
    profile->capabilities.remote_retention = CapabilityFlag{true, verified, ""};
    profile->capabilities.upload = CapabilityFlag{true, verified, ""};
    profile->capabilities.generation.seed = ParamMapping::Unsupported;
    profile->deadlines.connect = std::chrono::milliseconds{2'000};
    profile->deadlines.total = std::chrono::milliseconds{10'000};
    profile->default_data_policy.store = false;
    return profile;
}

[[nodiscard]] ModelResponse decision_response(const std::string &body) {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.status = ModelCompletionStatus::Completed;
    MessageOutput message;
    message.role = ModelRole::Assistant;
    OutputTextPart part;
    part.text = body;
    message.content.emplace_back(std::move(part));
    response.output.emplace_back(std::move(message));
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    response.usage.quality = UsageQuality::ProviderReported;
    response.requested_model = "scripted-model";
    return response;
}

// Concatenated text of the seam blocks (provenance source label frozen by the
// M25 contract) in one assembled request.
[[nodiscard]] std::string seam_text(const ModelRequest &request) {
    std::string text;
    for (const auto &item : request.input) {
        if (item.provenance.source != "mira.agent-loop.working-context.v1") {
            continue;
        }
        for (const auto &part : item.content) {
            if (const auto *text_part = std::get_if<TextPart>(&part)) {
                text += text_part->text;
                text += '\n';
            }
        }
    }
    return text;
}

[[nodiscard]] bool has_seam_block(const ModelRequest &request) {
    for (const auto &item : request.input) {
        if (item.provenance.source == "mira.agent-loop.working-context.v1") {
            return true;
        }
    }
    return false;
}

int run(const std::filesystem::path &root) {
    executor::Executor executor;
    executor::ExecutorConfig executor_config;
    executor_config.min_threads = 2;
    executor_config.max_threads = 4;
    executor_config.queue_capacity = 64;
    if (!executor.initialize(executor_config)) {
        std::cerr << "executor initialize failed\n";
        return 1;
    }
    int failures = 0;

    auto environment = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
    InMemoryWorkingContextStore store;
    MemoryEventStore events;
    const auto runtime_id = RuntimeId::generate();

    SqliteMemoryStoreOptions memory_options;
    memory_options.path = root / "wc-memory.db";
    auto memory_store = SqliteMemoryStore::open(executor, memory_options);
    if (!memory_store) {
        std::cerr << "memory store open failed\n";
        return 1;
    }
    MemoryConsolidator consolidator;

    // Runtime session and task frame: the identity the seam gate compares
    // against comes from the live task, not from host-side bookkeeping.
    MiraRuntime runtime({2, 16, 64});
    if (!runtime.initialize()) {
        std::cerr << "runtime initialize failed\n";
        return 1;
    }
    const auto session = runtime.open_session(environment);
    if (!session || !session.value().command.receipt(std::chrono::seconds(2)) ||
        !session.value().command.outcome(std::chrono::seconds(2))) {
        std::cerr << "open session failed\n";
        return 1;
    }
    const auto session_id = session.value().id;
    const auto task = runtime.submit_task(session_id, TaskSpec{"finish the quarterly report"});
    if (!task || !task.value().command.outcome(std::chrono::seconds(2))) {
        std::cerr << "submit task failed\n";
        return 1;
    }
    const auto task_id = task.value().id;
    const auto submitted = task.value().command.outcome(std::chrono::seconds(2));
    if (!submitted || !submitted.value().task.has_value()) {
        std::cerr << "task submission produced no frame\n";
        return 1;
    }
    const auto task_epoch = submitted.value().task->epoch;

    // Supervised curation plane (W3/W4 automation lives here, host-driven).
    ContextMemorySupervisor supervisor(executor, SupervisorConfig{}, &events, runtime_id,
                                       session_id);
    DeterministicCurator curator;
    WorkingContextAutoCurator auto_curator(supervisor, curator, store);

    // Scripted model side and the loop, wired with the snapshot supply seam.
    const auto profile = example_profile();
    ModelRouter router;
    router.register_profile(profile);
    ModelGateway gateway(executor, router, nullptr, PriceTable{}, ModelGatewayConfig{});
    const auto admission = std::make_shared<SimpleAdmissionGate>();
    gateway.set_admission_gate(admission);
    admission->activate(task_id, task_epoch);
    auto provider = std::make_shared<ScriptedProvider>(
        profile, std::vector<ModelResponse>{
                     decision_response(R"json({"action":"tap","x":0.5,"y":0.5,"reason":"go"})json"),
                     decision_response(R"json({"action":"tap","x":0.6,"y":0.6,"reason":"go"})json"),
                     decision_response(R"json({"action":"done","reason":"goal reached"})json")});
    gateway.register_provider(provider);

    HostGate curation_gate;
    HostGate merge_gate;
    provider->on_after_response(0, [&curation_gate] { curation_gate.enter_and_wait(); });
    provider->on_after_response(1, [&merge_gate] { merge_gate.enter_and_wait(); });

    AgentLoopSpec spec;
    spec.task_id = task_id;
    spec.session_id = session_id;
    spec.task_epoch = task_epoch;
    spec.profile_id = profile->id;
    spec.goal = "finish the quarterly report";
    AgentLoop loop(environment, gateway);
    loop.set_event_store(std::shared_ptr<IEventStore>(std::shared_ptr<IEventStore>(), &events),
                         runtime_id, session_id);
    // The seam: one read-only callback over the committed projection. Any
    // extra gating the host requires (e.g. its own environment epoch) closes
    // inside this callback — the loop holds no environment epoch itself.
    loop.set_working_context_supplier(
        [&store, session_id]() -> Result<std::optional<WorkingContextSnapshot>> {
            return store.latest(session_id);
        });

    const auto operation = runtime.begin_operation(task_id, StepId::generate());
    if (!operation) {
        std::cerr << "begin operation failed\n";
        return 1;
    }

    ModelDoneVerifier verifier;
    auto loop_future = executor.submit_auto([&loop, &spec, &verifier] {
        OperationContext context;
        context.session = spec.session_id;
        context.task = spec.task_id;
        context.task_epoch = spec.task_epoch;
        context.started_at = Timestamp::now();
        return loop.run(spec, context, verifier);
    });

    // --- Host orchestration 1 (W3 trigger): the session signal fires
    // curation whose commit lands before the loop's second request.
    curation_gate.wait_entered();
    {
        const auto checkpoint = host_checkpoint(session_id, task_id, task_epoch, 8, "1");
        WorkingContextRefreshInput input;
        input.checkpoint = checkpoint;
        input.identity = host_identity(task_id, task_epoch);
        input.live = host_live(session_id, task_id, task_epoch);
        const auto scheduled = auto_curator.on_signal(session_id, input);
        if (!scheduled) {
            std::cerr << "signal rejected\n";
            ++failures;
        } else if (scheduled->wait_for(std::chrono::seconds(5)) != std::future_status::ready ||
                   !scheduled->get().has_value()) {
            std::cerr << "supervised curation did not commit\n";
            ++failures;
        }
    }
    curation_gate.release();

    // --- Host orchestration 2 (W5 fork/merge): child session chain beside
    // the parent, merged at a strictly advancing parent watermark.
    SessionId child_session_id{};
    merge_gate.wait_entered();
    {
        const auto child_session = runtime.open_session(environment);
        if (!child_session || !child_session.value().command.receipt(std::chrono::seconds(2)) ||
            !child_session.value().command.outcome(std::chrono::seconds(2))) {
            std::cerr << "open child session failed\n";
            ++failures;
        } else {
            child_session_id = child_session.value().id;
            const auto child_task =
                runtime.submit_task(child_session_id, TaskSpec{"gather the login path facts"});
            if (!child_task || !child_task.value().command.outcome(std::chrono::seconds(2))) {
                std::cerr << "submit child task failed\n";
                ++failures;
            } else {
                const auto child_task_id = child_task.value().id;
                const auto child_submitted =
                    child_task.value().command.outcome(std::chrono::seconds(2));
                const auto child_epoch = child_submitted.value().task->epoch;

                const auto parent_base = store.latest(session_id);
                if (!parent_base.has_value() || !parent_base.value().has_value()) {
                    std::cerr << "parent snapshot missing at fork point\n";
                    ++failures;
                } else {
                    // Fork: child baseline derived from the committed parent.
                    WorkingContextForkSeed seed;
                    seed.child_session = child_session_id;
                    seed.child_identity = host_identity(child_task_id, child_epoch);
                    seed.child_watermark = 1;
                    auto baseline = fork_working_context(parent_base.value().value(), seed);
                    if (!baseline.has_value() ||
                        commit_working_context(
                            store, baseline.value(),
                            host_live(child_session_id, child_task_id, child_epoch))
                                .disposition != WorkingContextCommitDisposition::Committed) {
                        std::cerr << "fork baseline did not commit\n";
                        ++failures;
                    } else {
                        // The child chain advances on its own; the child Loop
                        // consumes it through its own seam (second consumer).
                        auto child_tip = baseline.value();
                        WorkingContextItem finding;
                        finding.content = "the second login path works";
                        finding.source_events = {EventId::generate()};
                        finding.source_sequence = 1;
                        finding.confidence = 0.8;
                        child_tip.constraints.push_back(std::move(finding));
                        child_tip.through_event_sequence = 2;
                        if (commit_working_context(
                                store, child_tip,
                                host_live(child_session_id, child_task_id, child_epoch))
                                .disposition != WorkingContextCommitDisposition::Committed) {
                            std::cerr << "child finding did not commit\n";
                            ++failures;
                        }

                        const auto child_profile = example_profile();
                        ModelRouter child_router;
                        child_router.register_profile(child_profile);
                        ModelGateway child_gateway(executor, child_router, nullptr, PriceTable{},
                                                   ModelGatewayConfig{});
                        const auto child_admission = std::make_shared<SimpleAdmissionGate>();
                        child_gateway.set_admission_gate(child_admission);
                        child_admission->activate(child_task_id, child_epoch);
                        auto child_provider = std::make_shared<ScriptedProvider>(
                            child_profile,
                            std::vector<ModelResponse>{decision_response(
                                R"json({"action":"done","reason":"facts gathered"})json")});
                        child_gateway.register_provider(child_provider);

                        AgentLoopSpec child_spec;
                        child_spec.task_id = child_task_id;
                        child_spec.session_id = child_session_id;
                        child_spec.task_epoch = child_epoch;
                        child_spec.profile_id = child_profile->id;
                        child_spec.goal = "gather the login path facts";
                        AgentLoop child_loop(environment, child_gateway);
                        child_loop.set_event_store(
                            std::shared_ptr<IEventStore>(std::shared_ptr<IEventStore>(), &events),
                            runtime_id, child_session_id);
                        child_loop.set_working_context_supplier(
                            [&store,
                             child_session_id]() -> Result<std::optional<WorkingContextSnapshot>> {
                                return store.latest(child_session_id);
                            });
                        ModelDoneVerifier child_verifier;
                        OperationContext child_context;
                        child_context.session = child_session_id;
                        child_context.task = child_task_id;
                        child_context.task_epoch = child_epoch;
                        child_context.started_at = Timestamp::now();
                        const auto child_outcome =
                            child_loop.run(child_spec, child_context, child_verifier);
                        if (!child_outcome.has_value() ||
                            child_outcome.value().outcome != LoopOutcome::Completed) {
                            std::cerr << "child loop did not complete\n";
                            ++failures;
                        } else {
                            const auto child_seam = seam_text(child_provider->requests().front());
                            if (child_seam.find("confirm before sending the report") ==
                                    std::string::npos ||
                                child_seam.find("the second login path works") ==
                                    std::string::npos) {
                                std::cerr << "child seam missed the fork baseline or finding\n";
                                ++failures;
                            }
                        }

                        // Child returns: delta folds into the parent at
                        // parent watermark 9 through the existing pipeline.
                        auto delta = working_context_delta_from_fork(baseline.value(), child_tip);
                        auto merged = merge_working_context_delta(
                            baseline.value(), parent_base.value().value(), delta.value(),
                            host_identity(task_id, task_epoch), 9);
                        if (!delta.has_value() || !merged.has_value() ||
                            commit_working_context(store, merged.value().merged,
                                                   host_live(session_id, task_id, task_epoch))
                                    .disposition != WorkingContextCommitDisposition::Committed) {
                            std::cerr << "merge did not commit at the advanced watermark\n";
                            ++failures;
                        }
                    }
                }
            }
        }
    }
    merge_gate.release();

    const auto loop_outcome = loop_future.get();
    if (!loop_outcome.has_value() || loop_outcome.value().outcome != LoopOutcome::Completed) {
        std::cerr << "main loop did not complete\n";
        ++failures;
    }

    // Request-level audit: context-blind, snapshot, merged child finding.
    const auto requests = provider->requests();
    if (requests.size() != 3) {
        std::cerr << "unexpected request count\n";
        ++failures;
    } else {
        if (has_seam_block(requests[0])) {
            std::cerr << "first request should be context-blind\n";
            ++failures;
        }
        const auto snapshot_seam = seam_text(requests[1]);
        const auto merged_seam = seam_text(requests[2]);
        if (!has_seam_block(requests[1]) ||
            snapshot_seam.find("confirm before sending the report") == std::string::npos) {
            std::cerr << "second request missed the committed snapshot\n";
            ++failures;
        }
        if (!has_seam_block(requests[2]) ||
            merged_seam.find("the second login path works") == std::string::npos) {
            std::cerr << "third request missed the merged child finding\n";
            ++failures;
        }
    }

    // Task boundary (W3 flush barrier): the flush future is bounded-awaited
    // BEFORE the terminal flag, then the runtime settles the task.
    {
        WorkingContextRefreshInput boundary_input;
        boundary_input.checkpoint = host_checkpoint(session_id, task_id, task_epoch, 8, "1");
        boundary_input.identity = host_identity(task_id, task_epoch);
        boundary_input.live = host_live(session_id, task_id, task_epoch);
        const auto boundary = auto_curator.flush(session_id, boundary_input);
        if (boundary.wait_for(std::chrono::seconds(5)) != std::future_status::ready ||
            !boundary.get().has_value()) {
            std::cerr << "boundary flush failed\n";
            ++failures;
        }
    }
    const auto completion = runtime.admit_operation_completion(operation.value());
    if (!completion || !completion.value().outcome(std::chrono::seconds(2))) {
        std::cerr << "operation completion rejected\n";
        ++failures;
    }
    const auto complete =
        runtime.complete_task(task_id, TaskOutcome{TaskState::Completed, std::nullopt});
    if (!complete || !complete.value().outcome(std::chrono::seconds(2)) ||
        complete.value().outcome(std::chrono::seconds(2)).value().status !=
            SettlementStatus::Applied) {
        std::cerr << "task completion rejected\n";
        ++failures;
    }

    // Host orchestration 3 (W4 promotion): the generic Deferrable route, the
    // future is consumed. `confirmed=false` keeps the promotion in the
    // pipeline's default approval-free path for the example.
    {
        auto promoted = store.latest(session_id);
        if (!promoted.has_value() || !promoted.value().has_value()) {
            std::cerr << "nothing committed to promote\n";
            ++failures;
        } else {
            const MemoryScope scope = [] {
                MemoryScope value;
                value.kind = MemoryScopeKind::Environment;
                value.subject_id = "wc-example";
                return value;
            }();
            auto future = supervisor.submit<WorkingContextPromotionReport>(
                "working-context-promotion", SupervisedOpClass::Deferrable, [&](SupervisorToken) {
                    return promote_working_context_to_memory(consolidator, *memory_store.value(),
                                                             promoted.value().value(), scope,
                                                             Timestamp::now());
                });
            const auto report = future.get();
            if (!report.has_value() ||
                report.value().consolidation.count_of(CandidateDisposition::Applied) == 0) {
                std::cerr << "promotion applied nothing\n";
                ++failures;
            }
        }
    }

    // Event audit: the closed loop left reconstructable evidence.
    bool saw_action = false;
    bool saw_verification = false;
    bool saw_settled = false;
    EventQuery query;
    query.session_id = session_id;
    while (true) {
        const auto page = events.read(query);
        if (!page) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            saw_action = saw_action || envelope.payload.type == "ActionDispatched";
            saw_verification = saw_verification || envelope.payload.type == "VerificationResult";
            saw_settled = saw_settled || envelope.payload.type == "LoopSettled";
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    if (!saw_action || !saw_verification || !saw_settled) {
        std::cerr << "event audit incomplete\n";
        ++failures;
    }

    // Shutdown order (plan §4.4): stop producers (the loop and the host
    // orchestration above are done), settle the supervisor, then the
    // executor; futures are consumed before teardown.
    const auto close = runtime.close_session(session_id);
    const auto child_close =
        child_session_id.is_nil() ? true : runtime.close_session(child_session_id).has_value();
    if (!close || !close.value().outcome(std::chrono::seconds(2)) || !child_close) {
        std::cerr << "session close failed\n";
        ++failures;
    }
    const auto shutdown = runtime.request_shutdown();
    if (!shutdown || !shutdown.value().outcome(std::chrono::seconds(2)) ||
        !runtime.finish_shutdown().clean) {
        std::cerr << "runtime shutdown not clean\n";
        ++failures;
    }
    const auto supervisor_shutdown = supervisor.begin_shutdown();
    if (!supervisor_shutdown.critical_drain_complete) {
        std::cerr << "supervisor drain incomplete\n";
        ++failures;
    }
    (void)memory_store.value()->close();
    if (executor.shutdown(true) != executor::ShutdownResult::Completed) {
        std::cerr << "executor shutdown incomplete\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "working context host example: OK\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    const auto root = std::filesystem::path{argc > 1 ? argv[1] : "mira-wc-host-example"};
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    const auto code = run(root);
    std::filesystem::remove_all(root, ignored);
    return code;
}
