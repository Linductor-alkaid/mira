#pragma once

// Shared fixture infrastructure for the ConversationLoop test files
// (split by responsibility under the file-size architecture budget):
// scripted providers, the gateway/admission fixture and the
// canonical request/event helpers. Test-only; no product surface.

#include "support/harness_support.hpp"

#include <mira/conversation_loop.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mira::testing {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class ConversationFixture final {
  public:
    explicit ConversationFixture(ProtocolDialect dialect = ProtocolDialect::OpenAIResponsesV1,
                                 ModelGatewayConfig gateway_config = ModelGatewayConfig{}) {
        kairo::ExecutorConfig executor_config;
        executor_config.min_threads = 2;
        executor_config.max_threads = 2;
        executor_config.queue_capacity = 32;
        executor_.initialize(executor_config);
        profile_ = std::make_shared<ModelProfile>(make_profile(dialect, "https://api.test"));
        // The profile keeps its default output limit (16384): the loop's
        // whole-run envelope (per-turn tokens x allowed requests) is compared
        // against it by the router, and the shipped defaults (1024 x 15) must
        // route. default_config_routes_against_default_profile_limits pins
        // that contract explicitly.
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                  gateway_config);
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);

        BuiltinToolSpec echo;
        echo.wire_name = "echo";
        echo.description = "Returns the message argument verbatim for round-trip tests.";
        echo.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"message": {"type": "string"}},
            "required": ["message"],
            "additionalProperties": false
        })json")
                                                .value()};
        echo_spec_ = echo;
        (void)registry_->register_tool(
            std::move(echo), [this](const JsonValue &arguments, const OperationContext &) {
                const std::lock_guard lock(arguments_mutex_);
                seen_arguments_.push_back(arguments);
                return JsonValue(JsonValue::Object{{"echo", arguments}});
            });

        spec_.task_id = TaskId::generate();
        spec_.session_id = SessionId::generate();
        spec_.task_epoch = 1;
        spec_.profile_id = profile_->id;
        spec_.goal = "what is the answer";
    }

    ~ConversationFixture() { (void)executor_.shutdown(true); }

    void use_provider(std::vector<ModelResponse> script) {
        provider_ = std::make_shared<RecordingProvider>(profile_, std::move(script));
        gateway_->register_provider(provider_);
    }

    // A provider whose first calls fail with scripted errors (for gateway
    // give-up paths) before serving the scripted responses.
    void use_flaky_provider(std::vector<Error> failures, std::vector<ModelResponse> script) {
        provider_ = nullptr;
        flaky_ = std::make_shared<FlakyProvider>(profile_, std::move(failures), std::move(script));
        gateway_->register_provider(flaky_);
    }

    [[nodiscard]] ConversationLoop make_loop(const ConversationLoopConfig &config,
                                             bool with_tools = true) {
        ConversationLoop loop(*gateway_, config);
        loop.set_event_store(events_, runtime_, spec_.session_id);
        if (with_tools) {
            loop.set_tool_registry(registry_);
        }
        admission_->activate(spec_.task_id, spec_.task_epoch);
        return loop;
    }

    [[nodiscard]] const std::vector<JsonValue> &seen_arguments() const {
        const std::lock_guard lock(arguments_mutex_);
        return seen_arguments_;
    }

    kairo::Executor executor_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<RecordingProvider> provider_;
    std::shared_ptr<BuiltinToolRegistry> registry_ = std::make_shared<BuiltinToolRegistry>();
    BuiltinToolSpec echo_spec_;
    std::shared_ptr<MemoryEventStore> events_ = std::make_shared<MemoryEventStore>();
    RuntimeId runtime_ = RuntimeId::generate();
    AgentLoopSpec spec_;

    [[nodiscard]] const BuiltinToolSpec &echo_spec() const { return echo_spec_; }

    // Total infer invocations of the active flaky provider, i.e. requests
    // the provider received, including every gateway-level retry attempt.
    [[nodiscard]] std::size_t flaky_infer_calls() const {
        return flaky_ == nullptr ? 0 : flaky_->calls();
    }

  private:
    class FlakyProvider final : public IModelProvider {
      public:
        FlakyProvider(std::shared_ptr<const ModelProfile> profile, std::vector<Error> failures,
                      std::vector<ModelResponse> script)
            : profile_(std::move(profile)), failures_(std::move(failures)),
              script_(std::move(script)) {}

        [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
        [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                                  const OperationContext &context,
                                                  const ProviderInferOptions &) override {
            ++calls_;
            if (context.cancelled()) {
                return make_model_error(ModelDomainCode::ModelCancelled, "flaky provider cancelled",
                                        false, request.operation_id);
            }
            const std::lock_guard lock(mutex_);
            if (failure_cursor_ < failures_.size()) {
                return failures_[failure_cursor_++];
            }
            if (script_cursor_ >= script_.size()) {
                return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                        "flaky provider is exhausted", false, request.operation_id);
            }
            ModelResponse response = script_[script_cursor_++];
            response.request_id = request.request_id;
            response.operation_id = request.operation_id;
            response.profile_id = request.profile_id;
            return response;
        }

        // Total infer invocations, i.e. requests the provider received,
        // including every gateway-level retry attempt.
        [[nodiscard]] std::size_t calls() const { return calls_.load(); }

      private:
        std::shared_ptr<const ModelProfile> profile_;
        mutable std::mutex mutex_;
        std::vector<Error> failures_;
        std::vector<ModelResponse> script_;
        std::size_t failure_cursor_ = 0;
        std::size_t script_cursor_ = 0;
        std::atomic<std::size_t> calls_{0};
    };

    std::shared_ptr<FlakyProvider> flaky_;

    mutable std::mutex arguments_mutex_;
    std::vector<JsonValue> seen_arguments_;
};

[[nodiscard]] OperationContext conversation_context(std::function<bool()> cancelled = nullptr) {
    OperationContext context;
    context.session = SessionId::generate();
    context.task = TaskId::generate();
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.cancellation_requested = std::move(cancelled);
    return context;
}

[[nodiscard]] std::vector<std::string> session_event_types(const MemoryEventStore &store,
                                                           const SessionId &session) {
    EventQuery query;
    query.session_id = session;
    auto page = store.read(query);
    std::vector<std::string> types;
    if (!page.has_value()) {
        return types;
    }
    for (const auto &event : page.value().events) {
        types.push_back(event.payload.type);
    }
    return types;
}

// ---------------------------------------------------------------------------
// ConversationLoop scenarios
// ---------------------------------------------------------------------------

} // namespace mira::testing
