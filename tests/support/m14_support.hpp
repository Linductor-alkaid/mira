#pragma once

// Shared fixtures for the M14 recovery-orchestration tests (DEC-031): the
// m9 workflow fixture (executor + control plane + simulator session +
// registry + event store) fronted by one ModelGateway over a scripted,
// recording provider. The provider doubles as a blocking gate for race
// scenarios. Consumers: tests/m14/*.cpp.

#include "m13_support.hpp"
#include "m3_support.hpp"

#include <mira/model_gateway.hpp>
#include <mira/workflow_recovery.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mira::testing {

// Scripted provider that records every request it admitted. `block()` parks
// infer() until `release()` or the operation's cancellation probe fires, so
// tests can interleave host actions with an in-flight recovery attempt.
class RecoveryScriptProvider final : public IModelProvider {
  public:
    RecoveryScriptProvider(std::shared_ptr<const ModelProfile> profile,
                           std::vector<ModelResponse> script)
        : profile_(std::move(profile)), script_(std::move(script)) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &) override {
        entered_.store(true);
        entered_signal_.notify_all();
        if (blocking_.load()) {
            std::unique_lock lock(block_mutex_);
            block_signal_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return released_.load();
            });
            while (!released_.load()) {
                if (context.cancelled()) {
                    break;
                }
                block_signal_.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
        if (context.cancelled()) {
            return make_model_error(ModelDomainCode::ModelCancelled,
                                    "recovery provider cancelled", false,
                                    request.operation_id);
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        std::lock_guard lock(mutex_);
        if (cursor_ >= script_.size()) {
            return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                    "recovery provider is exhausted", false,
                                    request.operation_id);
        }
        ModelResponse response = script_[cursor_++];
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        return response;
    }

    void add_response(ModelResponse response) {
        std::lock_guard lock(mutex_);
        script_.push_back(std::move(response));
    }

    // Race-scenario gate: park the next (and following) infer() calls until
    // release(); the cancellation probe still unblocks them.
    void block() { blocking_.store(true); }
    void release() {
        released_.store(true);
        block_signal_.notify_all();
    }
    void wait_entered() {
        std::unique_lock lock(block_mutex_);
        entered_signal_.wait_for(lock, std::chrono::seconds(5),
                                 [this] { return entered_.load(); });
    }

    [[nodiscard]] std::vector<ModelRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    [[nodiscard]] std::size_t consumed() const {
        std::lock_guard lock(mutex_);
        return cursor_;
    }

  private:
    std::shared_ptr<const ModelProfile> profile_;
    mutable std::mutex mutex_;
    std::vector<ModelResponse> script_;
    std::size_t cursor_ = 0;
    std::vector<ModelRequest> requests_;
    std::atomic<bool> blocking_{false};
    std::atomic<bool> released_{false};
    std::atomic<bool> entered_{false};
    std::mutex block_mutex_;
    std::condition_variable block_signal_;
    std::condition_variable entered_signal_;
};

// The recovery decision bodies (mira.workflow.recovery-decision.v1).
[[nodiscard]] inline std::string skip_step_decision(const std::string &step_id,
                                                    const std::string &rationale) {
    return "{\"action\":\"patch_and_resume\",\"patch_entries\":[{\"target\":\"step_arguments\","
           "\"op\":\"skip\",\"path\":\"" +
           step_id + "\"}],\"used_lessons\":[],\"rationale\":\"" + rationale + "\"}";
}

[[nodiscard]] inline std::string resume_decision(const std::string &rationale) {
    return "{\"action\":\"resume\",\"used_lessons\":[],\"rationale\":\"" + rationale + "\"}";
}

[[nodiscard]] inline std::string cancel_decision(const std::string &rationale) {
    return "{\"action\":\"cancel\",\"used_lessons\":[],\"rationale\":\"" + rationale + "\"}";
}

[[nodiscard]] inline std::string need_user_decision(const std::string &rationale) {
    return "{\"action\":\"need_user\",\"used_lessons\":[],\"rationale\":\"" + rationale + "\"}";
}

// Full harness: workflow fixture + gateway + scripted provider + workflow
// runtime. Orchestrators are created per scenario with make_orchestrator so
// each test controls its config; every orchestrator shares this fixture's
// event store for audit assertions.
class RecoveryFixture final {
  public:
    explicit RecoveryFixture(std::vector<ModelResponse> script = {}) {
        profile_ = std::make_shared<ModelProfile>(
            make_profile(ProtocolDialect::OpenAIResponsesV1, "https://recovery.test"));
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(fixture_.executor_, router_, nullptr,
                                                  PriceTable{}, ModelGatewayConfig{});
        provider_ = std::make_shared<RecoveryScriptProvider>(profile_, std::move(script));
        gateway_->register_provider(provider_);
        gateway_->set_event_store(fixture_.events_, RuntimeId::generate(),
                                  fixture_.session_id_);
        workflow_ = fixture_.make_workflow();
    }

    using ConfigTweak = std::function<void(WorkflowRecoveryConfig &)>;
    [[nodiscard]] std::unique_ptr<WorkflowRecoveryOrchestrator>
    make_orchestrator(ConfigTweak tweak = {}) {
        WorkflowRecoveryConfig config;
        config.profile_id = profile_->id;
        config.model_call_deadline = std::chrono::seconds{5};
        if (tweak) {
            tweak(config);
        }
        auto orchestrator = std::make_unique<WorkflowRecoveryOrchestrator>(
            fixture_.executor_, *workflow_, *fixture_.runtime_, *gateway_,
            fixture_.session_id_, config);
        orchestrator->set_event_store(fixture_.events_);
        return orchestrator;
    }

    WorkflowFixture fixture_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<RecoveryScriptProvider> provider_;
    std::unique_ptr<WorkflowRuntime> workflow_;
};

// Event-store assertion helper: every WorkflowRecoveryAttempted payload of
// the session, parsed.
[[nodiscard]] inline std::vector<WorkflowRecoveryAttemptedEvent>
recovery_attempt_events(const MemoryEventStore &events, const SessionId &session) {
    EventQuery query;
    query.session_id = session;
    const auto page = events.read(query);
    std::vector<WorkflowRecoveryAttemptedEvent> parsed;
    if (!page.has_value()) {
        return parsed;
    }
    for (const auto &envelope : page.value().events) {
        if (envelope.payload.type != "WorkflowRecoveryAttempted") {
            continue;
        }
        auto attempt = parse_workflow_recovery_attempted(envelope.payload);
        if (attempt.has_value()) {
            parsed.push_back(attempt.value());
        }
    }
    return parsed;
}

// One Add mutation wrapping a validated record, for seeding the fake store.
[[nodiscard]] inline MemoryMutation mutation_of(MemoryRecord record) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.type = MemoryMutationType::Add;
    mutation.scope = record.scope;
    mutation.proposed = std::move(record);
    mutation.evidence = {EventId::generate()};
    mutation.reason = MutationReasonCode::VerifiedEvent;
    return mutation;
}

// Collects the payload JSON text of every event of one type in the session.
[[nodiscard]] inline std::vector<std::string>
event_payloads_of_type(const MemoryEventStore &events, const SessionId &session,
                       std::string_view type) {
    EventQuery query;
    query.session_id = session;
    const auto page = events.read(query);
    std::vector<std::string> payloads;
    if (!page.has_value()) {
        return payloads;
    }
    for (const auto &envelope : page.value().events) {
        if (envelope.payload.type == type) {
            payloads.push_back(envelope.payload.data);
        }
    }
    return payloads;
}

} // namespace mira::testing
