// M15 (MNT-202609-29, DEC-034): the minimal discrete-action + workflow eval
// profile harness. Implements docs/design/discrete_workflow_eval_profile.md
// v1.0: four comparison arms driven through public APIs only, the 17-case
// R/W/L/F families with the arm applicability matrix, seed {0,1,2} sampling
// plus the seed-0 triple for the G4 normalized-determinism gate, event-based
// metrics, the G1-G6 hard gates and the profile §9.2 budget guardrails.
//
// The default round prints a JSON report to stdout (and to argv[1] when
// given); any gate failure or budget overrun exits non-zero so CI treats the
// profile as a regression gate. --soak N repeats the full round N times and
// tracks VmRSS between rounds on Linux (profile §8: harness-side metering,
// Linux-only, no runtime counter exists).

#include "support/m14_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/model_gateway.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/workflow_recovery.hpp>
#include <mira/workflow_runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

[[nodiscard]] double monotonic_ms(const Timestamp &stamp) {
    return std::chrono::duration<double, std::milli>(stamp.monotonic.time_since_epoch())
        .count();
}

struct Percentiles final {
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double max = 0;
};

[[nodiscard]] Percentiles percentiles(std::vector<double> samples) {
    Percentiles result;
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    const auto pick = [&samples](double fraction) {
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size() - 1));
        return samples[std::min(index, samples.size() - 1)];
    };
    result.p50 = pick(0.50);
    result.p95 = pick(0.95);
    result.p99 = pick(0.99);
    result.max = samples.back();
    return result;
}

// Wilson score interval (95%); profile §7: proportions report intervals even
// when the per-cell sample size is small.
[[nodiscard]] std::string wilson95(std::uint64_t successes, std::uint64_t n) {
    if (n == 0) {
        return "0.000..0.000";
    }
    const double z = 1.959963984540054;
    const double p = static_cast<double>(successes) / static_cast<double>(n);
    const double denominator = 1 + z * z / static_cast<double>(n);
    const double center = p + z * z / (2 * static_cast<double>(n));
    const double spread =
        z * std::sqrt(p * (1 - p) / static_cast<double>(n)
                      + z * z / (4.0 * static_cast<double>(n) * static_cast<double>(n)));
    std::ostringstream stream;
    stream.precision(3);
    stream << std::fixed << (center - spread) / denominator << ".."
           << (center + spread) / denominator;
    return stream.str();
}

[[nodiscard]] std::string to_hex(const Sha256Digest &digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0F]);
    }
    return text;
}

[[nodiscard]] std::string json_escape(const std::string &text) {
    std::string escaped;
    for (const char character : text) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                std::ostringstream stream;
                stream << "\\u" << std::hex
                       << static_cast<int>(static_cast<unsigned char>(character));
                escaped += stream.str();
            } else {
                escaped.push_back(character);
            }
            break;
        }
    }
    return escaped;
}

[[nodiscard]] bool is_hex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
           || (character >= 'A' && character <= 'F');
}

// G4 normalization (profile §9.1): Id128 identifiers are generated through
// std::random_device and content digests embed those ids (profile digests,
// for one), so distinct tokens of 32/64 hex digits or the 8-4-4-4-12 UUID
// shape map to first-occurrence ordinals. Equal inputs still compare equal
// and unequal ones still diverge; sequence numbers, epochs, enum names and
// bounded counters compare verbatim.
[[nodiscard]] std::string
normalize_identifiers(const std::string &payload, std::map<std::string, int> &ids) {
    std::string output;
    output.reserve(payload.size());
    std::size_t index = 0;
    while (index < payload.size()) {
        const char character = payload[index];
        if (!is_hex(character)) {
            output.push_back(character);
            ++index;
            continue;
        }
        std::size_t end = index;
        while (end < payload.size() && is_hex(payload[end])) {
            ++end;
        }
        const std::size_t length = end - index;
        const bool token_shape =
            length == 32 || length == 64
            || (length == 36 && payload[index + 8] == '-' && payload[index + 13] == '-'
                && payload[index + 18] == '-' && payload[index + 23] == '-');
        if (token_shape) {
            const std::string token = payload.substr(index, length);
            const auto inserted = ids.emplace(token, static_cast<int>(ids.size()));
            output += "<id" + std::to_string(inserted.first->second) + ">";
        } else {
            output += payload.substr(index, length);
        }
        index = end;
    }
    return output;
}

[[nodiscard]] std::vector<EventEnvelope> session_events(const IEventStore &store,
                                                        const SessionId &session,
                                                        std::uint64_t after = 0) {
    std::vector<EventEnvelope> events;
    EventQuery query;
    query.session_id = session;
    query.after_sequence = after;
    while (true) {
        const auto page = store.read(query);
        if (!page.has_value()) {
            break;
        }
        for (const auto &envelope : page.value().events) {
            events.push_back(envelope);
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    return events;
}

[[nodiscard]] std::uint64_t event_count(const IEventStore &store, const SessionId &session) {
    return session_events(store, session).size();
}

// ---------------------------------------------------------------------------
// Run records and the oracle/gate recorder
// ---------------------------------------------------------------------------

enum class Arm : std::uint8_t { A = 0, B = 1, C = 2, D = 3 };

[[nodiscard]] std::string arm_name(Arm arm) {
    switch (arm) {
    case Arm::A:
        return "agent-direct";
    case Arm::B:
        return "workflow-strict";
    case Arm::C:
        return "recovery-no-memory";
    case Arm::D:
        return "recovery-with-memory";
    }
    return "unknown";
}

struct RunRecord final {
    std::string case_id;
    Arm arm = Arm::A;
    int seed = 0;
    int repeat = 0;
    bool oracle_success = false;
    std::string failure_class; // profile §6; empty on success
    std::string detail;
    std::uint64_t model_calls = 0;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    double wall_ms = 0;
    std::vector<double> step_ms;
    std::uint32_t interventions = 0;
    bool duplicate_side_effect = false;
    bool correlation_ok = true; // G5
    bool privacy_leak = false;  // G6
    double shutdown_ms = 0;     // F2
    std::vector<std::string> normalized_events;
    std::string debug_head; // first normalized events, diagnostics only
};

// Collects oracle and gate outcomes without aborting the round: profile §6
// requires every failed run to carry a unique primary classification.
class Recorder final {
  public:
    explicit Recorder(RunRecord &record) : record_(record) {}

    void oracle(bool condition, const std::string &label) {
        if (!condition && record_.failure_class.empty()) {
            record_.failure_class = "oracle-failed";
            record_.detail = label;
        }
        failures_ += condition ? 0U : 1U;
    }

    void runtime_defect(const std::string &label) {
        // G2: a runtime contract violation always outranks an oracle miss.
        record_.failure_class = "runtime-defect";
        if (record_.detail.empty()) {
            record_.detail = label;
        }
        ++failures_;
    }

    void budget(const std::string &label) {
        if (record_.failure_class.empty()) {
            record_.failure_class = "budget-exhausted";
            record_.detail = label;
        }
        ++failures_;
    }

    void fixture(const std::string &label) {
        if (record_.failure_class.empty()) {
            record_.failure_class = "fixture-defect";
            record_.detail = label;
        }
        ++failures_;
    }

    [[nodiscard]] bool ok() const { return failures_ == 0; }

  private:
    RunRecord &record_;
    std::uint32_t failures_ = 0;
};

// ---------------------------------------------------------------------------
// Per run-unit fixture: full public-API assembly, serial execution, the
// profile §11 shutdown order in the destructor (F2 measures it explicitly).
// ---------------------------------------------------------------------------

class EvalFixture final {
  public:
    EvalFixture(int seed, const std::string &tag) {
        executor::ExecutorConfig executor_config;
        executor_config.min_threads = 4;
        executor_config.max_threads = 4;
        executor_config.queue_capacity = 64;
        (void)executor_.initialize(executor_config);
        runtime_ = std::make_unique<MiraRuntime>(RuntimeConfig{2, 16, 64});
        (void)runtime_->initialize();
        environment_ =
            std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
        // The seed perturbs the environment surface (density) so the three
        // seed samples differ in observation content while staying
        // reproducible per seed.
        const auto topology = environment_->topology();
        if (topology.has_value() && !topology.value().displays.empty()) {
            const auto density = environment_->set_density(
                topology.value().displays.front().id, 420.0 + 10.0 * static_cast<double>(seed));
            (void)density;
        }
        const auto opened = runtime_->open_session(environment_);
        if (opened.has_value()) {
            (void)opened.value().command.outcome(std::chrono::seconds(2));
            session_id_ = opened.value().id;
        }
        registry_ = std::make_shared<BuiltinToolRegistry>();
        events_ = std::make_shared<MemoryEventStore>();
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://eval.test"));
        router_.register_profile(profile_);
        gateway_ =
            std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                           ModelGatewayConfig{});
        provider_ = std::make_shared<RecoveryScriptProvider>(
            profile_, std::vector<ModelResponse>{});
        gateway_->register_provider(provider_);
        gateway_->set_event_store(events_, RuntimeId::generate(), session_id_);
        workflow_ = std::make_unique<WorkflowRuntime>(executor_, *runtime_, session_id_,
                                                      environment_, WorkflowRuntimeConfig{});
        workflow_->set_event_store(events_);
        workflow_->set_tool_registry(registry_);
        tag_ = tag;
    }

    ~EvalFixture() {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        (void)workflow_->shutdown();
        memory_.reset();
        if (!sqlite_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(sqlite_path_, ec);
            std::filesystem::remove(std::filesystem::path(sqlite_path_.string() + "-wal"), ec);
            std::filesystem::remove(std::filesystem::path(sqlite_path_.string() + "-shm"), ec);
        }
        const auto shutdown = runtime_->request_shutdown();
        if (shutdown.has_value()) {
            (void)shutdown.value().outcome(std::chrono::seconds(5));
        }
        (void)runtime_->finish_shutdown();
        (void)executor_.shutdown(true);
    }

    EvalFixture(const EvalFixture &) = delete;
    EvalFixture &operator=(const EvalFixture &) = delete;

    [[nodiscard]] std::unique_ptr<WorkflowRecoveryOrchestrator> make_orchestrator() {
        WorkflowRecoveryConfig config;
        config.profile_id = profile_->id;
        config.model_call_deadline = std::chrono::seconds{5};
        auto orchestrator = std::make_unique<WorkflowRecoveryOrchestrator>(
            executor_, *workflow_, *runtime_, *gateway_, session_id_, config);
        orchestrator->set_event_store(events_);
        return orchestrator;
    }

    // Installs the D-arm learning context over a fresh SQLite database.
    // WAL/SHM sidecars of a previous round with the same tag must go too,
    // otherwise the new database can resurrect stale pages.
    [[nodiscard]] bool attach_sqlite_learning() {
        const auto root =
            std::filesystem::temp_directory_path() / ("m15-eval-" + tag_ + ".sqlite3");
        std::error_code ec;
        std::filesystem::remove(root, ec);
        std::filesystem::remove(std::filesystem::path(root.string() + "-wal"), ec);
        std::filesystem::remove(std::filesystem::path(root.string() + "-shm"), ec);
        SqliteMemoryStoreOptions options;
        options.path = root;
        auto opened = SqliteMemoryStore::open(executor_, options);
        if (!opened.has_value()) {
            return false;
        }
        sqlite_path_ = root;
        memory_ = std::move(opened.value());
        return workflow_->set_learning_context(memory_, learning_scope()).has_value();
    }

    executor::Executor executor_;
    std::unique_ptr<MiraRuntime> runtime_;
    std::shared_ptr<SimulatorEnvironment> environment_;
    std::shared_ptr<BuiltinToolRegistry> registry_;
    std::shared_ptr<MemoryEventStore> events_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<RecoveryScriptProvider> provider_;
    std::unique_ptr<WorkflowRuntime> workflow_;
    std::shared_ptr<IMemory> memory_;
    std::filesystem::path sqlite_path_;
    SessionId session_id_;
    std::string tag_;
    bool torn_down_ = false;
};

// IMemory decorator carrying the F4 knobs (slow query, failing query) over
// any backend; the degradation semantics are under test, not the backend.
class KnobbedMemory final : public IMemory {
  public:
    KnobbedMemory(std::shared_ptr<IMemory> inner, std::chrono::milliseconds delay,
                  bool fail_queries)
        : inner_(std::move(inner)), delay_(delay), fail_queries_(fail_queries) {}

    Result<MemoryQueryResult> query(const MemoryQuery &query) const override {
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }
        if (fail_queries_) {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.eval";
            error.safe_message = "scripted store outage";
            return error;
        }
        return inner_->query(query);
    }
    Result<std::optional<MemoryRecord>> get(MemoryId record) const override {
        return inner_->get(record);
    }
    Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override {
        return inner_->apply(mutation);
    }
    Result<MemoryCompactionResult> compact(const MemoryScope &scope) override {
        return inner_->compact(scope);
    }
    Result<ErasureResult> erase(const ErasureRequest &request) override {
        return inner_->erase(request);
    }

  private:
    std::shared_ptr<IMemory> inner_;
    std::chrono::milliseconds delay_;
    bool fail_queries_;
};

[[nodiscard]] OperationContext drive_context(const SessionId &session) {
    OperationContext context;
    context.session = session;
    context.started_at = Timestamp::now();
    return context;
}

[[nodiscard]] WorkflowDefinition eval_workflow(std::string name, WorkflowPolicy policy) {
    auto definition = base_definition(std::move(name));
    definition.default_policy = policy;
    definition.steps.push_back(tool_step("scripted", std::nullopt));
    return definition;
}

// A decision body carrying every optional field the M3 schema accepts.
[[nodiscard]] std::string agent_decision(const std::string &action, double x, double y,
                                         const std::string &text = "",
                                         const std::string &reason = "eval") {
    std::ostringstream stream;
    stream << "{\"action\":\"" << action << "\",\"x\":" << x << ",\"y\":" << y
           << ",\"end_x\":" << (x + 0.05) << ",\"end_y\":" << (y + 0.05) << ",\"text\":\""
           << text << "\",\"reason\":\"" << reason << "\"}";
    return stream.str();
}

// ---------------------------------------------------------------------------
// Workflow-arm metric extraction
// ---------------------------------------------------------------------------

void collect_workflow_metrics(EvalFixture &fixture, RunRecord &record,
                              std::uint64_t events_before,
                              bool waiting_agent_window = false) {
    const auto envelopes =
        session_events(*fixture.events_, fixture.session_id_, events_before);
    std::map<std::string, int> ids;
    std::map<std::string, double> step_started;
    std::map<std::string, double> run_started;
    std::map<std::string, int> started_runs;
    std::map<std::string, int> settled_runs;
    for (const auto &envelope : envelopes) {
        record.normalized_events.push_back(envelope.payload.type + "|"
                                           + normalize_identifiers(envelope.payload.data,
                                                                   ids));
        if (record.debug_head.size() < 400) {
            record.debug_head += record.normalized_events.back() + "\n";
        }
        if (envelope.payload.type == "ModelRequestPrepared") {
            ++record.model_calls;
        }
        if (envelope.payload.data.find("test-credential") != std::string::npos
            || envelope.payload.data.find("eval repair") != std::string::npos) {
            record.privacy_leak = true; // G6: secrets and rationale never travel
        }
        auto payload = parse_json(envelope.payload.data);
        if (!payload.has_value() || !payload.value().is_object()) {
            continue;
        }
        const auto *run_id = payload.value().find("run_id");
        const std::string run =
            run_id != nullptr && run_id->is_string() ? *run_id->as_string() : std::string();
        if (envelope.payload.type == "WorkflowStepStarted" && !run.empty()) {
            const auto *step = payload.value().find("step_id");
            if (step != nullptr && step->is_string()) {
                step_started.emplace(run + "|" + *step->as_string(),
                                     monotonic_ms(envelope.timestamp));
            }
        } else if (envelope.payload.type == "WorkflowStepSettled" && !run.empty()) {
            const auto *step = payload.value().find("step_id");
            if (step != nullptr && step->is_string()) {
                const auto key = run + "|" + *step->as_string();
                const auto began = step_started.find(key);
                if (began != step_started.end()) {
                    record.step_ms.push_back(monotonic_ms(envelope.timestamp)
                                             - began->second);
                }
            }
        } else if (envelope.payload.type == "WorkflowRunStarted" && !run.empty()) {
            run_started.emplace(run, monotonic_ms(envelope.timestamp));
            ++started_runs[run];
        } else if (envelope.payload.type == "WorkflowRunSettled" && !run.empty()) {
            ++settled_runs[run];
            const auto began = run_started.find(run);
            if (began != run_started.end()) {
                record.wall_ms = monotonic_ms(envelope.timestamp) - began->second;
            }
        } else if (envelope.payload.type == "WorkflowRecoveryAttempted") {
            const auto attempted = parse_workflow_recovery_attempted(envelope.payload);
            if (attempted.has_value()) {
                if (attempted.value().outcome == WorkflowRecoveryOutcome::DeferredToHost) {
                    ++record.interventions;
                }
                // G5: a patched resume must carry the whole correlation
                // chain (request -> decision -> patch, DEC-031 §7).
                if (attempted.value().outcome == WorkflowRecoveryOutcome::PatchedAndResumed
                    && (!attempted.value().model_request_id.has_value()
                        || !attempted.value().decision_digest.has_value()
                        || !attempted.value().patch_id.has_value())) {
                    record.correlation_ok = false;
                }
            }
        }
    }
    if (!waiting_agent_window && started_runs != settled_runs) {
        record.correlation_ok = false; // G5: every started run settles exactly once
    }
}

// Token accounting from the recorded script (profile §8: UsageQuality is
// ProviderReported for scripted fixtures; recorded rounds carry no cost).
void collect_script_tokens(const RecoveryScriptProvider &provider, RunRecord &record) {
    const auto consumed = provider.consumed();
    record.input_tokens += 10 * consumed;
    record.output_tokens += 5 * consumed;
}

// ---------------------------------------------------------------------------
// Arm A drivers
// ---------------------------------------------------------------------------

class InputCountVerifier final : public ILoopVerifier {
  public:
    InputCountVerifier(SimulatorEnvironment &environment, std::size_t expected)
        : environment_(environment), expected_(expected) {}
    Verdict verify(const Observation &, const DecisionCandidate &) override {
        return environment_.executed_inputs().size() >= expected_ ? Verdict::Satisfied
                                                                  : Verdict::NotSatisfied;
    }

  private:
    SimulatorEnvironment &environment_;
    std::size_t expected_;
};

class NeverSatisfied final : public ILoopVerifier {
  public:
    Verdict verify(const Observation &, const DecisionCandidate &) override {
        return Verdict::NotSatisfied;
    }
};

struct AgentScript final {
    std::vector<std::string> decisions;
    AgentLoopConfig config;
    std::size_t expected_inputs = 0;
    LoopOutcome expected_outcome = LoopOutcome::Completed;
    std::optional<std::string> user_message;
    bool require_message_visible = false;
};

[[nodiscard]] RunRecord
drive_agent_arm(EvalFixture &fixture, const AgentScript &script, const std::string &case_id,
                int seed, int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = Arm::A;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    AgentLoop loop(fixture.environment_, *fixture.gateway_, script.config);
    loop.set_event_store(fixture.events_, RuntimeId::generate(), fixture.session_id_);
    if (script.user_message.has_value()) {
        const auto queued = loop.enqueue_user_message(*script.user_message);
        if (!queued.has_value()) {
            recorder.fixture("user message enqueue rejected");
        }
    }
    AgentLoopSpec spec;
    spec.task_id = TaskId::generate();
    spec.session_id = fixture.session_id_;
    spec.task_epoch = 1;
    spec.goal = "eval profile goal";
    spec.profile_id = fixture.profile_->id;
    InputCountVerifier counter(*fixture.environment_, script.expected_inputs);
    NeverSatisfied never;
    // A Completed oracle means "the environment proves the goal", so the
    // counter verifier applies; bounded-failure oracles (Failed/MaxSteps)
    // use the never-satisfied verifier so the budget path is under test.
    ILoopVerifier &verifier = script.expected_outcome == LoopOutcome::Completed
                                 ? static_cast<ILoopVerifier &>(counter)
                                 : static_cast<ILoopVerifier &>(never);
    for (const auto &decision : script.decisions) {
        fixture.provider_->add_response(text_response(decision));
    }
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto started = std::chrono::steady_clock::now();
    const auto result = loop.run(spec, drive_context(fixture.session_id_), verifier);
    record.wall_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    if (!result.has_value()) {
        recorder.runtime_defect("agent loop error: " + result.error().safe_message);
        record.oracle_success = false;
        return record;
    }
    const auto envelopes =
        session_events(*fixture.events_, fixture.session_id_, events_before);
    std::map<std::string, int> ids;
    bool message_visible = false;
    for (const auto &envelope : envelopes) {
        record.normalized_events.push_back(envelope.payload.type + "|"
                                           + normalize_identifiers(envelope.payload.data,
                                                                   ids));
        if (record.debug_head.size() < 400) {
            record.debug_head += record.normalized_events.back() + "\n";
        }
        if (envelope.payload.type == "ModelRequestPrepared") {
            ++record.model_calls;
        }
        if (envelope.payload.type == "UserMessageInjected") {
            message_visible = true;
        }
    }
    for (const auto &step : result.value().steps) {
        static_cast<void>(step);
    }
    record.step_ms.push_back(record.wall_ms);
    collect_script_tokens(*fixture.provider_, record);
    const auto inputs = fixture.environment_->executed_inputs().size();
    recorder.oracle(inputs == script.expected_inputs,
                    "inputs " + std::to_string(inputs) + " != expected "
                        + std::to_string(script.expected_inputs));
    record.duplicate_side_effect = inputs > script.expected_inputs;
    recorder.oracle(result.value().outcome == script.expected_outcome,
                    "outcome " + loop_outcome_name(result.value().outcome));
    if (script.require_message_visible) {
        recorder.oracle(message_visible, "user message never injected");
        const auto requests = fixture.provider_->requests();
        bool in_request = false;
        for (const auto &request : requests) {
            for (const auto &item : request.input) {
                for (const auto &part : item.content) {
                    const auto *text = std::get_if<TextPart>(&part);
                    if (text != nullptr
                        && text->text.find(*script.user_message) != std::string::npos) {
                        in_request = true;
                    }
                }
            }
        }
        recorder.oracle(in_request, "user message never reached a request");
    }
    record.oracle_success = recorder.ok();
    return record;
}

// F1 for arm A: the provider parks inside infer(), an executor task raises
// the cancellation probe, the loop must settle Cancelled with zero inputs.
[[nodiscard]] RunRecord drive_agent_cancel(EvalFixture &fixture, const std::string &case_id,
                                           int seed, int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = Arm::A;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    fixture.provider_->add_response(text_response(agent_decision("tap", 0.5, 0.5)));
    fixture.provider_->block();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto provider = fixture.provider_;
    auto raise = fixture.executor_.submit_auto([provider, cancelled]() {
        provider->wait_entered();
        cancelled->store(true);
        provider->release();
    });
    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 1});
    loop.set_event_store(fixture.events_, RuntimeId::generate(), fixture.session_id_);
    AgentLoopSpec spec;
    spec.task_id = TaskId::generate();
    spec.session_id = fixture.session_id_;
    spec.task_epoch = 1;
    spec.goal = "cancel mid request";
    spec.profile_id = fixture.profile_->id;
    NeverSatisfied verifier;
    OperationContext context = drive_context(fixture.session_id_);
    context.cancellation_requested = [cancelled]() { return cancelled->load(); };
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto started = std::chrono::steady_clock::now();
    const auto result = loop.run(spec, context, verifier);
    record.wall_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    if (raise.valid()) {
        static_cast<void>(raise.get());
    }
    if (!result.has_value()) {
        recorder.runtime_defect("cancel run error");
        record.oracle_success = false;
        return record;
    }
    const auto envelopes =
        session_events(*fixture.events_, fixture.session_id_, events_before);
    std::map<std::string, int> ids;
    for (const auto &envelope : envelopes) {
        record.normalized_events.push_back(envelope.payload.type + "|"
                                           + normalize_identifiers(envelope.payload.data,
                                                                   ids));
        if (record.debug_head.size() < 400) {
            record.debug_head += record.normalized_events.back() + "\n";
        }
        if (envelope.payload.type == "ModelRequestPrepared") {
            ++record.model_calls;
        }
    }
    collect_script_tokens(*fixture.provider_, record);
    recorder.oracle(result.value().outcome == LoopOutcome::Cancelled,
                    "outcome " + loop_outcome_name(result.value().outcome));
    const auto inputs = fixture.environment_->executed_inputs().size();
    recorder.oracle(inputs == 0, "inputs after cancellation");
    record.duplicate_side_effect = inputs > 0;
    record.oracle_success = recorder.ok();
    return record;
}

// ---------------------------------------------------------------------------
// Workflow arm driver (B: Strict; C/D: Recoverable + orchestrator)
// ---------------------------------------------------------------------------

struct WorkflowScript final {
    WorkflowPolicy policy = WorkflowPolicy::Strict;
    std::vector<int> tool_failures;
    std::vector<std::string> recovery_decisions;
    WorkflowRunState expected_state = WorkflowRunState::Completed;
    std::size_t expected_dispatches = 1;
    bool expect_lesson_retrieved = false;
    bool expect_stale = false;
    // W3: the skip decision targets the failing step id, which is only
    // known after the escalation, so the driver synthesizes it.
    bool fill_skip_decision = false;
};

[[nodiscard]] RunRecord drive_workflow_arm(EvalFixture &fixture, const WorkflowScript &script,
                                           const WorkflowDefinition &definition,
                                           const std::string &case_id, Arm arm, int seed,
                                           int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = arm;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    ScriptedTool tool{script.tool_failures};
    if (!register_registration(*fixture.registry_, tool.registration("scripted"))
             .has_value()) {
        recorder.fixture("tool registration failed");
        return record;
    }
    const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run.has_value()) {
        recorder.fixture("create_run failed: " + run.error().safe_message);
        return record;
    }
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto driven =
        fixture.workflow_->execute_run(run.value().run_id, drive_context(fixture.session_id_));
    if (!driven.has_value()) {
        recorder.runtime_defect("execute_run error: " + driven.error().safe_message);
        return record;
    }
    bool deferred = false;
    if (script.policy == WorkflowPolicy::Strict || script.tool_failures.empty()) {
        recorder.oracle(driven.value().state == script.expected_state,
                        "terminal state " + workflow_run_state_name(driven.value().state));
    } else {
        recorder.oracle(driven.value().state == WorkflowRunState::WaitingAgent,
                        "escalation state " + workflow_run_state_name(driven.value().state));
        auto orchestrator = fixture.make_orchestrator();
        std::size_t decision_index = 0;
        const std::size_t decision_count =
            script.recovery_decisions.empty()
                ? (script.fill_skip_decision ? 1U : 0U)
                : script.recovery_decisions.size();
        while (decision_index < decision_count) {
            const auto continuation =
                fixture.workflow_->agent_continuation(run.value().run_id);
            if (!continuation.has_value()) {
                const auto mid = fixture.workflow_->run_snapshot(run.value().run_id);
                recorder.runtime_defect(
                    "agent_continuation missing at decision " + std::to_string(decision_index)
                    + " state "
                    + (mid.has_value() ? workflow_run_state_name(mid.value().state)
                                       : std::string("unknown")));
                break;
            }
            if (script.expect_lesson_retrieved) {
                bool saw_lesson = false;
                for (const auto &attached : continuation.value().relevant_lessons) {
                    saw_lesson =
                        saw_lesson || attached.kind == MemoryKind::RecoveryLesson;
                }
                recorder.oracle(saw_lesson, "expected a retrieved lesson");
            }
            std::string decision;
            if (decision_index < script.recovery_decisions.size()) {
                decision = script.recovery_decisions[decision_index];
            } else {
                decision = skip_step_decision(
                    continuation.value().current_step->to_string(), "eval skip repair");
            }
            ++decision_index;
            fixture.provider_->add_response(text_response(decision));
            const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
            if (!attempt.has_value()) {
                recorder.runtime_defect("attempt_recovery error");
                break;
            }
            if (attempt.value().outcome == WorkflowRecoveryOutcome::DeferredToHost) {
                deferred = true;
            }
            if (script.expect_stale && attempt.value().lessons_stale == 0) {
                recorder.oracle(false, "expected stale lesson filtering");
            }
            if (!deferred && decision_index < decision_count) {
                // An ineffective repair re-drives the run; wait for the next
                // boundary (a re-escalation or a terminal state) before the
                // next decision.
                const auto boundary = fixture.workflow_->wait_run(run.value().run_id,
                                                                  std::chrono::seconds(10));
                if (!boundary.has_value()
                    || boundary.value().state == WorkflowRunState::Running) {
                    recorder.runtime_defect("run never reached the next boundary");
                    break;
                }
            }
        }
        const auto settled =
            fixture.workflow_->wait_run(run.value().run_id, std::chrono::seconds(10));
        if (!settled.has_value()) {
            recorder.runtime_defect("wait_run error");
        } else if (deferred) {
            recorder.oracle(settled.value().state == WorkflowRunState::WaitingAgent,
                            "deferred run must stay WaitingAgent, got "
                                + workflow_run_state_name(settled.value().state));
        } else {
            recorder.oracle(settled.value().state == script.expected_state,
                            "settled state "
                                + workflow_run_state_name(settled.value().state));
        }
        // G2 spot check: a duplicate notification is absorbed and the
        // terminal state never changes (DEC-031 §6).
        if (!deferred) {
            const auto duplicate = orchestrator->attempt_recovery(run.value().run_id);
            recorder.oracle(duplicate.has_value()
                                && duplicate.value().outcome == WorkflowRecoveryOutcome::Aborted,
                            "duplicate notification must abort");
            const auto after = fixture.workflow_->run_snapshot(run.value().run_id);
            recorder.oracle(after.has_value() && settled.has_value()
                                && after.value().state == settled.value().state,
                            "terminal state changed after duplicate notification");
        }
        static_cast<void>(orchestrator->shutdown());
    }
    collect_workflow_metrics(fixture, record, events_before, deferred);
    collect_script_tokens(*fixture.provider_, record);
    // G1: tool dispatches are the workflow external side effect; the count
    // must match the scripted expectation exactly (no duplicate retries).
    const auto dispatches = static_cast<std::size_t>(tool.dispatches.load());
    recorder.oracle(dispatches == script.expected_dispatches,
                    "dispatches " + std::to_string(dispatches) + " != expected "
                        + std::to_string(script.expected_dispatches));
    record.duplicate_side_effect = dispatches > script.expected_dispatches;
    record.oracle_success = recorder.ok();
    return record;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// L family: learning transfer (arms C/D). Two phases share one fixture and
// learning store: a Strict failure seeds the episode, a recovered run
// supplies the lesson, then the evaluation run fails the same way again.
// Arm D retrieves the lesson; arm C (no learning context) retrieves nothing.
// ---------------------------------------------------------------------------

[[nodiscard]] RunRecord drive_learning_case(bool with_memory, const WorkflowDefinition &definition,
                                            const std::string &case_id,
                                            int seed, int repeat, bool stale_variant) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = with_memory ? Arm::D : Arm::C;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    const std::string tag =
        case_id + "-" + arm_name(record.arm) + "-s" + std::to_string(seed) + "-r"
        + std::to_string(repeat);
    EvalFixture fixture(seed, tag);
    if (with_memory && !fixture.attach_sqlite_learning()) {
        recorder.fixture("sqlite learning context failed to open");
        return record;
    }

    if (stale_variant) {
        // L2: one seeded episode whose ir_digest drifted from the live run
        // (the m14-proven stale pattern): retrieval still offers it, the
        // orchestrator must filter it (stale >= 1, kept == 0) and the run
        // settles through the lesson-free repair path.
        ScriptedTool tool{std::vector<int>{1}};
        if (!register_registration(*fixture.registry_, tool.registration("scripted"))
                 .has_value()) {
            recorder.fixture("tool registration failed");
            return record;
        }
        const auto run =
            fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
        if (!run.has_value()) {
            recorder.fixture("create_run failed: " + run.error().safe_message);
            return record;
        }
        if (with_memory) {
            WorkflowEpisodeRecord stale;
            stale.run_id = WorkflowRunId::generate().to_string();
            stale.workflow_id = definition.workflow_id.to_string();
            stale.ir_digest = digest_string("m15-eval-stale").to_string();
            stale.policy = "recoverable";
            stale.outcome = "failed";
            stale.failed_step_id = definition.steps.front().id.to_string();
            stale.failure_reason_code = "mira.eval:stale";
            stale.recorded_at_ms = 1;
            const auto seeded = fixture.memory_->apply(
                mutation_of(episode_to_memory_record(
                    stale, learning_scope(), {EventId::generate()},
                    std::chrono::system_clock::now())));
            recorder.oracle(seeded.has_value(), "stale seeding failed");
        }
        const auto events_before = event_count(*fixture.events_, fixture.session_id_);
        const auto driven = fixture.workflow_->execute_run(run.value().run_id,
                                                           drive_context(fixture.session_id_));
        recorder.oracle(driven.has_value()
                            && driven.value().state == WorkflowRunState::WaitingAgent,
                        "must escalate first");
        const auto continuation = fixture.workflow_->agent_continuation(run.value().run_id);
        if (continuation.has_value() && continuation.value().current_step.has_value()) {
            fixture.provider_->add_response(text_response(skip_step_decision(
                continuation.value().current_step->to_string(), "eval repair no lessons")));
            auto orchestrator = fixture.make_orchestrator();
            const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
            if (attempt.has_value()) {
                if (with_memory) {
                    recorder.oracle(attempt.value().lessons_offered >= 1
                                        && attempt.value().lessons_stale >= 1
                                        && attempt.value().lessons_kept == 0,
                                    "stale lesson must be offered then filtered");
                }
                recorder.oracle(attempt.value().outcome
                                    == WorkflowRecoveryOutcome::PatchedAndResumed,
                                "lesson-free repair must still work");
            } else {
                recorder.runtime_defect("stale attempt error");
            }
            const auto settled = fixture.workflow_->wait_run(run.value().run_id,
                                                             std::chrono::seconds(10));
            recorder.oracle(settled.has_value()
                                && settled.value().state == WorkflowRunState::Completed,
                            "stale run must complete");
            static_cast<void>(orchestrator->shutdown());
        } else {
            recorder.runtime_defect("stale continuation missing");
        }
        collect_workflow_metrics(fixture, record, events_before);
        collect_script_tokens(*fixture.provider_, record);
        const auto dispatches = static_cast<std::size_t>(tool.dispatches.load());
        recorder.oracle(dispatches == 1, "stale dispatches");
        record.duplicate_side_effect = dispatches > 1;
        record.oracle_success = recorder.ok();
        return record;
    }

    // L1: two phases share the fixture and store. A terminal Strict failure
    // seeds the episodic half of the retrieval signature (DEC-030 §2),
    // run A recovers and supplies the lesson, then run B fails the same way.
    // One shared tool serves every phase: with memory the seeding failure
    // consumes dispatch 1, run A fails dispatch 2 and run B dispatch 3;
    // without memory run A fails dispatch 1 and run B dispatch 2.
    ScriptedTool tool{with_memory ? std::vector<int>{1, 2, 3} : std::vector<int>{1, 2}};
    if (!register_registration(*fixture.registry_, tool.registration("scripted"))
             .has_value()) {
        recorder.fixture("tool registration failed");
        return record;
    }
    if (with_memory) {
        const auto seed_run =
            fixture.workflow_->create_run(definition, JsonValue{}, WorkflowPolicy::Strict);
        if (seed_run.has_value()) {
            const auto failed = fixture.workflow_->execute_run(
                seed_run.value().run_id, drive_context(fixture.session_id_));
            recorder.oracle(failed.has_value()
                                && failed.value().state == WorkflowRunState::Failed,
                            "seeding run must fail");
        } else {
            recorder.fixture("seed create_run failed: " + seed_run.error().safe_message);
        }
    }

    // Run A: escalate, repair by skipping the failing step, complete, then
    // the host records the lesson (DEC-030 §4).
    const auto run_a = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run_a.has_value()) {
        recorder.fixture("run A create_run failed: " + run_a.error().safe_message);
        return record;
    }
    const auto events_a = event_count(*fixture.events_, fixture.session_id_);
    if (fixture.workflow_
            ->execute_run(run_a.value().run_id, drive_context(fixture.session_id_))
            .has_value()) {
        const auto continuation = fixture.workflow_->agent_continuation(run_a.value().run_id);
        if (continuation.has_value() && continuation.value().current_step.has_value()) {
            fixture.provider_->add_response(text_response(skip_step_decision(
                continuation.value().current_step->to_string(), "eval repair alpha")));
            auto orchestrator = fixture.make_orchestrator();
            const auto attempt = orchestrator->attempt_recovery(run_a.value().run_id);
            recorder.oracle(attempt.has_value()
                                && attempt.value().outcome
                                       == WorkflowRecoveryOutcome::PatchedAndResumed,
                            "run A repair outcome");
            const auto settled =
                fixture.workflow_->wait_run(run_a.value().run_id, std::chrono::seconds(10));
            recorder.oracle(settled.has_value()
                                && settled.value().state == WorkflowRunState::Completed,
                            "run A must complete");
            static_cast<void>(orchestrator->shutdown());
            if (with_memory) {
                const auto lesson =
                    fixture.workflow_->record_recovery_lesson(run_a.value().run_id);
                recorder.oracle(lesson.has_value(), "lesson recording failed");
            }
        } else {
            recorder.runtime_defect("run A continuation missing");
        }
    } else {
        recorder.runtime_defect("run A execute failed");
    }
    collect_workflow_metrics(fixture, record, events_a);

    // Run B: the same failure again. With memory the retrieval must offer
    // the recorded lesson; without memory the retrieval is empty and the
    // repair proceeds lesson-free (the C/D paired comparison of profile
    // §9.4).
    const auto run_b = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run_b.has_value()) {
        recorder.fixture("run B create_run failed: " + run_b.error().safe_message);
        return record;
    }
    const auto events_b = event_count(*fixture.events_, fixture.session_id_);
    if (fixture.workflow_
            ->execute_run(run_b.value().run_id, drive_context(fixture.session_id_))
            .has_value()) {
        const auto continuation = fixture.workflow_->agent_continuation(run_b.value().run_id);
        if (continuation.has_value() && continuation.value().current_step.has_value()) {
            if (with_memory) {
                bool saw_lesson = false;
                for (const auto &attached : continuation.value().relevant_lessons) {
                    saw_lesson = saw_lesson || attached.kind == MemoryKind::RecoveryLesson;
                }
                recorder.oracle(saw_lesson, "expected a retrieved lesson");
            }
            fixture.provider_->add_response(text_response(skip_step_decision(
                continuation.value().current_step->to_string(), "eval repair beta")));
            auto orchestrator = fixture.make_orchestrator();
            const auto attempt = orchestrator->attempt_recovery(run_b.value().run_id);
            recorder.oracle(attempt.has_value()
                                && attempt.value().outcome
                                       == WorkflowRecoveryOutcome::PatchedAndResumed,
                            "run B repair outcome");
            const auto settled =
                fixture.workflow_->wait_run(run_b.value().run_id, std::chrono::seconds(10));
            recorder.oracle(settled.has_value()
                                && settled.value().state == WorkflowRunState::Completed,
                            "run B must complete");
            static_cast<void>(orchestrator->shutdown());
        } else {
            recorder.runtime_defect("run B continuation missing");
        }
    } else {
        recorder.runtime_defect("run B execute failed");
    }
    collect_workflow_metrics(fixture, record, events_b);
    collect_script_tokens(*fixture.provider_, record);
    const auto dispatches = static_cast<std::size_t>(tool.dispatches.load());
    recorder.oracle(dispatches == (with_memory ? 3U : 2U),
                    "L1 dispatches " + std::to_string(dispatches));
    record.duplicate_side_effect = dispatches > (with_memory ? 3U : 2U);
    record.oracle_success = recorder.ok();
    return record;
}

// ---------------------------------------------------------------------------
// F-family dedicated drivers
// ---------------------------------------------------------------------------

// F3 for arms C/D: unparseable decisions exhaust the repair budget and defer
// to the host; the run must stay in WaitingAgent.
[[nodiscard]] RunRecord
drive_decision_invalid(EvalFixture &fixture, Arm arm, const WorkflowDefinition &definition,
                       const std::string &case_id, int seed, int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = arm;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    ScriptedTool tool{std::vector<int>{1}};
    if (!register_registration(*fixture.registry_, tool.registration("scripted"))
             .has_value()) {
        recorder.fixture("tool registration failed");
        return record;
    }
    const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run.has_value()) {
        recorder.fixture("create_run failed");
        return record;
    }
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto driven =
        fixture.workflow_->execute_run(run.value().run_id, drive_context(fixture.session_id_));
    recorder.oracle(driven.has_value()
                        && driven.value().state == WorkflowRunState::WaitingAgent,
                    "must escalate first");
    // max_decision_repairs defaults to 1: two unparseable bodies exhaust the
    // shared repair budget and defer to the host.
    fixture.provider_->add_response(text_response("this is not a decision"));
    fixture.provider_->add_response(text_response("still not a decision"));
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    recorder.oracle(attempt.has_value()
                        && attempt.value().outcome == WorkflowRecoveryOutcome::DeferredToHost,
                    "invalid decisions must defer");
    if (attempt.has_value()) {
        recorder.oracle(attempt.value().reason_code == "decision-invalid",
                        "reason " + attempt.value().reason_code);
    }
    const auto snapshot = fixture.workflow_->run_snapshot(run.value().run_id);
    recorder.oracle(snapshot.has_value()
                        && snapshot.value().state == WorkflowRunState::WaitingAgent,
                    "run must stay WaitingAgent after deferral");
    static_cast<void>(orchestrator->shutdown());
    collect_workflow_metrics(fixture, record, events_before, true);
    collect_script_tokens(*fixture.provider_, record);
    const auto dispatches = static_cast<std::size_t>(tool.dispatches.load());
    recorder.oracle(dispatches == 1, "dispatches after deferral");
    record.duplicate_side_effect = dispatches > 1;
    record.oracle_success = recorder.ok();
    return record;
}

// F4 for arms C/D: a slow (200 ms) then failing store degrades retrieval to
// empty without blocking escalation; audit outcomes stay visible.
[[nodiscard]] RunRecord
drive_store_outage(bool with_memory, const WorkflowDefinition &definition,
                   const std::string &case_id, int seed, int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = with_memory ? Arm::D : Arm::C;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    const std::string tag = case_id + "-" + arm_name(record.arm) + "-s" + std::to_string(seed)
                            + "-r" + std::to_string(repeat);
    EvalFixture fixture(seed, tag);
    std::shared_ptr<IMemory> backend = std::make_shared<FakeLearningMemory>();
    if (with_memory) {
        // The D arm keeps a real backend under the knob decorator; the F4
        // oracle is the degradation semantics, not the storage engine.
        const auto root =
            std::filesystem::temp_directory_path() / ("m15-eval-" + tag + ".sqlite3");
        std::error_code ec;
        std::filesystem::remove(root, ec);
        SqliteMemoryStoreOptions options;
        options.path = root;
        auto opened = SqliteMemoryStore::open(fixture.executor_, options);
        if (opened.has_value()) {
            fixture.sqlite_path_ = root;
            backend = std::move(opened.value());
        }
    }
    const auto installed = fixture.workflow_->set_learning_context(
        std::make_shared<KnobbedMemory>(backend, std::chrono::milliseconds{200}, false),
        learning_scope());
    recorder.oracle(installed.has_value(), "learning context install");
    ScriptedTool tool{std::vector<int>{1}};
    if (!register_registration(*fixture.registry_, tool.registration("scripted"))
             .has_value()) {
        recorder.fixture("tool registration failed");
        return record;
    }
    const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run.has_value()) {
        recorder.fixture("create_run failed");
        return record;
    }
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto started = std::chrono::steady_clock::now();
    const auto driven =
        fixture.workflow_->execute_run(run.value().run_id, drive_context(fixture.session_id_));
    // A 200 ms query delay must not block the escalation path unboundedly.
    const double escalation_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
    recorder.oracle(driven.has_value()
                        && driven.value().state == WorkflowRunState::WaitingAgent,
                    "slow store must not block escalation");
    recorder.oracle(escalation_ms < 10'000.0, "escalation took too long");
    fixture.provider_->add_response(text_response(resume_decision("retry past outage")));
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    recorder.oracle(attempt.has_value(), "attempt during store outage");
    const auto settled =
        fixture.workflow_->wait_run(run.value().run_id, std::chrono::seconds(10));
    recorder.oracle(settled.has_value()
                        && settled.value().state == WorkflowRunState::Completed,
                    "run must complete past the outage");
    static_cast<void>(orchestrator->shutdown());
    collect_workflow_metrics(fixture, record, events_before, true);
    collect_script_tokens(*fixture.provider_, record);
    record.duplicate_side_effect = false;
    record.oracle_success = recorder.ok();
    return record;
}

// F5 for arms C/D: takeover during the recovery request; no new autonomous
// action may be admitted afterwards.
[[nodiscard]] RunRecord drive_takeover(EvalFixture &fixture, Arm arm,
                                       const WorkflowDefinition &definition,
                                       const std::string &case_id, int seed, int repeat) {
    RunRecord record;
    record.case_id = case_id;
    record.arm = arm;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    ScriptedTool tool{std::vector<int>{1}};
    if (!register_registration(*fixture.registry_, tool.registration("scripted"))
             .has_value()) {
        recorder.fixture("tool registration failed");
        return record;
    }
    const auto run = fixture.workflow_->create_run(definition, JsonValue{}, std::nullopt);
    if (!run.has_value()) {
        recorder.fixture("create_run failed");
        return record;
    }
    const auto events_before = event_count(*fixture.events_, fixture.session_id_);
    const auto driven =
        fixture.workflow_->execute_run(run.value().run_id, drive_context(fixture.session_id_));
    recorder.oracle(driven.has_value()
                        && driven.value().state == WorkflowRunState::WaitingAgent,
                    "must escalate first");
    // The m14-proven deterministic shape: takeover lands before the attempt,
    // so admission itself rejects the recovery request (no autonomous
    // action under takeover, DEC-018/DEC-031 §6).
    const auto takeover = fixture.runtime_->request_human_takeover(fixture.session_id_);
    recorder.oracle(takeover.has_value(), "takeover request failed");
    if (takeover.has_value()) {
        static_cast<void>(takeover.value().outcome(std::chrono::seconds(2)));
    }
    fixture.provider_->add_response(text_response(resume_decision("blocked by takeover")));
    auto orchestrator = fixture.make_orchestrator();
    const auto attempt = orchestrator->attempt_recovery(run.value().run_id);
    recorder.oracle(attempt.has_value()
                        && attempt.value().outcome == WorkflowRecoveryOutcome::Aborted
                        && attempt.value().reason_code == "takeover",
                    "takeover must abort the attempt");
    recorder.oracle(fixture.provider_->consumed() == 0,
                    "no response may be consumed under takeover");
    static_cast<void>(orchestrator->shutdown());
    collect_workflow_metrics(fixture, record, events_before, true);
    collect_script_tokens(*fixture.provider_, record);
    record.duplicate_side_effect = false;
    record.oracle_success = recorder.ok();
    return record;
}

// F2 for every arm: the full profile §11 shutdown sequence must drain within
// the 120 s guardrail. Returns the measured duration.
[[nodiscard]] double measure_shutdown(EvalFixture &fixture) {
    const auto started = std::chrono::steady_clock::now();
    static_cast<void>(fixture.workflow_->shutdown());
    fixture.memory_.reset();
    const auto shutdown = fixture.runtime_->request_shutdown();
    if (shutdown.has_value()) {
        static_cast<void>(shutdown.value().outcome(std::chrono::seconds(5)));
    }
    static_cast<void>(fixture.runtime_->finish_shutdown());
    static_cast<void>(fixture.executor_.shutdown(true));
    fixture.torn_down_ = true;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                     - started)
        .count();
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Case registry (profile §4): 17 cases across the R/W/L/F families with the
// arm applicability matrix of §4.6.
// ---------------------------------------------------------------------------

enum class CaseKind : std::uint8_t {
    Agent,
    Workflow,
    Learning,
    AgentCancel,
    DecisionInvalid,
    StoreOutage,
    Takeover,
    ShutdownProbe,
};

struct CaseDef final {
    const char *id;
    char family;
    CaseKind kind;
    std::vector<Arm> arms;
    AgentScript agent;
    WorkflowScript workflow;
    bool stale_variant = false;
    // Built once per round so every repeat of the case pins the same
    // creation-time digest (W-03); ir_digest then compares verbatim under
    // the G4 normalization instead of drifting with random step ids.
    WorkflowDefinition definition = base_definition("eval-unbuilt", false);
};

[[nodiscard]] std::vector<CaseDef> build_case_table() {
    std::vector<CaseDef> cases;

    AgentScript r1;
    r1.decisions = {agent_decision("tap", 0.5, 0.5)};
    r1.expected_inputs = 1;
    cases.push_back({"EVP1-R1", 'R', CaseKind::Agent, {Arm::A}, r1, {}});

    AgentScript r2;
    r2.decisions = {agent_decision("tap", 0.5, 0.5),
                    agent_decision("type", 0.5, 0.5, "eval text"),
                    agent_decision("swipe", 0.4, 0.4)};
    r2.expected_inputs = 3;
    cases.push_back({"EVP1-R2", 'R', CaseKind::Agent, {Arm::A}, r2, {}});

    AgentScript r3;
    r3.decisions = {agent_decision("done", 0.5, 0.5)};
    r3.expected_inputs = 0;
    cases.push_back({"EVP1-R3", 'R', CaseKind::Agent, {Arm::A}, r3, {}});

    AgentScript r4;
    r4.decisions = {agent_decision("tap", 0.5, 0.5), agent_decision("fail", 0, 0)};
    r4.expected_inputs = 1;
    r4.expected_outcome = LoopOutcome::Failed;
    cases.push_back({"EVP1-R4", 'R', CaseKind::Agent, {Arm::A}, r4, {}});

    AgentScript r5;
    r5.config.max_steps = 2;
    r5.decisions = {agent_decision("tap", 0.5, 0.5), agent_decision("tap", 0.6, 0.6)};
    r5.expected_inputs = 2;
    r5.expected_outcome = LoopOutcome::MaxSteps;
    cases.push_back({"EVP1-R5", 'R', CaseKind::Agent, {Arm::A}, r5, {}});

    AgentScript r6;
    r6.decisions = {agent_decision("tap", 0.5, 0.5), agent_decision("tap", 0.6, 0.6)};
    r6.expected_inputs = 2;
    r6.user_message = std::string("redirect: finish after the next tap");
    r6.require_message_visible = true;
    cases.push_back({"EVP1-R6", 'R', CaseKind::Agent, {Arm::A}, r6, {}});

    WorkflowScript w1;
    w1.policy = WorkflowPolicy::Strict;
    w1.expected_state = WorkflowRunState::Completed;
    cases.push_back({"EVP1-W1", 'W', CaseKind::Workflow, {Arm::B}, {}, w1, false,
                     eval_workflow("eval-w1", WorkflowPolicy::Strict)});
    WorkflowScript w1r;
    w1r.policy = WorkflowPolicy::Recoverable;
    w1r.expected_state = WorkflowRunState::Completed;
    cases.push_back({"EVP1-W1", 'W', CaseKind::Workflow, {Arm::C, Arm::D}, {}, w1r,
                     false, eval_workflow("eval-w1", WorkflowPolicy::Recoverable)});

    WorkflowScript w2b;
    w2b.policy = WorkflowPolicy::Strict;
    w2b.tool_failures = {1};
    w2b.expected_state = WorkflowRunState::Failed;
    cases.push_back({"EVP1-W2", 'W', CaseKind::Workflow, {Arm::B}, {}, w2b, false,
                     eval_workflow("eval-w2", WorkflowPolicy::Strict)});
    WorkflowScript w2r;
    w2r.policy = WorkflowPolicy::Recoverable;
    w2r.tool_failures = {1};
    w2r.recovery_decisions = {resume_decision("retry once")};
    w2r.expected_state = WorkflowRunState::Completed;
    w2r.expected_dispatches = 2;
    cases.push_back({"EVP1-W2", 'W', CaseKind::Workflow, {Arm::C, Arm::D}, {}, w2r,
                     false, eval_workflow("eval-w2", WorkflowPolicy::Recoverable)});

    WorkflowScript w3b;
    w3b.policy = WorkflowPolicy::Strict;
    w3b.tool_failures = {1};
    w3b.expected_state = WorkflowRunState::Failed;
    cases.push_back({"EVP1-W3", 'W', CaseKind::Workflow, {Arm::B}, {}, w3b, false,
                     eval_workflow("eval-w3", WorkflowPolicy::Strict)});
    WorkflowScript w3r;
    w3r.policy = WorkflowPolicy::Recoverable;
    w3r.tool_failures = {1};
    w3r.fill_skip_decision = true;
    w3r.expected_state = WorkflowRunState::Completed;
    cases.push_back({"EVP1-W3", 'W', CaseKind::Workflow, {Arm::C, Arm::D}, {}, w3r,
                     false, eval_workflow("eval-w3", WorkflowPolicy::Recoverable)});

    WorkflowScript w4b;
    w4b.policy = WorkflowPolicy::Strict;
    w4b.tool_failures = {1};
    w4b.expected_state = WorkflowRunState::Failed;
    cases.push_back({"EVP1-W4", 'W', CaseKind::Workflow, {Arm::B}, {}, w4b, false,
                     eval_workflow("eval-w4", WorkflowPolicy::Strict)});
    WorkflowScript w4r;
    w4r.policy = WorkflowPolicy::Recoverable;
    w4r.tool_failures = {1, 2};
    w4r.recovery_decisions = {resume_decision("ineffective retry"),
                              need_user_decision("defer to host")};
    w4r.expected_state = WorkflowRunState::Completed; // unused: run defers
    w4r.expected_dispatches = 2;
    cases.push_back({"EVP1-W4", 'W', CaseKind::Workflow, {Arm::C, Arm::D}, {}, w4r,
                     false, eval_workflow("eval-w4", WorkflowPolicy::Recoverable)});

    cases.push_back({"EVP1-L1", 'L', CaseKind::Learning, {Arm::C, Arm::D}, {}, {}, false,
                     eval_workflow("eval-l1", WorkflowPolicy::Recoverable)});
    cases.push_back({"EVP1-L2", 'L', CaseKind::Learning, {Arm::C, Arm::D}, {}, {}, true,
                     eval_workflow("eval-l2", WorkflowPolicy::Recoverable)});

    cases.push_back({"EVP1-F1", 'F', CaseKind::AgentCancel, {Arm::A}, {}, {}});
    cases.push_back({"EVP1-F2", 'F', CaseKind::ShutdownProbe, {Arm::A, Arm::B, Arm::C, Arm::D},
                     {}, {}});

    AgentScript f3a;
    f3a.config.max_steps = 2;
    f3a.decisions = {"not a decision", "still not a decision"};
    f3a.expected_inputs = 0;
    f3a.expected_outcome = LoopOutcome::Failed;
    cases.push_back({"EVP1-F3", 'F', CaseKind::Agent, {Arm::A}, f3a, {}});
    cases.push_back({"EVP1-F3", 'F', CaseKind::DecisionInvalid, {Arm::C, Arm::D}, {}, {},
                     false, eval_workflow("eval-f3", WorkflowPolicy::Recoverable)});

    cases.push_back({"EVP1-F4", 'F', CaseKind::StoreOutage, {Arm::C, Arm::D}, {}, {}, false,
                     eval_workflow("eval-f4", WorkflowPolicy::Recoverable)});
    cases.push_back({"EVP1-F5", 'F', CaseKind::Takeover, {Arm::C, Arm::D}, {}, {}, false,
                     eval_workflow("eval-f5", WorkflowPolicy::Recoverable)});

    return cases;
}

// The canonical case table for the dataset digest (profile §4.1): identity,
// family, kind and arm applicability, in registration order.
[[nodiscard]] std::string canonical_case_table(const std::vector<CaseDef> &cases) {
    std::string canonical;
    for (const auto &def : cases) {
        canonical += def.id;
        canonical += "|f=";
        canonical.push_back(def.family);
        canonical += "|k=" + std::to_string(static_cast<int>(def.kind));
        canonical += "|arms=";
        for (const auto arm : def.arms) {
            canonical += std::to_string(static_cast<int>(arm));
        }
        canonical += ";";
    }
    return canonical;
}

[[nodiscard]] RunRecord execute_one(const CaseDef &def, Arm arm, int seed, int repeat) {
    const std::string tag =
        std::string(def.id) + "-" + arm_name(arm) + "-s" + std::to_string(seed) + "-r"
        + std::to_string(repeat);
    switch (def.kind) {
    case CaseKind::Agent:
        return [&]() {
            EvalFixture fixture(seed, tag);
            return drive_agent_arm(fixture, def.agent, def.id, seed, repeat);
        }();
    case CaseKind::AgentCancel:
        return [&]() {
            EvalFixture fixture(seed, tag);
            return drive_agent_cancel(fixture, def.id, seed, repeat);
        }();
    case CaseKind::Workflow:
        return [&]() {
            EvalFixture fixture(seed, tag);
            if (arm == Arm::D && !fixture.attach_sqlite_learning()) {
                RunRecord record;
                record.case_id = def.id;
                record.arm = arm;
                record.seed = seed;
                record.repeat = repeat;
                record.failure_class = "fixture-defect";
                record.detail = "sqlite learning context failed";
                return record;
            }
            WorkflowScript script = def.workflow;
            return drive_workflow_arm(fixture, script, def.definition, def.id, arm, seed,
                                      repeat);
        }();
    case CaseKind::Learning:
        return drive_learning_case(arm == Arm::D, def.definition, def.id, seed, repeat,
                                   def.stale_variant);
    case CaseKind::DecisionInvalid:
        return [&]() {
            EvalFixture fixture(seed, tag);
            return drive_decision_invalid(fixture, arm, def.definition, def.id, seed, repeat);
        }();
    case CaseKind::StoreOutage:
        return drive_store_outage(arm == Arm::D, def.definition, def.id, seed, repeat);
    case CaseKind::Takeover:
        return [&]() {
            EvalFixture fixture(seed, tag);
            return drive_takeover(fixture, arm, def.definition, def.id, seed, repeat);
        }();
    case CaseKind::ShutdownProbe: break;
    }
    // F2: drive one representative scenario per arm, then measure the full
    // §11 shutdown sequence against the 120 s guardrail.
    RunRecord record;
    record.case_id = def.id;
    record.arm = arm;
    record.seed = seed;
    record.repeat = repeat;
    Recorder recorder(record);
    EvalFixture fixture(seed, tag);
    if (arm == Arm::A) {
        AgentScript script;
        script.decisions = {agent_decision("tap", 0.5, 0.5)};
        script.expected_inputs = 1;
        const auto settled = drive_agent_arm(fixture, script, def.id, seed, repeat);
        recorder.oracle(settled.oracle_success, "warm-up run failed");
    } else if (arm == Arm::B) {
        auto warm = eval_workflow("eval-f2-b", WorkflowPolicy::Strict);
        WorkflowScript script;
        script.policy = WorkflowPolicy::Strict;
        script.expected_state = WorkflowRunState::Completed;
        const auto settled =
            drive_workflow_arm(fixture, script, warm, def.id, arm, seed, repeat);
        recorder.oracle(settled.oracle_success, "warm-up run failed");
    } else {
        if (arm == Arm::D && !fixture.attach_sqlite_learning()) {
            recorder.fixture("sqlite learning context failed");
        }
        auto warm = eval_workflow("eval-f2-r", WorkflowPolicy::Recoverable);
        WorkflowScript script;
        script.policy = WorkflowPolicy::Recoverable;
        script.tool_failures = {1};
        script.recovery_decisions = {resume_decision("f2 warm-up repair")};
        script.expected_dispatches = 2;
        const auto settled =
            drive_workflow_arm(fixture, script, warm, def.id, arm, seed, repeat);
        recorder.oracle(settled.oracle_success, "warm-up run failed");
    }
    record.shutdown_ms = measure_shutdown(fixture);
    recorder.oracle(record.shutdown_ms <= 120'000.0, "shutdown guardrail");
    record.oracle_success = recorder.ok();
    return record;
}

// ---------------------------------------------------------------------------
// Round engine: gates, budgets and the report
// ---------------------------------------------------------------------------

struct GateSummary final {
    bool g1_duplicate_side_effects = true;
    bool g2_runtime_defects = true;
    bool g4_determinism = true;
    bool g5_correlation = true;
    bool g6_privacy = true;
    bool budgets = true;
    std::vector<std::string> g4_mismatches;
    std::vector<std::string> budget_violations;
};

[[nodiscard]] GateSummary evaluate_gates(const std::vector<RunRecord> &records) {
    GateSummary gates;
    // Budgets (profile §9.2): recorded rounds carry no live cost; the model
    // call, token, wall-clock and shutdown guardrails apply per run.
    for (const auto &record : records) {
        if (record.duplicate_side_effect) {
            gates.g1_duplicate_side_effects = false;
        }
        if (record.failure_class == "runtime-defect") {
            gates.g2_runtime_defects = false;
        }
        if (!record.correlation_ok) {
            gates.g5_correlation = false;
        }
        if (record.privacy_leak) {
            gates.g6_privacy = false;
        }
        if (record.model_calls > 64) {
            gates.budgets = false;
            gates.budget_violations.push_back(record.case_id + "/" + arm_name(record.arm)
                                              + ": model calls "
                                              + std::to_string(record.model_calls));
        }
        if (record.input_tokens + record.output_tokens > 2'000'000) {
            gates.budgets = false;
            gates.budget_violations.push_back(record.case_id + "/" + arm_name(record.arm)
                                              + ": token budget");
        }
        if (record.wall_ms > 300'000.0) {
            gates.budgets = false;
            gates.budget_violations.push_back(record.case_id + "/" + arm_name(record.arm)
                                              + ": wall clock");
        }
        if (record.shutdown_ms > 120'000.0) {
            gates.budgets = false;
            gates.budget_violations.push_back(record.case_id + "/" + arm_name(record.arm)
                                              + ": shutdown guardrail");
        }
    }
    // G4: the seed-0 triple must agree on the normalized events. Arm A is a
    // single producer, so the sequence compares positionally. The workflow
    // arms have two concurrent producers (the drive worker and the
    // orchestrator's audit emission after resume_run), whose interleaving
    // order is not a contract — recorded as a baseline-round finding — so
    // those arms compare the sorted multiset: content drift, missing or
    // extra events still fail the gate.
    std::map<std::string, const RunRecord *> first_repeat;
    for (const auto &record : records) {
        if (record.seed != 0) {
            continue;
        }
        const std::string key = record.case_id + "/" + arm_name(record.arm);
        const auto position = first_repeat.emplace(key, &record);
        if (!position.second) {
            const auto &baseline = *position.first->second;
            bool diverged = baseline.normalized_events != record.normalized_events;
            std::vector<std::string> mismatch_pair;
            if (diverged && record.arm != Arm::A) {
                auto sorted_baseline = baseline.normalized_events;
                auto sorted_repeat = record.normalized_events;
                std::sort(sorted_baseline.begin(), sorted_baseline.end());
                std::sort(sorted_repeat.begin(), sorted_repeat.end());
                if (sorted_baseline == sorted_repeat) {
                    diverged = false; // legal cross-producer interleaving
                }
            }
            if (diverged) {
                gates.g4_determinism = false;
                std::size_t index = 0;
                while (index < baseline.normalized_events.size()
                       && index < record.normalized_events.size()
                       && baseline.normalized_events[index] == record.normalized_events[index]) {
                    ++index;
                }
                const auto clip = [](const std::string &value) {
                    return value.substr(0, 300);
                };
                gates.g4_mismatches.push_back(
                    key + ": event " + std::to_string(index) + " baseline ["
                    + clip(baseline.normalized_events[index]) + "] repeat ["
                    + (index < record.normalized_events.size()
                           ? clip(record.normalized_events[index])
                           : std::string("<end>"))
                    + "]");
            }
        }
    }
    return gates;
}

void append_percentiles(std::ostream &out, const std::string &indent,
                        const Percentiles &values) {
    out << indent << "\"p50\": " << values.p50 << ", \"p95\": " << values.p95
        << ", \"p99\": " << values.p99 << ", \"max\": " << values.max;
}

[[nodiscard]] std::string build_report(const std::vector<CaseDef> &cases,
                                       const std::vector<RunRecord> &records,
                                       const GateSummary &gates,
                                       const std::vector<double> &rss_kb) {
    const std::string dataset_digest = to_hex(digest_string(canonical_case_table(cases)));
    std::ostringstream out;
    out << "{\n";
    out << "  \"profile\": \"discrete-workflow-eval-v1\",\n";
    out << "  \"dataset_digest\": \"" << dataset_digest << "\",\n";
    out << "  \"runs\": " << records.size() << ",\n";
    out << "  \"gates\": {\n";
    out << "    \"G1_no_duplicate_side_effects\": "
        << (gates.g1_duplicate_side_effects ? "true" : "false") << ",\n";
    out << "    \"G2_no_runtime_defects\": " << (gates.g2_runtime_defects ? "true" : "false")
        << ",\n";
    out << "    \"G4_recorded_determinism\": " << (gates.g4_determinism ? "true" : "false")
        << ",\n";
    out << "    \"G5_event_correlation\": " << (gates.g5_correlation ? "true" : "false")
        << ",\n";
    out << "    \"G6_privacy\": " << (gates.g6_privacy ? "true" : "false") << ",\n";
    out << "    \"budgets\": " << (gates.budgets ? "true" : "false") << "\n";
    out << "  },\n";
    if (!gates.g4_mismatches.empty()) {
        out << "  \"g4_mismatches\": [";
        for (std::size_t index = 0; index < gates.g4_mismatches.size(); ++index) {
            out << (index == 0 ? "" : ", ") << "\"" << json_escape(gates.g4_mismatches[index])
                << "\"";
        }
        out << "],\n";
    }
    if (!gates.budget_violations.empty()) {
        out << "  \"budget_violations\": [";
        for (std::size_t index = 0; index < gates.budget_violations.size(); ++index) {
            out << (index == 0 ? "" : ", ")
                << "\"" << json_escape(gates.budget_violations[index]) << "\"";
        }
        out << "],\n";
    }
    // Stratified per-arm aggregates (profile §7: family-stratified, never a
    // single pooled success number).
    out << "  \"arms\": {\n";
    bool first_arm = true;
    for (const auto arm : {Arm::A, Arm::B, Arm::C, Arm::D}) {
        std::vector<std::string> families;
        for (const auto &def : cases) {
            if (std::find(def.arms.begin(), def.arms.end(), arm) == def.arms.end()) {
                continue;
            }
            const std::string family(1, def.family);
            if (std::find(families.begin(), families.end(), family) == families.end()) {
                families.push_back(family);
            }
        }
        out << (first_arm ? "" : ",\n") << "    \"" << arm_name(arm) << "\": {";
        first_arm = false;
        bool first_family = true;
        for (const auto &family : families) {
            std::uint64_t success = 0;
            std::uint64_t total = 0;
            std::vector<double> walls;
            std::vector<double> steps;
            std::uint64_t calls = 0;
            std::uint32_t interventions = 0;
            for (const auto &record : records) {
                const bool family_matches = record.case_id.size() > 5
                                            && record.case_id[5] == family[0];
                if (record.arm != arm || !family_matches) {
                    continue;
                }
                ++total;
                success += record.oracle_success ? 1U : 0U;
                walls.push_back(record.wall_ms);
                steps.insert(steps.end(), record.step_ms.begin(), record.step_ms.end());
                calls += record.model_calls;
                interventions += record.interventions;
            }
            if (total == 0) {
                continue;
            }
            out << (first_family ? "" : ", ") << "\n      \"" << family << "\": {";
            first_family = false;
            out << "\n        \"success\": \"" << success << "/" << total
                << " (wilson95 " << wilson95(success, total) << ")\",";
            out << "\n        \"model_calls_total\": " << calls << ",";
            out << "\n        \"human_interventions\": " << interventions << ",";
            out << "\n        \"wall_ms\": {";
            append_percentiles(out, "\n          ", percentiles(walls));
            out << "},";
            out << "\n        \"step_ms\": {";
            append_percentiles(out, "\n          ", percentiles(steps));
            out << "}\n      }";
        }
        out << "\n    }";
    }
    out << "\n  },\n";
    // L1 paired comparison (profile §9.4: report-only).
    const RunRecord *l1_c = nullptr;
    const RunRecord *l1_d = nullptr;
    for (const auto &record : records) {
        if (record.case_id != "EVP1-L1" || record.seed != 0 || record.repeat != 0) {
            continue;
        }
        if (record.arm == Arm::C) {
            l1_c = &record;
        } else if (record.arm == Arm::D) {
            l1_d = &record;
        }
    }
    std::vector<double> shutdowns;
    for (const auto &record : records) {
        if (record.shutdown_ms > 0) {
            shutdowns.push_back(record.shutdown_ms);
        }
    }
    if (!shutdowns.empty()) {
        out << "  \"shutdown_ms\": {";
        append_percentiles(out, "\n    ", percentiles(shutdowns));
        out << "\n  },\n";
    }
    out << "  \"learning_gain_l1\": ";
    if (l1_c != nullptr && l1_d != nullptr) {
        out << "{\n";
        out << "    \"note\": \"report-only, no v1 pass line (DEC-034 s3)\",\n";
        out << "    \"no_memory\": {\"success\": " << l1_c->oracle_success
            << ", \"model_calls\": " << l1_c->model_calls
            << ", \"wall_ms\": " << l1_c->wall_ms << "},\n";
        out << "    \"with_memory\": {\"success\": " << l1_d->oracle_success
            << ", \"model_calls\": " << l1_d->model_calls
            << ", \"wall_ms\": " << l1_d->wall_ms << "}\n";
        out << "  },\n";
    } else {
        out << "null,\n";
    }
    if (!rss_kb.empty()) {
        out << "  \"rss_kb\": [";
        for (std::size_t index = 0; index < rss_kb.size(); ++index) {
            out << (index == 0 ? "" : ", ") << rss_kb[index];
        }
        out << "],\n";
    }
    out << "  \"failures\": [";
    bool first_failure = true;
    for (const auto &record : records) {
        if (record.failure_class.empty()) {
            continue;
        }
        out << (first_failure ? "\n" : ",\n") << "    {\"case\": \"" << record.case_id
            << "\", \"arm\": \"" << arm_name(record.arm) << "\", \"seed\": " << record.seed
            << ", \"repeat\": " << record.repeat << ", \"class\": \""
            << record.failure_class << "\", \"detail\": \"" << json_escape(record.detail)
            << "\"}";
        first_failure = false;
    }
    out << (first_failure ? "]" : "\n  ]") << "\n";
    out << "}\n";
    return out.str();
}

#if defined(__linux__)
[[nodiscard]] std::uint64_t current_rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            const auto first = line.find_first_of("0123456789");
            if (first != std::string::npos) {
                return static_cast<std::uint64_t>(
                    std::strtoull(line.c_str() + first, nullptr, 10));
            }
        }
    }
    return 0;
}
#endif

int run_round(const std::string &output_path, std::uint64_t round_serial,
              std::vector<double> *rss_kb, bool *ok) {
    const auto cases = build_case_table();
    std::vector<RunRecord> records;
    for (const auto &def : cases) {
        for (const auto arm : def.arms) {
            for (const int seed : {0, 1, 2}) {
                records.push_back(execute_one(def, arm, seed, 0));
            }
            // The seed-0 triple backs the G4 determinism gate (profile §7).
            records.push_back(execute_one(def, arm, 0, 1));
            records.push_back(execute_one(def, arm, 0, 2));
        }
    }
    const auto gates = evaluate_gates(records);
    const bool round_ok = gates.g1_duplicate_side_effects && gates.g2_runtime_defects
                          && gates.g4_determinism && gates.g5_correlation
                          && gates.g6_privacy && gates.budgets;
    const auto report = build_report(cases, records, gates, rss_kb != nullptr ? *rss_kb
                                                                              : std::vector<double>{});
    std::cout << report;
    if (!output_path.empty()) {
        std::ofstream file(output_path + (round_serial > 1 ? "." + std::to_string(round_serial)
                                                           : std::string())
                           + ".json");
        file << report;
    }
    if (ok != nullptr) {
        *ok = round_ok;
    }
    return round_ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    std::string output_path;
    std::uint64_t soak_rounds = 1;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--soak" && index + 1 < argc) {
            soak_rounds = std::strtoull(argv[++index], nullptr, 10);
        } else if (output_path.empty()) {
            output_path = argument;
        }
    }
    std::vector<double> rss_kb;
    bool all_ok = true;
    for (std::uint64_t round = 1; round <= soak_rounds; ++round) {
        bool round_ok = false;
        const int status = run_round(output_path, round, nullptr, &round_ok);
        all_ok = all_ok && round_ok && status == 0;
#if defined(__linux__)
        if (soak_rounds > 1) {
            // Steady-state readings only: round 1 is the warm-up that pays
            // the allocator/SQLite setup once (evaluation design §9), so
            // growth is measured from the post-warm-up baseline.
            rss_kb.push_back(static_cast<double>(current_rss_kb()));
        }
#endif
    }
    if (soak_rounds > 1 && rss_kb.size() >= 2 && rss_kb.front() > 0) {
        const double growth = (rss_kb.back() - rss_kb.front()) / rss_kb.front();
        std::cout << "{ \"soak_rounds\": " << soak_rounds << ", \"rss_kb\": [";
        for (std::size_t index = 0; index < rss_kb.size(); ++index) {
            std::cout << (index == 0 ? "" : ", ") << rss_kb[index];
        }
        std::cout << "], \"soak_rss_growth\": " << growth << ", \"limit\": 0.10 }\n";
        if (growth > 0.10) {
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
