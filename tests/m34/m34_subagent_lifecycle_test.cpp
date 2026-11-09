#include "support/subagent_test_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int cancellation_deadlines_takeover_and_executor_shutdown() {
    for (int mode = 0; mode < 6; ++mode) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        probe->blocked = true;
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
        auto profile = subagent_profile(probe);
        if (mode == 4)
            profile.timeout = 300ms;
        MIRA_CHECK(owner.register_profile(std::move(profile)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"work"})"),
             text_response("waiting"), text_response("finished")});
        auto context = conversation_context();
        if (mode == 3)
            context.deadline = std::chrono::steady_clock::now() + 300ms;
        const auto started = owner.start_parent(subagent_parent(fixture), context);
        MIRA_CHECK(started && probe->wait_entered());
        const auto handle = spawned_handle(*log, fixture.spec_.session_id);
        MIRA_CHECK(handle);
        if (mode == 0)
            MIRA_CHECK(owner.cancel(*handle));
        if (mode == 1)
            owner.request_cancel();
        if (mode == 2)
            owner.request_human_takeover();
        if (mode == 5)
            (void)fixture.executor_.shutdown(true);
        MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
        const auto result = started.value().get();
        MIRA_CHECK(result);
        const auto snapshot = owner.get(*handle);
        MIRA_CHECK(snapshot && snapshot.value().answer.empty());
        MIRA_CHECK(snapshot.value().status ==
                       (mode == 4 ? SubagentStatus::TimedOut : SubagentStatus::Cancelled) ||
                   (mode == 3 && snapshot.value().status == SubagentStatus::TimedOut));
        if (mode == 0 || mode == 4)
            MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
        else
            MIRA_CHECK(result.value().outcome != ConversationOutcome::Answered);
        auto wrong = *handle;
        wrong.execution_id = OperationId::generate();
        MIRA_CHECK(!owner.get(wrong) && !owner.cancel(wrong));
        MIRA_CHECK(owner.close());
        MIRA_CHECK(!owner.spawn("reader", "late work", subagent_caller(fixture)));
        MIRA_CHECK(!owner.start_parent(subagent_parent(fixture), conversation_context()));
    }
    return 0;
}

int close_timeout_does_not_report_a_false_terminal() {
    ConversationFixture fixture;
    auto log = std::make_shared<SubagentTestLog>();
    auto probe = std::make_shared<SubagentProbe>();
    probe->blocked = true;
    probe->ignore_cancel = true;
    SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                              std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
    MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
    const auto tools = register_subagent_tools(owner, *fixture.registry_);
    fixture.use_provider({tool_call_response(tools[0], R"({"profile":"reader","prompt":"work"})"),
                          text_response("waiting")});
    const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
    const bool entered = started && probe->wait_entered();
    const auto first_close = owner.close(5ms);
    const auto handle = spawned_handle(*log, fixture.spec_.session_id);
    const auto snapshot = handle ? owner.get(*handle) : Result<SubagentSnapshot>{Error{}};
    const bool pending = started && started.value().wait_for(0ms) != std::future_status::ready;
    probe->release(); // Always release the deliberately uncooperative fixture before assertions.
    MIRA_CHECK(entered && handle && snapshot);
    MIRA_CHECK(!first_close && first_close.error().code == ErrorCode::DeadlineExceeded);
    MIRA_CHECK(snapshot.value().status == SubagentStatus::Running && pending);
    MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(started.value().get().value().outcome == ConversationOutcome::Cancelled);
    MIRA_CHECK(owner.get(*handle).value().answer.empty());
    MIRA_CHECK(owner.close());
    return 0;
}

int cancellation_during_factory_and_worker_self_close() {
    ConversationFixture fixture;
    auto log = std::make_shared<SubagentTestLog>();
    auto probe = std::make_shared<SubagentProbe>();
    SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                              std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
    auto profile = subagent_profile(probe);
    std::atomic<bool> self_close_rejected{false};
    profile.create_resources = [&](const AgentLoopSpec &,
                                   const OperationContext &context) -> Result<SubagentResources> {
        const auto closed = owner.close(1ms);
        self_close_rejected.store(!closed && closed.error().code == ErrorCode::InvalidState);
        std::unique_lock lock(probe->mutex);
        probe->entered = true;
        probe->cv.notify_all();
        while (!context.cancelled())
            probe->cv.wait_for(lock, 5ms);
        Error error;
        error.code = ErrorCode::Cancelled;
        return error;
    };
    MIRA_CHECK(owner.register_profile(std::move(profile)));
    const auto tools = register_subagent_tools(owner, *fixture.registry_);
    fixture.use_provider({tool_call_response(tools[0], R"({"profile":"reader","prompt":"work"})"),
                          text_response("waiting")});
    const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
    MIRA_CHECK(started && probe->wait_entered());
    owner.request_cancel();
    MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(started.value().get());
    MIRA_CHECK(self_close_rejected.load() && probe->recorded().empty());
    MIRA_CHECK(owner.close());
    return 0;
}

class EmptyWorker final : public kairo::IBlockingIoWorker {
  public:
    void run(kairo::StopToken) override {}
    void wakeup() noexcept override {}
};

int executor_rejection_and_event_failure_are_visible() {
    for (int mode = 0; mode < 4; ++mode) {
        ConversationFixture fixture;
        auto log = std::make_shared<SubagentTestLog>();
        auto probe = std::make_shared<SubagentProbe>();
        SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                                  std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
        MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
        const auto tools = register_subagent_tools(owner, *fixture.registry_);
        if (mode < 2) {
            log->on_append = [&](const AppendRequest &request) {
                const auto wanted = mode == 0 ? "SubagentParentStarted" : "SubagentSpawned";
                if (request.payload.type != wanted)
                    return;
                const auto json = parse_json(request.payload.data).value();
                const auto id =
                    *json.find(mode == 0 ? "parent_execution_id" : "execution_id")->as_string();
                kairo::BlockingWorkerSpec duplicate;
                duplicate.name = (mode == 0 ? "mira-parent-" : "mira-child-") + id;
                duplicate.config.thread_name = duplicate.name;
                duplicate.worker = std::make_unique<EmptyWorker>();
                (void)fixture.executor_.start_worker(std::move(duplicate));
            };
        } else
            log->fail_on(mode == 2 ? "SubagentSpawned" : "SubagentSettled");
        fixture.use_provider(
            {tool_call_response(tools[0], R"({"profile":"reader","prompt":"work"})"),
             text_response("handled"), text_response("handled")});
        const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
        if (mode == 0) {
            MIRA_CHECK(!started && started.error().code == ErrorCode::ResourceExhausted);
            MIRA_CHECK(probe->recorded().empty());
        } else {
            MIRA_CHECK(started && started.value().wait_for(3s) == std::future_status::ready);
            const auto result = started.value().get();
            if (mode == 3)
                MIRA_CHECK(!result && result.error().code == ErrorCode::DataLoss);
            else {
                MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
                MIRA_CHECK(probe->recorded().empty());
            }
        }
        const auto closed = owner.close();
        if (mode == 3)
            MIRA_CHECK(!closed && closed.error().code == ErrorCode::DataLoss);
        else
            MIRA_CHECK(closed);
    }
    return 0;
}

int control_tools_use_the_canonical_boundary() {
    ConversationFixture fixture;
    auto log = std::make_shared<SubagentTestLog>();
    auto probe = std::make_shared<SubagentProbe>();
    probe->blocked = true;
    SubagentCoordinator owner(fixture.executor_, fixture.spec_, log,
                              std::make_shared<MemoryArtifactStore>(), fixture.runtime_);
    MIRA_CHECK(owner.register_profile(subagent_profile(probe)));
    const auto tools = register_subagent_tools(owner, *fixture.registry_);
    fixture.use_provider({tool_call_response(tools[0], R"({"profile":"reader","prompt":"read"})"),
                          text_response("waiting"), text_response("cancel handled")});
    const auto started = owner.start_parent(subagent_parent(fixture), conversation_context());
    MIRA_CHECK(started && probe->wait_entered() && log->wait("SubagentParentWaiting"));
    const auto handle = spawned_handle(*log, fixture.spec_.session_id);
    MIRA_CHECK(handle);
    JsonValue::Object args{{"session_id", handle->session_id.to_string()},
                           {"task_id", handle->task_id.to_string()},
                           {"execution_id", handle->execution_id.to_string()}};
    for (std::size_t index = 1; index <= 2; ++index) {
        auto request = fixture.provider_->requests().back();
        request.request_id = ModelRequestId::generate();
        const auto response = tool_call_response(tools[index], to_json_string(JsonValue{args}));
        const auto proposals = resolve_tool_calls(request, response);
        MIRA_CHECK(proposals && proposals.value().proposals.size() == 1);
        auto caller = subagent_caller(fixture);
        caller.operation = proposals.value().proposals.front().operation_id;
        const auto executed =
            fixture.registry_->execute(proposals.value().proposals.front(), caller);
        MIRA_CHECK(executed && !executed.value().failed);
        MIRA_CHECK(executed.value().result.find("execution_id") &&
                   *executed.value().result.find("execution_id")->as_string() ==
                       handle->execution_id.to_string());
    }
    MIRA_CHECK(started.value().wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(started.value().get().value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(owner.get(*handle).value().status == SubagentStatus::Cancelled);
    MIRA_CHECK(owner.close());
    return 0;
}
} // namespace

int main() {
    for (const auto test : {cancellation_deadlines_takeover_and_executor_shutdown,
                            close_timeout_does_not_report_a_false_terminal,
                            cancellation_during_factory_and_worker_self_close,
                            executor_rejection_and_event_failure_are_visible,
                            control_tools_use_the_canonical_boundary}) {
        if (const auto result = test(); result != 0)
            return result;
    }
    return 0;
}
