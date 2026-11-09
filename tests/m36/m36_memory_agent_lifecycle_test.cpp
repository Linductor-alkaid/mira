#include "support/m36_support.hpp"
#include "support/test.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

int correction_and_takeover_discard_late_model_results() {
    MemoryAgentProbe probe;
    MemoryAgentFixture fixture;
    fixture.start(probe.processor());
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.seed(binding, "volume=30"));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "volume";
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && probe.wait());
    MIRA_CHECK(!fixture.service->submit(request, memory_context(binding.work)));
    MIRA_CHECK(fixture.service->revise(binding.work.task_id, 2, 3));
    probe.release();
    MIRA_CHECK(handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(!handle.value().result.get());
    binding.work.task_epoch = 2;
    auto snapshot = fixture.service->context_supplier()(memory_request(binding.work),
                                                        memory_context(binding.work));
    MIRA_CHECK(snapshot && snapshot.value().candidates.empty());
    MIRA_CHECK(fixture.service->revoke(binding.work.task_id));
    request.mode = MemoryAgentMode::Record;
    request.write_scope = binding.scopes.front();
    MIRA_CHECK(!fixture.service->submit(request, memory_context(binding.work)));
    return 0;
}

int erase_fences_inflight_writes_and_survives_service_restart() {
    MemoryAgentProbe probe;
    probe.proposal.memories = {{MemoryKind::Preference, {"tone=old", {0}}}};
    MemoryAgentFixture fixture;
    fixture.start(probe.processor());
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.fact(binding, "volume", "30"));
    MIRA_CHECK(fixture.seed(binding, "volume=30"));
    MemoryAgentRequest request;
    request.mode = MemoryAgentMode::Record;
    request.task = binding.work.task_id;
    request.write_scope = binding.scopes.front();
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && probe.wait());
    auto erased = fixture.service->erase_scope(binding.scopes.front());
    probe.release();
    MIRA_CHECK(erased && erased.value().status == ErasureStatus::Complete);
    MIRA_CHECK(handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(!handle.value().result.get());
    MemoryQuery query;
    query.scopes = binding.scopes;
    MIRA_CHECK(fixture.memory->query(query).value().records.empty());
    MIRA_CHECK(fixture.service->close());
    fixture.start();
    MIRA_CHECK(!fixture.service->bind(binding));
    return 0;
}

int cancellation_timeout_exception_and_executor_rejection_settle() {
    for (const auto mode : {0, 1, 2}) {
        MemoryAgentProbe probe;
        probe.throws = mode == 2;
        MemoryAgentFixture fixture;
        MemoryAgentConfig config;
        if (mode == 1)
            config.timeout = 20ms;
        fixture.start(probe.processor(), config);
        const auto binding = fixture.binding();
        MIRA_CHECK(fixture.service->bind(binding));
        MemoryAgentRequest request;
        request.task = binding.work.task_id;
        auto handle = fixture.service->submit(request, memory_context(binding.work));
        MIRA_CHECK(handle && probe.wait());
        if (mode == 0)
            MIRA_CHECK(fixture.service->cancel(handle.value().operation));
        if (mode == 2)
            probe.release();
        MIRA_CHECK(handle.value().result.wait_for(3s) == std::future_status::ready);
        const auto &result = handle.value().result.get();
        MIRA_CHECK(!result);
        const auto expected = mode == 0   ? ErrorCode::Cancelled
                              : mode == 1 ? ErrorCode::DeadlineExceeded
                                          : ErrorCode::Internal;
        MIRA_CHECK(result.error().code == expected);
        MIRA_CHECK(result.error().safe_message.find("private") == std::string::npos);
        MIRA_CHECK(fixture.service->reap().value() == 1);
    }
    MemoryAgentFixture fixture;
    fixture.start();
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.memory->close());
    (void)fixture.executor.shutdown(true);
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    auto rejected = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(!rejected && rejected.error().code == ErrorCode::ResourceExhausted);
    return 0;
}

int close_timeout_is_not_forced_termination() {
    MemoryAgentProbe probe;
    probe.ignore_cancel = true;
    MemoryAgentFixture fixture;
    fixture.start(probe.processor());
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && probe.wait());
    const auto closed = fixture.service->close(0ms);
    probe.release();
    MIRA_CHECK(!closed && closed.error().code == ErrorCode::DeadlineExceeded);
    MIRA_CHECK(fixture.service->close(3s));
    MIRA_CHECK(handle.value().result.wait_for(0ms) == std::future_status::ready);
    MIRA_CHECK(!handle.value().result.get());
    return 0;
}

int failed_input_delivery_suspends_context_until_retry() {
    MemoryAgentFixture fixture;
    fixture.start();
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    class Sink final : public IUserInputSink {
      public:
        bool refuse = true;
        Result<void> deliver(const InputEnvelope &, TaskId) override {
            if (refuse) {
                Error error;
                error.code = ErrorCode::ResourceExhausted;
                return error;
            }
            return {};
        }
    };
    const auto target = std::make_shared<Sink>();
    const auto sink = fixture.service->input_sink(binding.work.task_id, target);
    InputEnvelope input;
    input.principal = binding.principal;
    input.input_id = "correction";
    input.content = "target volume 20";
    const auto supplier = fixture.service->context_supplier();
    auto old = supplier(memory_request(binding.work), memory_context(binding.work));
    MIRA_CHECK(old && old.value().current());
    MIRA_CHECK(!sink->deliver(input, binding.work.task_id));
    MIRA_CHECK(!old.value().current());
    MIRA_CHECK(!supplier(memory_request(binding.work), memory_context(binding.work)));
    target->refuse = false;
    MIRA_CHECK(sink->deliver(input, binding.work.task_id));
    MIRA_CHECK(supplier(memory_request(binding.work), memory_context(binding.work)));
    input.principal.user_id = UserId::generate();
    MIRA_CHECK(!sink->deliver(input, binding.work.task_id));
    return 0;
}
} // namespace
int main() {
    if (correction_and_takeover_discard_late_model_results())
        return 1;
    if (erase_fences_inflight_writes_and_survives_service_restart())
        return 1;
    if (cancellation_timeout_exception_and_executor_rejection_settle())
        return 1;
    if (close_timeout_is_not_forced_termination())
        return 1;
    return failed_input_delivery_suspends_context_until_retry();
}
