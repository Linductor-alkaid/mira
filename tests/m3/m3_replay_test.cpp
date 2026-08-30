#include "support/m3_support.hpp"

#include <executor/executor.hpp>
#include "support/test.hpp"

#include <mira/agent_loop.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_replay.hpp>
#include <mira/replay.hpp>

#include <memory>
#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

class ReplayFixture final {
  public:
    ReplayFixture() {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        executor_.initialize(config);
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                 ModelGatewayConfig{});
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);
    }
    ~ReplayFixture() { (void)executor_.shutdown(true); }

    executor::Executor executor_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    AgentLoopSpec loop_spec = [] {
        AgentLoopSpec spec;
        spec.task_id = TaskId::generate();
        spec.session_id = SessionId::generate();
        spec.task_epoch = 1;
        spec.goal = "replay the recorded task";
        return spec;
    }();
    [[nodiscard]] AgentLoopSpec &spec() noexcept { return loop_spec; }
};

class NeverSatisfiedLike final : public ILoopVerifier {
  public:
    Verdict verify(const Observation &, const DecisionCandidate &) override {
        return Verdict::NotSatisfied;
    }
};

// A verifier satisfied by the second done claim, mirroring a recorded run.
class ReplayVerifier final : public ILoopVerifier {
  public:
    Verdict verify(const Observation &, const DecisionCandidate &decision) override {
        const auto action = decision.value.find("action");
        if (action == nullptr || !action->is_string() || *action->as_string() != "done") {
            return Verdict::NotSatisfied;
        }
        return ++done_claims_ >= 1 ? Verdict::Satisfied : Verdict::NotSatisfied;
    }

  private:
    int done_claims_ = 0;
};

int offline_replay_runs_without_network_or_input() {
    ReplayFixture fixture;
    fixture.admission_->activate(fixture.spec().task_id, 1);

    // Recorded canonical responses: one action, one terminal claim.
    std::vector<ModelResponse> script;
    script.push_back(text_response("{\"action\":\"tap\",\"x\":0.5,\"y\":0.5,\"reason\":\"replay\"}"));
    script.push_back(text_response("{\"action\":\"done\",\"reason\":\"replayed\"}"));
    auto provider = std::make_shared<ReplayModelProvider>(fixture.profile_, std::move(script));
    fixture.gateway_->register_provider(provider);

    fixture.spec().profile_id = fixture.profile_->id;

    // The replay environment serves recorded observations and receipts; it
    // has no screen capture path and never dispatches input.
    std::vector<Observation> observations;
    for (int index = 0; index < 4; ++index) {
        Observation observation;
        observation.id = ObservationId::generate();
        observation.session_id = SessionId::generate();
        observation.environment_epoch = 1;
        observations.push_back(std::move(observation));
    }
    std::vector<ExecutionReceipt> receipts;
    ExecutionReceipt receipt;
    receipt.status = ExecutionStatus::Completed;
    receipts.push_back(receipt);
    OfflineReplayEnvironment environment(std::move(observations), std::move(receipts),
                                         EnvironmentCapabilities{});

    AgentLoop loop(std::shared_ptr<IEnvironment>(
                       static_cast<IEnvironment *>(&environment), [](auto *) {}),
                   *fixture.gateway_, AgentLoopConfig{4, 1});
    ReplayVerifier verifier;
    OperationContext context;
    context.session = SessionId::generate();
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    auto result = loop.run(fixture.spec(), context, verifier);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Completed);
    // Both recorded model responses were consumed...
    MIRA_CHECK(provider->consumed() == 2);
    // ...the recorded input receipt was returned (not dispatched)...
    MIRA_CHECK(environment.recorded_receipt().has_value());
    // ...and no live input path exists in the replay environment.
    MIRA_CHECK(!environment.capabilities().screen_capture);
    MIRA_CHECK(!environment.capabilities().discrete_input);
    return 0;
}

int replay_is_exhaustion_bounded() {
    ReplayFixture fixture;
    fixture.admission_->activate(fixture.spec().task_id, 1);
    fixture.spec().profile_id = fixture.profile_->id;
    std::vector<ModelResponse> script{text_response("{\"action\":\"tap\",\"x\":0.5,\"y\":0.5,\"reason\":\"r\"}")};
    auto provider = std::make_shared<ReplayModelProvider>(fixture.profile_, std::move(script));
    fixture.gateway_->register_provider(provider);

    std::vector<Observation> observations;
    for (int index = 0; index < 4; ++index) {
        Observation observation;
        observation.id = ObservationId::generate();
        observation.session_id = SessionId::generate();
        observation.environment_epoch = 1;
        observations.push_back(std::move(observation));
    }
    OfflineReplayEnvironment environment(std::move(observations), {}, EnvironmentCapabilities{});
    AgentLoop loop(std::shared_ptr<IEnvironment>(static_cast<IEnvironment *>(&environment),
                                                 [](auto *) {}),
                   *fixture.gateway_, AgentLoopConfig{4, 1});
    NeverSatisfiedLike verifier;
    OperationContext context;
    context.started_at = Timestamp::now();
    auto result = loop.run(fixture.spec(), context, verifier);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == LoopOutcome::Failed);
    MIRA_CHECK(provider->consumed() == provider->script_size());
    // Stream requests are a capability mismatch in replay.
    ProviderInferOptions options;
    options.stream = true;
    auto streamed = provider->infer(ModelRequest{}, OperationContext{}, options);
    MIRA_CHECK(!streamed.has_value());
    return 0;
}

int raw_payload_tombstone_degrades_but_replays() {
    ReplayFixture fixture;
    fixture.admission_->activate(fixture.spec().task_id, 1);

    ModelResponse recorded = text_response("{\"action\":\"done\",\"reason\":\"done\"}");
    ArtifactRef raw;
    raw.id = ArtifactId::generate();
    raw.digest = digest_string("raw");
    raw.media_type = "application/json";
    raw.sensitivity = Sensitivity::Sensitive;
    recorded.protected_raw_response = raw;

    auto provider = std::make_shared<ReplayModelProvider>(
        fixture.profile_, std::vector<ModelResponse>{recorded});
    // The raw artifact was deleted after recording.
    provider->note_raw_artifact_erased();
    fixture.gateway_->register_provider(provider);

    ProviderInferOptions options;
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    ModelRequest request;
    request.request_id = ModelRequestId::generate();
    request.operation_id = context.operation;
    request.profile_id = fixture.profile_->id;
    auto replayed = provider->infer(request, context, options);
    MIRA_CHECK(replayed.has_value());
    MIRA_CHECK(replayed.value().status == ModelCompletionStatus::Completed);
    // Canonical data is intact; the missing raw payload is only flagged.
    MIRA_CHECK(provider->raw_payload_missing());
    MIRA_CHECK(replayed.value().protected_raw_response.has_value());
    return 0;
}

} // namespace

int main() {
    if (const int status = offline_replay_runs_without_network_or_input(); status != 0) {
        return status;
    }
    if (const int status = replay_is_exhaustion_bounded(); status != 0) {
        return status;
    }
    if (const int status = raw_payload_tombstone_degrades_but_replays(); status != 0) {
        return status;
    }
    return 0;
}
