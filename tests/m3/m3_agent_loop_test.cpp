#include "support/m3_support.hpp"

#include <executor/executor.hpp>
#include "support/test.hpp"

#include <mira/agent_loop.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/model_gateway.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace {

using namespace mira;
using namespace mira::adapters::simulator;
using namespace mira::testing;

class LoopFixture final {
  public:
    LoopFixture() {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        executor_.initialize(config);
        // The environment exists before anything derives an artifact source
        // from it.
        environment_ = std::make_shared<SimulatorEnvironment>(SimulatorSetup::single_display());
        transport_ = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                 ModelGatewayConfig{});
        gateway_->register_provider(std::make_shared<OpenAiCompatibleProvider>(
            profile_, transport_, artifact_source()));
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);
    }
    ~LoopFixture() { (void)executor_.shutdown(true); }

    void activate(std::uint64_t epoch = 1) { admission_->activate(spec_.task_id, epoch); }

    [[nodiscard]] AgentLoopSpec &spec() { return spec_; }

    // Serves the simulator's published screenshot artifacts to the wire
    // mapper, exactly as a production host bridges its ArtifactStore.
    class SimulatorArtifactSource final : public IArtifactSource {
      public:
        explicit SimulatorArtifactSource(SimulatorEnvironment &environment)
            : environment_(environment) {}
        Result<std::vector<std::byte>> fetch(const ArtifactRef &reference) override {
            std::vector<std::byte> bytes;
            auto opened = environment_.open_artifact(reference.id, bytes);
            if (!opened) {
                return opened.error();
            }
            return bytes;
        }

      private:
        SimulatorEnvironment &environment_;
    };

    executor::Executor executor_;
    std::shared_ptr<MockHttpTransport> transport_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<SimulatorEnvironment> environment_;

    [[nodiscard]] std::shared_ptr<IArtifactSource> artifact_source() {
        return std::make_shared<SimulatorArtifactSource>(*environment_);
    }
    AgentLoopSpec spec_ = [] {
        AgentLoopSpec spec;
        spec.task_id = TaskId::generate();
        spec.session_id = SessionId::generate();
        spec.task_epoch = 1;
        spec.goal = "tap the target";
        return spec;
    }();
};

class NeverSatisfied final : public ILoopVerifier {
  public:
    Verdict verify(const Observation &, const DecisionCandidate &) override {
        return Verdict::NotSatisfied;
    }
};

[[nodiscard]] OperationContext loop_context(std::function<bool()> cancelled = nullptr) {
    OperationContext context;
    context.session = SessionId::generate();
    context.task = TaskId::generate();
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.cancellation_requested = std::move(cancelled);
    return context;
}

// Verifies that the simulator actually executed at least one input before
// accepting the model's done claim.
class InputCountVerifier final : public ILoopVerifier {
  public:
    explicit InputCountVerifier(SimulatorEnvironment &environment,
                                std::size_t required_inputs = 1)
        : environment_(environment), required_(required_inputs) {}
    Verdict verify(const Observation &, const DecisionCandidate &decision) override {
        const auto action = decision.value.find("action");
        if (action == nullptr || !action->is_string() || *action->as_string() != "done") {
            return Verdict::NotSatisfied;
        }
        return environment_.executed_inputs().size() >= required_ ? Verdict::Satisfied
                                                                  : Verdict::NotSatisfied;
    }

  private:
    SimulatorEnvironment &environment_;
    std::size_t required_;
};

int successful_two_step_loop() {
    LoopFixture fixture;
    fixture.activate();
    fixture.spec().profile_id = fixture.profile_->id;

    // Step 1: tap; step 2: done (after verification sees one executed input).
    fixture.transport_->enqueue_json(200, R"({"id":"r1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"tap\",\"x\":0.4,\"y\":0.6,\"reason\":\"first\"}"}]}],"usage":{"input_tokens":8,"output_tokens":3}})");
    fixture.transport_->enqueue_json(200, R"({"id":"r2","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"done\",\"reason\":\"goal achieved\"}"}]}],"usage":{"input_tokens":8,"output_tokens":2}})");

    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{8, 1});
    InputCountVerifier verifier(*fixture.environment_, 1);
    auto result = loop.run(fixture.spec(), loop_context(), verifier);
    if (!result.has_value() || result.value().outcome != LoopOutcome::Completed) {
        std::cerr << "loop outcome="
                  << (result.has_value() ? loop_outcome_name(result.value().outcome)
                                         : std::string("<error>") + result.error().safe_message)
                  << " summary=" << (result.has_value() ? result.value().safe_summary : "-")
                  << " steps=" << (result.has_value() ? result.value().steps.size() : 0) << "\n";
    }
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(result.value().steps.size() >= 2);
    MIRA_CHECK(fixture.environment_->executed_inputs().size() == 1);
    MIRA_CHECK(fixture.environment_->executed_inputs()[0].events[0].kind == "tap");
    MIRA_CHECK(fixture.transport_->recorded().size() == 2);
    // The wire request carried the screenshot artifact reference.
    MIRA_CHECK(fixture.transport_->recorded()[0].body.find("input_image") != std::string::npos);
    // Every request carries the strict decision schema and explicit store.
    for (const auto &recorded : fixture.transport_->recorded()) {
        MIRA_CHECK(recorded.body.find("\"store\":false") != std::string::npos);
        MIRA_CHECK(recorded.body.find("json_schema") != std::string::npos);
    }
    return 0;
}

int refused_and_failed_paths() {
    {
        LoopFixture fixture;
        fixture.activate();
        fixture.spec().profile_id = fixture.profile_->id;
        fixture.transport_->enqueue_json(200, R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"refusal","refusal":"no"}]}],"usage":{}})");
        AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 1});
        ModelDoneVerifier verifier;
        auto result = loop.run(fixture.spec(), loop_context(), verifier);
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
        MIRA_CHECK(fixture.environment_->executed_inputs().empty());
    }
    {
        LoopFixture fixture;
        fixture.activate();
        fixture.spec().profile_id = fixture.profile_->id;
        // Model declares explicit failure.
        fixture.transport_->enqueue_json(200, R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"fail\",\"reason\":\"impossible\"}"}]}],"usage":{}})");
        AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 1});
        ModelDoneVerifier verifier;
        auto result = loop.run(fixture.spec(), loop_context(), verifier);
        MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
        MIRA_CHECK(result.value().safe_summary.find("impossible") != std::string::npos);
    }
    {
        LoopFixture fixture;
        fixture.activate();
        fixture.spec().profile_id = fixture.profile_->id;
        // Incomplete output, no recovery left after the budget.
        MockStep step;
        step.status = 200;
        step.body = R"({"id":"r","status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[],"usage":{}})";
        fixture.transport_->enqueue(std::move(step));
        fixture.transport_->enqueue(std::move(step));
        AgentLoopConfig config;
        config.max_steps = 2;
        config.max_recoveries_per_step = 1;
        AgentLoop loop(fixture.environment_, *fixture.gateway_, config);
        ModelDoneVerifier verifier;
        auto result = loop.run(fixture.spec(), loop_context(), verifier);
        MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
    }
    return 0;
}

int malformed_decision_triggers_bounded_repair() {
    LoopFixture fixture;
    fixture.activate();
    fixture.spec().profile_id = fixture.profile_->id;
    // First response is completed but schema-invalid; the repair request
    // yields a valid decision.
    fixture.transport_->enqueue_json(200, R"({"id":"r1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"detonate\",\"reason\":\"typo\"}"}]}],"usage":{"input_tokens":5,"output_tokens":2}})");
    fixture.transport_->enqueue_json(200, R"({"id":"r2","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"done\",\"reason\":\"repaired\"}"}]}],"usage":{"input_tokens":5,"output_tokens":2}})");

    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 1});
    InputCountVerifier verifier(*fixture.environment_, 0);
    auto result = loop.run(fixture.spec(), loop_context(), verifier);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(result.value().repairs == 1);
    MIRA_CHECK(fixture.transport_->recorded().size() == 2);
    // The repair request quotes a validation summary, not the raw output.
    MIRA_CHECK(fixture.transport_->recorded()[1].body.find("detonate") == std::string::npos);
    return 0;
}

int max_steps_and_verification_disagreement() {
    {
        LoopFixture fixture;
        fixture.activate();
        fixture.spec().profile_id = fixture.profile_->id;
        for (int index = 0; index < 3; ++index) {
            fixture.transport_->enqueue_json(200, R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"swipe\",\"x\":0.1,\"y\":0.1,\"end_x\":0.2,\"end_y\":0.2,\"reason\":\"r\"}"}]}],"usage":{}})");
        }
        AgentLoopConfig config;
        config.max_steps = 3;
        AgentLoop loop(fixture.environment_, *fixture.gateway_, config);
        NeverSatisfied verifier;
        auto result = loop.run(fixture.spec(), loop_context(), verifier);
        MIRA_CHECK(result.value().outcome == LoopOutcome::MaxSteps);
        MIRA_CHECK(fixture.environment_->executed_inputs().size() == 3);
    }
    {
        // The model claims done; verification disagrees; the loop continues
        // and the eventual done verifies.
        LoopFixture fixture;
        fixture.activate();
        fixture.spec().profile_id = fixture.profile_->id;
        fixture.transport_->enqueue_json(200, R"({"id":"r1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"done\",\"reason\":\"premature\"}"}]}],"usage":{}})");
        fixture.transport_->enqueue_json(200, R"({"id":"r2","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"tap\",\"x\":0.2,\"y\":0.2,\"reason\":\"again\"}"}]}],"usage":{}})");
        fixture.transport_->enqueue_json(200, R"({"id":"r3","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"done\",\"reason\":\"now\"}"}]}],"usage":{}})");

        class OnceVerifier final : public ILoopVerifier {
          public:
            Verdict verify(const Observation &, const DecisionCandidate &decision) override {
                const auto action = decision.value.find("action");
                if (action == nullptr || !action->is_string() || *action->as_string() != "done") {
                    return Verdict::NotSatisfied;
                }
                return ++done_claims_ >= 2 ? Verdict::Satisfied : Verdict::NotSatisfied;
            }
          private:
            int done_claims_ = 0;
        };
        AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{6, 1});
        OnceVerifier verifier;
        auto result = loop.run(fixture.spec(), loop_context(), verifier);
        MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
        MIRA_CHECK(fixture.environment_->executed_inputs().size() == 1);
    }
    return 0;
}

int cancellation_stops_before_next_action() {
    LoopFixture fixture;
    fixture.activate();
    fixture.spec().profile_id = fixture.profile_->id;

    std::atomic<bool> cancel{false};
    // First decision executes; then cancellation is observed.
    fixture.transport_->enqueue_json(200, R"({"id":"r1","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"tap\",\"x\":0.3,\"y\":0.3,\"reason\":\"r\"}"}]}],"usage":{}})");
    fixture.transport_->enqueue_json(200, R"({"id":"r2","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"tap\",\"x\":0.7,\"y\":0.7,\"reason\":\"never\"}"}]}],"usage":{}})");

    class CancelAfterFirstAction final : public ILoopVerifier {
      public:
        explicit CancelAfterFirstAction(std::atomic<bool> &flag) : flag_(flag) {}
        Verdict verify(const Observation &, const DecisionCandidate &) override {
            if (flag_.load()) {
                return Verdict::NotSatisfied;
            }
            flag_.store(true);
            return Verdict::NotSatisfied;
        }
      private:
        std::atomic<bool> &flag_;
    };

    CancelAfterFirstAction verifier(cancel);
    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{6, 1});
    auto context = loop_context([&cancel] { return cancel.load(); });
    auto result = loop.run(fixture.spec(), context, verifier);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Cancelled);
    // Exactly one action executed; the cancelled step dispatched nothing.
    MIRA_CHECK(fixture.environment_->executed_inputs().size() == 1);
    return 0;
}

int admission_rejection_prevents_actions() {
    LoopFixture fixture;
    // The gate stays inactive: no request may even leave.
    fixture.spec().profile_id = fixture.profile_->id;
    fixture.transport_->enqueue_json(200, R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"tap\",\"x\":0.5,\"y\":0.5,\"reason\":\"r\"}"}]}],"usage":{}})");
    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 1});
    ModelDoneVerifier verifier;
    auto result = loop.run(fixture.spec(), loop_context(), verifier);
    MIRA_CHECK(result.has_value());
    // An unadmitted epoch means the task is cancelled or taken over; the
    // loop must stop without dispatching anything.
    MIRA_CHECK(result.value().outcome == LoopOutcome::Cancelled);
    MIRA_CHECK(fixture.environment_->executed_inputs().empty());
    MIRA_CHECK(fixture.transport_->recorded().empty());
    return 0;
}

int rate_limit_recovery_inside_the_loop() {
    LoopFixture fixture;
    fixture.activate();
    fixture.spec().profile_id = fixture.profile_->id;

    MockStep limited;
    limited.status = 429;
    limited.headers = {{"Retry-After", "0"}};
    limited.body = R"({"error":{"code":"rate_limited"}})";
    fixture.transport_->enqueue(std::move(limited));
    fixture.transport_->enqueue_json(200, R"({"id":"r","status":"completed","model":"m","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"action\":\"done\",\"reason\":\"after retry\"}"}]}],"usage":{"input_tokens":3,"output_tokens":2}})");

    AgentLoop loop(fixture.environment_, *fixture.gateway_, AgentLoopConfig{4, 2});
    InputCountVerifier verifier(*fixture.environment_, 0);
    auto result = loop.run(fixture.spec(), loop_context(), verifier);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(fixture.transport_->recorded().size() == 2);
    return 0;
}

} // namespace

int main() {
    if (const int status = successful_two_step_loop(); status != 0) {
        return status;
    }
    if (const int status = refused_and_failed_paths(); status != 0) {
        return status;
    }
    if (const int status = malformed_decision_triggers_bounded_repair(); status != 0) {
        return status;
    }
    if (const int status = max_steps_and_verification_disagreement(); status != 0) {
        return status;
    }
    if (const int status = cancellation_stops_before_next_action(); status != 0) {
        return status;
    }
    if (const int status = admission_rejection_prevents_actions(); status != 0) {
        return status;
    }
    if (const int status = rate_limit_recovery_inside_the_loop(); status != 0) {
        return status;
    }
    return 0;
}
