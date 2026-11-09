#pragma once

#include "support/conversation_loop_support.hpp"
#include <mira/subagent.hpp>

#include <condition_variable>
#include <stdexcept>

namespace mira::testing {
using namespace std::chrono_literals;

class SubagentTestLog final : public IEventStore {
  public:
    Result<AppendReceipt> append(const AppendRequest &request) override {
        const std::lock_guard lock(mutex);
        if (on_append)
            on_append(request);
        if (request.payload.type == fail_type) {
            Error error;
            error.code = ErrorCode::DataLoss;
            error.safe_message = "private store error";
            return error;
        }
        auto result = store.append(request);
        if (result)
            ++counts[request.payload.type];
        cv.notify_all();
        return result;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return store.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return store.read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return store.recover(options);
    }
    Result<void> flush(Durability durability) override { return store.flush(durability); }
    bool wait(const std::string &type, std::size_t count = 1) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 3s, [&] { return counts[type] >= count; });
    }
    std::size_t count(const std::string &type) {
        const std::lock_guard lock(mutex);
        return counts[type];
    }
    void fail_on(std::string type) {
        const std::lock_guard lock(mutex);
        fail_type = std::move(type);
    }
    MemoryEventStore store;
    std::function<void(const AppendRequest &)> on_append;

  private:
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::size_t> counts;
    std::string fail_type;
};

struct SubagentProbe final {
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false;
    bool ignore_cancel = false;
    bool released = false;
    bool entered = false;
    bool throws = false;
    bool fails = false;
    std::vector<ModelRequest> requests;
    std::vector<AgentLoopSpec> specs;
    std::vector<ModelResponse> script;
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 3s, [&] { return entered; });
    }
    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    std::vector<ModelRequest> recorded() {
        const std::lock_guard lock(mutex);
        return requests;
    }
};

class SubagentProbeProvider final : public IModelProvider {
  public:
    SubagentProbeProvider(ModelProfile profile, std::shared_ptr<SubagentProbe> probe)
        : profile_(std::move(profile)), probe_(std::move(probe)) {}
    const ModelProfile &profile() const override { return profile_; }
    Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &context,
                                const ProviderInferOptions &) override {
        std::unique_lock lock(probe_->mutex);
        probe_->requests.push_back(request);
        probe_->entered = true;
        probe_->cv.notify_all();
        while (probe_->blocked && !probe_->released) {
            if (!probe_->ignore_cancel && context.cancelled_or_expired(Timestamp::now())) {
                Error error;
                error.code =
                    context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded;
                return error;
            }
            probe_->cv.wait_for(lock, 5ms);
        }
        if (probe_->throws)
            throw std::runtime_error("private provider exception");
        if (probe_->fails)
            return make_model_error(ModelDomainCode::CapabilityMismatch, "private failure");
        const auto index = cursor_++;
        auto response =
            index < probe_->script.size() ? probe_->script[index] : text_response("child finding");
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = request.profile_id;
        return response;
    }

  private:
    ModelProfile profile_;
    std::shared_ptr<SubagentProbe> probe_;
    std::size_t cursor_ = 0;
};

inline SubagentProfile subagent_profile(const std::shared_ptr<SubagentProbe> &probe,
                                        std::shared_ptr<BuiltinToolRegistry> tools = nullptr) {
    SubagentProfile profile;
    profile.name = "reader";
    profile.model = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    profile.conversation.max_turns = 4;
    profile.conversation.max_recoveries = 0;
    profile.conversation.max_output_tokens_per_turn = 128;
    profile.conversation.system_prompt = ConversationSystemPrompt{
        "You are a delegated reader. Return findings to your parent; do not speak to the user.",
        "test.child.system.v1"};
    const auto model = profile.model;
    profile.create_resources = [model, probe,
                                tools](const AgentLoopSpec &spec,
                                       const OperationContext &) -> Result<SubagentResources> {
        {
            const std::lock_guard lock(probe->mutex);
            probe->specs.push_back(spec);
        }
        SubagentResources resources;
        resources.provider = std::make_unique<SubagentProbeProvider>(model, probe);
        resources.tools = tools;
        return resources;
    };
    return profile;
}

inline std::vector<BuiltinToolSpec> register_subagent_tools(SubagentCoordinator &owner,
                                                            BuiltinToolRegistry &registry) {
    std::vector<BuiltinToolSpec> specs;
    for (auto &registration : owner.tools()) {
        specs.push_back(registration.spec);
        if (!registry.register_tool(std::move(registration.spec), std::move(registration.handler)))
            return {};
    }
    return specs;
}

inline std::unique_ptr<ConversationLoop> subagent_parent(ConversationFixture &fixture,
                                                         ConversationLoopConfig config = {}) {
    return std::make_unique<ConversationLoop>(fixture.make_loop(config));
}

inline OperationContext subagent_caller(const ConversationFixture &fixture) {
    auto context = conversation_context();
    context.session = fixture.spec_.session_id;
    context.task = fixture.spec_.task_id;
    context.task_epoch = fixture.spec_.task_epoch;
    return context;
}

inline std::optional<SubagentHandle> spawned_handle(SubagentTestLog &log, SessionId parent) {
    EventQuery query;
    query.session_id = parent;
    const auto page = log.read(query);
    if (!page)
        return std::nullopt;
    for (const auto &event : page.value().events) {
        if (event.payload.type != "SubagentSpawned")
            continue;
        const auto parsed = parse_json(event.payload.data);
        if (!parsed)
            return std::nullopt;
        const auto &json = parsed.value();
        const auto session = SessionId::parse(*json.find("child_session_id")->as_string());
        const auto task = TaskId::parse(*json.find("child_task_id")->as_string());
        const auto execution = OperationId::parse(*json.find("execution_id")->as_string());
        if (session && task && execution)
            return SubagentHandle{*session, *task, *execution};
    }
    return std::nullopt;
}
} // namespace mira::testing
