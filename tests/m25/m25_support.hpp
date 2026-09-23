#pragma once

// Shared fixtures for the M25 host-integration-round IVA test matrix
// (DEC-045; plan §4 frozen seam semantics, §6 gates HI-G1..G4). Consumers:
// tests/m25/*.cpp.
//
// The loop harness follows the M3 fixture shape (scripted provider over the
// real ModelGateway, SimpleAdmissionGate admission) but records the
// ModelRequest objects themselves, so the seam tests assert on the assembled
// request: input item order, provenance source labels and text blocks. A
// FakeEnvironment serves one canned observation, which keeps the assembled
// request byte-stable across two loop instances for the zero-drift gate
// (HI-G1) — the simulator would produce a fresh screenshot per observe.

#include "../support/fake_environment.hpp"
#include "../support/harness_support.hpp"
#include "../support/test.hpp"

#include <executor/executor.hpp>

#include <mira/agent_loop.hpp>
#include <mira/context_consolidation.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/model_gateway.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mira::testing {

// ---------------------------------------------------------------------------
// Deterministic builders (M21-M24 style)
// ---------------------------------------------------------------------------

[[nodiscard]] inline Id128 m25_id_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

[[nodiscard]] inline SessionId m25_session_from_seed(std::uint64_t seed) {
    return SessionId{m25_id_from_seed(seed)};
}

[[nodiscard]] inline TaskId m25_task_from_seed(std::uint64_t seed) {
    return TaskId{m25_id_from_seed(seed)};
}

[[nodiscard]] inline EventId m25_event_from_seed(std::uint64_t seed) {
    return EventId{m25_id_from_seed(seed)};
}

[[nodiscard]] inline WorkingContextItem m25_item(std::string content, std::uint64_t event_seed,
                                                 double confidence) {
    WorkingContextItem item;
    item.content = std::move(content);
    item.source_events = {m25_event_from_seed(event_seed)};
    item.source_sequence = event_seed;
    item.confidence = confidence;
    return item;
}

[[nodiscard]] inline Timestamp m25_fixed_now() {
    Timestamp stamp;
    stamp.wall = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    return stamp;
}

// Minimal valid snapshot. Identity is supplied by the caller so tests can
// align it with the AgentLoopSpec task frame (HI-G1 identity gate) or
// deliberately misalign one leg.
[[nodiscard]] inline WorkingContextSnapshot m25_snapshot(const SessionId &session,
                                                         const TaskId &task, std::uint64_t epoch,
                                                         std::uint64_t seed) {
    WorkingContextSnapshot snapshot;
    snapshot.id = working_context_snapshot_id_from_seed("m25-snapshot-" + std::to_string(seed));
    snapshot.session_id = session;
    snapshot.task_id = task;
    snapshot.task_epoch = epoch;
    snapshot.environment_epoch = 7;
    snapshot.through_event_sequence = 48;
    snapshot.source_checkpoints = {
        conversation_checkpoint_id_from_seed("m25-checkpoint-" + std::to_string(seed))};
    snapshot.created_at = m25_fixed_now();
    return snapshot;
}

[[nodiscard]] inline ConversationCheckpoint m25_checkpoint(const SessionId &session,
                                                           const TaskId &task,
                                                           std::uint64_t watermark,
                                                           std::uint64_t revision) {
    ConversationCheckpoint checkpoint;
    checkpoint.id = conversation_checkpoint_id_from_seed(
        session.to_string() + "|" + std::to_string(watermark) + "|" + std::to_string(revision));
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = 3;
    checkpoint.environment_epoch = 7;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = m25_fixed_now();
    ConversationStatement constraint;
    constraint.content = "constraint r" + std::to_string(revision) + " confirm before sending";
    constraint.source_events = {m25_event_from_seed(watermark + 1)};
    constraint.source_sequence = watermark - 1;
    constraint.confidence = 0.9;
    checkpoint.constraints.push_back(std::move(constraint));
    ConversationStatement decision;
    decision.content = "decision r" + std::to_string(revision) + " use batch provider";
    decision.source_events = {m25_event_from_seed(watermark + 2)};
    decision.source_sequence = watermark - 1;
    decision.confidence = 0.9;
    checkpoint.decisions.push_back(std::move(decision));
    checkpoint.summary = "revision " + std::to_string(revision);
    checkpoint.source_events = {m25_event_from_seed(watermark + 1),
                                m25_event_from_seed(watermark + 2)};
    checkpoint.confidence = 0.9;
    return checkpoint;
}

[[nodiscard]] inline WorkingContextIdentity m25_identity(const TaskId &task) {
    WorkingContextIdentity identity;
    identity.task = task;
    identity.task_epoch = 3;
    identity.environment_epoch = 7;
    return identity;
}

[[nodiscard]] inline WorkingContextCommitState m25_live(const SessionId &session,
                                                        const TaskId &task) {
    WorkingContextCommitState live;
    live.session = session;
    live.task = task;
    live.task_epoch = 3;
    live.environment_epoch = 7;
    return live;
}

[[nodiscard]] inline WorkingContextRefreshInput
m25_refresh_input(const ConversationCheckpoint &checkpoint, const WorkingContextIdentity &identity,
                  const WorkingContextCommitState &live, std::uint64_t reported_events = 0) {
    WorkingContextRefreshInput input;
    input.checkpoint = checkpoint;
    input.identity = identity;
    input.live = live;
    input.reported_events = reported_events;
    return input;
}

// ---------------------------------------------------------------------------
// Event-store observation surface
// ---------------------------------------------------------------------------

// Collects the event types recorded for one session (page walk, harness
// style). The seam's diagnostic visibility (plan §4.2: skip/degrade is
// counted on the existing event surface) is asserted against the
// WorkingContext type family below: the baseline loop only ever emits
// UserMessageInjected / ToolExecuted / ActionDispatched / VerificationResult /
// LoopSettled, so any WorkingContext* type in the loop's own store is a seam
// diagnostic. This pins the implementation-frozen diagnostic type to the
// WorkingContext family token.
[[nodiscard]] inline std::vector<std::string> m25_session_event_types(const IEventStore &store,
                                                                      const SessionId &session) {
    std::vector<std::string> types;
    EventQuery query;
    query.session_id = session;
    while (true) {
        const auto page = store.read(query);
        if (!page) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            types.push_back(envelope.payload.type);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return types;
}

[[nodiscard]] inline bool m25_is_working_context_diagnostic(const std::string &event_type) {
    return event_type.find("WorkingContext") != std::string::npos;
}

[[nodiscard]] inline std::size_t m25_working_context_diagnostic_count(const IEventStore &store,
                                                                      const SessionId &session) {
    std::size_t count = 0;
    for (const auto &type : m25_session_event_types(store, session)) {
        if (m25_is_working_context_diagnostic(type)) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Request canonicalization (zero-drift byte comparison, HI-G1)
// ---------------------------------------------------------------------------

// Byte-stable rendering of everything build_request assembles, excluding the
// per-call generated ids (request_id / operation_id). With the canned
// FakeEnvironment observation held identical across two loop instances, two
// zero-drift runs must render equal bytes.
[[nodiscard]] inline std::string m25_request_wire_bytes(const ModelRequest &request) {
    std::string wire;
    wire += "v" + std::to_string(request.contract_version.major) + "." +
            std::to_string(request.contract_version.minor) + ";";
    wire += "task=" + request.task_id.to_string() + ";";
    wire += "epoch=" + std::to_string(request.task_epoch) + ";";
    wire += "profile=" + request.profile_id.to_string() + ";";
    wire += "tools=";
    for (const auto &tool : request.tools) {
        wire += tool.wire_name + ",";
    }
    wire += ";input=";
    for (const auto &item : request.input) {
        wire += "[r=" + std::to_string(static_cast<int>(item.role)) +
                ",src=" + item.provenance.source +
                ",auth=" + std::to_string(static_cast<int>(item.authority)) + ":";
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                wire +=
                    "T(" + std::to_string(static_cast<int>(text->sensitivity)) + ")" + text->text;
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                wire += "I(" + image->source.id.to_string() + "," + image->source.media_type + "," +
                        std::to_string(static_cast<long long>(image->source.byte_size)) + "," +
                        image->source.digest.to_string() + ")";
            } else {
                wire += "P?";
            }
            wire += "|";
        }
        wire += "]";
    }
    wire += ";schema=" + request.output_contract.canonical_schema_digest.to_string();
    wire += ";gen=" + (request.generation.max_output_tokens.has_value()
                           ? std::to_string(request.generation.max_output_tokens.value())
                           : std::string("none"));
    wire += ";budget=" + std::to_string(request.budget.max_output_tokens) + "/" +
            std::to_string(request.budget.max_requests);
    wire += ";store=" + std::string(request.data_policy.store ? "1" : "0");
    wire += ";tmpl=" + request.prompt_provenance.system_template_digest.to_string();
    return wire;
}

// Indices of the request input items rendered from the working-context seam
// (the frozen provenance source label, plan §4.2).
[[nodiscard]] inline std::vector<std::size_t> m25_seam_item_indices(const ModelRequest &request) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < request.input.size(); ++index) {
        if (request.input[index].provenance.source == "mira.agent-loop.working-context.v1") {
            indices.push_back(index);
        }
    }
    return indices;
}

[[nodiscard]] inline std::optional<std::size_t> m25_find_item_source(const ModelRequest &request,
                                                                     const std::string &source) {
    for (std::size_t index = 0; index < request.input.size(); ++index) {
        if (request.input[index].provenance.source == source) {
            return index;
        }
    }
    return std::nullopt;
}

// Concatenated text of the seam items, in request order: the surface the
// Layer 0 conversion order, truncation marker and content assertions read.
[[nodiscard]] inline std::string m25_seam_text(const ModelRequest &request) {
    std::string text;
    for (const auto index : m25_seam_item_indices(request)) {
        for (const auto &part : request.input[index].content) {
            if (const auto *text_part = std::get_if<TextPart>(&part)) {
                text += text_part->text;
                text += '\n';
            }
        }
    }
    return text;
}

[[nodiscard]] inline ModelRole m25_seam_role(const ModelRequest &request) {
    const auto indices = m25_seam_item_indices(request);
    return indices.empty() ? ModelRole::System : request.input[indices.front()].role;
}

[[nodiscard]] inline std::size_t m25_count_or(IWorkingContextStore &store, const SessionId &session,
                                              std::size_t fallback) {
    const auto counted = store.count(session);
    return counted.has_value() ? counted.value() : fallback;
}

// ---------------------------------------------------------------------------
// Supplier harness: one working-context store behind the frozen
// WorkingContextSupplier shape, with scripted modes and a call counter
// (exactly-once assertion, plan §4.2).
// ---------------------------------------------------------------------------

class WorkingContextSupplierHarness final {
  public:
    enum class Mode { Store, Empty, Error, ThrowStd, ThrowInt };

    WorkingContextSupplierHarness(IWorkingContextStore &store, SessionId session)
        : store_(&store), session_(std::move(session)) {}

    // Scripts the mode per supply call (last entry repeats); call order is
    // the loop's build_request order.
    void set_script(const std::vector<Mode> &modes) {
        std::lock_guard lock(mutex_);
        modes_ = modes;
    }

    [[nodiscard]] WorkingContextSupplier supplier() {
        return [this]() -> Result<std::optional<WorkingContextSnapshot>> {
            const Mode mode = next_mode();
            calls_.fetch_add(1, std::memory_order_acq_rel);
            switch (mode) {
            case Mode::Error: {
                Error error;
                error.code = ErrorCode::Unavailable;
                error.domain = "mira.test.m25";
                error.safe_message = "scripted supplier down";
                return Result<std::optional<WorkingContextSnapshot>>(error);
            }
            case Mode::ThrowStd:
                throw std::runtime_error("scripted supplier exploded");
            case Mode::ThrowInt:
                throw 42;
            case Mode::Empty:
                return Result<std::optional<WorkingContextSnapshot>>(
                    std::optional<WorkingContextSnapshot>{});
            case Mode::Store:
            default:
                // The read-only store projection; a missing snapshot is the
                // normal empty state (nullopt), not an error.
                return store_->latest(session_);
            }
        };
    }

    [[nodiscard]] std::size_t calls() const { return calls_.load(std::memory_order_acquire); }

    [[nodiscard]] const SessionId &session() const { return session_; }

  private:
    [[nodiscard]] Mode next_mode() {
        std::lock_guard lock(mutex_);
        if (cursor_ < modes_.size()) {
            return modes_[cursor_++];
        }
        return modes_.empty() ? Mode::Store : modes_.back();
    }

    IWorkingContextStore *store_;
    SessionId session_;
    std::mutex mutex_;
    std::vector<Mode> modes_;
    std::size_t cursor_ = 0;
    std::atomic<std::size_t> calls_{0};
};

// ---------------------------------------------------------------------------
// Loop harness: canned-observation environment + scripted provider over the
// real gateway, in the M3 fixture shape.
// ---------------------------------------------------------------------------

class SeamLoopHarness final {
  public:
    // `profile_id` lets two harness instances share one model profile
    // identity, so the zero-drift byte comparison covers the whole request
    // including the profile field.
    SeamLoopHarness(const SessionId &session, const TaskId &task,
                    ModelProfileId profile_id = ModelProfileId::generate())
        : environment_(std::make_shared<mira::test::FakeEnvironment>()) {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        (void)executor_.initialize(config);
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
        profile_->id = profile_id;
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                  ModelGatewayConfig{});
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);
        spec_.task_id = task;
        spec_.session_id = session;
        spec_.task_epoch = 1;
        spec_.profile_id = profile_->id;
        spec_.goal = "finish the quarterly report";
        admission_->activate(spec_.task_id, spec_.task_epoch);
    }

    ~SeamLoopHarness() { (void)executor_.shutdown(true); }

    SeamLoopHarness(const SeamLoopHarness &) = delete;
    SeamLoopHarness &operator=(const SeamLoopHarness &) = delete;

    [[nodiscard]] executor::Executor &executor() { return executor_; }
    [[nodiscard]] ModelGateway &gateway() { return *gateway_; }
    [[nodiscard]] AgentLoopSpec &spec() { return spec_; }
    [[nodiscard]] IEventStore &events() { return *events_; }
    [[nodiscard]] std::shared_ptr<mira::test::FakeEnvironment> &environment() {
        return environment_;
    }

    void script(const std::vector<ModelResponse> &responses) {
        provider_ = std::make_shared<RecordingProvider>(profile_, responses);
        gateway_->register_provider(provider_);
    }

    [[nodiscard]] RecordingProvider &provider() { return *provider_; }

    [[nodiscard]] std::unique_ptr<AgentLoop> make_loop() {
        auto loop = std::make_unique<AgentLoop>(environment_, *gateway_);
        loop->set_event_store(events_, runtime_, spec_.session_id);
        return loop;
    }

    [[nodiscard]] OperationContext loop_context() {
        OperationContext context;
        context.session = spec_.session_id;
        context.task = spec_.task_id;
        context.operation = OperationId::generate();
        context.task_epoch = spec_.task_epoch;
        context.started_at = Timestamp::now();
        return context;
    }

  private:
    executor::Executor executor_;
    std::shared_ptr<mira::test::FakeEnvironment> environment_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<RecordingProvider> provider_;
    std::shared_ptr<IEventStore> events_ = std::make_shared<MemoryEventStore>();
    RuntimeId runtime_ = RuntimeId::generate();
    AgentLoopSpec spec_;
};

// Two-step script: one discrete tap, then a verified done.
[[nodiscard]] inline std::vector<ModelResponse> m25_tap_then_done_script() {
    return {text_response(decision_body("tap", 0.5, 0.5)),
            text_response(R"json({"action":"done","reason":"goal reached"})json")};
}

// ---------------------------------------------------------------------------
// Gated scripted curator for the orchestration/lifecycle gates (M22 pattern):
// projects deterministically, optionally parking inside curate() until the
// supervisor's stop flag (begin_shutdown) releases it — the cooperative
// cancellation path AGENTS.md requires.
// ---------------------------------------------------------------------------

class M25ScriptedCurator final : public IContextCurator {
  public:
    [[nodiscard]] Result<WorkingContextSnapshot>
    curate(const WorkingContextSnapshot * /*previous*/, const ConversationCheckpoint &checkpoint,
           std::span<const ConversationSegmentEntry> /*recent_events*/,
           const ContextCurationOptions &options) override {
        {
            std::lock_guard lock(mutex_);
            ++entered_;
        }
        entered_cv_.notify_all();
        if (park_) {
            std::unique_lock lock(mutex_);
            while (!released_ && !options.cancelled()) {
                parked_cv_.wait_for(lock, std::chrono::milliseconds(1));
            }
        }
        if (options.cancelled()) {
            Error cancelled;
            cancelled.code = ErrorCode::Cancelled;
            cancelled.domain = "mira.test.m25";
            cancelled.safe_message = "scripted curator observed cancellation";
            return cancelled;
        }
        WorkingContextIdentity identity;
        identity.task = checkpoint.task_id;
        identity.task_epoch = checkpoint.task_epoch;
        identity.environment_epoch = checkpoint.environment_epoch;
        return working_context_from_checkpoint(checkpoint, identity);
    }

    void park() {
        std::lock_guard lock(mutex_);
        park_ = true;
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        parked_cv_.notify_all();
    }

    void wait_entered(std::size_t count) {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this, count] { return entered_ >= count; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable entered_cv_;
    std::condition_variable parked_cv_;
    std::size_t entered_ = 0;
    bool park_ = false;
    bool released_ = false;
};

// Releases the curator gate on every exit path so a failing MIRA_CHECK can
// never strand an Executor worker on a destroyed condition variable.
struct M25CuratorReleaser final {
    M25ScriptedCurator &curator;
    ~M25CuratorReleaser() { curator.release(); }
};

} // namespace mira::testing
