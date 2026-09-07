#pragma once

// Shared fixtures for the agent-harness closure tests (DEC-015/DEC-016): a
// request-recording scripted provider, a canonical tool-call response builder
// and a plain loop operation context. Consumers: m3_tool_loop_test.cpp and
// integration/agent_harness_test.cpp.

#include "m3_support.hpp"

#include <mira/model_contracts.hpp>
#include <mira/model_digest.hpp>
#include <mira/tool_executor.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mira::testing {

// Serves a scripted list of canonical responses and records every request so
// tests can assert on exposure and feedback. An optional hook runs after the
// nth response was taken, which lets tests inject user messages mid-run
// deterministically (the loop only observes them at the next step boundary).
class RecordingProvider final : public IModelProvider {
  public:
    RecordingProvider(std::shared_ptr<const ModelProfile> profile,
                      std::vector<ModelResponse> script)
        : profile_(std::move(profile)), script_(std::move(script)) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &) override {
        if (context.cancelled()) {
            return make_model_error(ModelDomainCode::ModelCancelled,
                                    "recording provider cancelled", false,
                                    request.operation_id);
        }
        std::unique_lock lock(mutex_);
        requests_.push_back(request);
        std::function<void()> hook;
        const auto hooked = hooks_.find(cursor_);
        if (hooked != hooks_.end()) {
            hook = hooked->second;
        }
        if (cursor_ >= script_.size()) {
            return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                    "recording provider is exhausted", false,
                                    request.operation_id);
        }
        ModelResponse response = script_[cursor_++];
        lock.unlock();
        if (hook != nullptr) {
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

    [[nodiscard]] std::string request_text(std::size_t index) const {
        const auto all = requests();
        std::string joined;
        for (const auto &item : all.at(index).input) {
            for (const auto &part : item.content) {
                if (const auto *text = std::get_if<TextPart>(&part)) {
                    joined += text->text;
                    joined += '\n';
                }
            }
        }
        return joined;
    }

  private:
    std::shared_ptr<const ModelProfile> profile_;
    mutable std::mutex mutex_;
    std::vector<ModelResponse> script_;
    std::vector<ModelRequest> requests_;
    std::map<std::size_t, std::function<void()>> hooks_;
    std::size_t cursor_ = 0;
};

// A canonical ToolCallOutput response targeting one registered BuiltIn tool.
[[nodiscard]] inline ModelResponse tool_call_response(const BuiltinToolSpec &spec,
                                                      const std::string &arguments_json,
                                                      const std::string &call_id = "call-1") {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.status = ModelCompletionStatus::Completed;
    ToolCallOutput call;
    call.provider_call_id = ProviderToolCallId{call_id};
    call.tool_id = spec.tool_id;
    call.provider_name = spec.wire_name;
    call.arguments = parse_json(arguments_json).value();
    call.arguments_digest = digest_string(to_json_string(call.arguments));
    response.output.emplace_back(std::move(call));
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    response.usage.quality = UsageQuality::ProviderReported;
    response.requested_model = "test-model";
    return response;
}

[[nodiscard]] inline OperationContext plain_loop_context() {
    OperationContext context;
    context.session = SessionId::generate();
    context.task = TaskId::generate();
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    return context;
}

} // namespace mira::testing
