#include "m30_support.hpp"

using namespace mira;
using namespace mira::testing;

namespace {
int delivery_boundary_keeps_memory_suspended() {
    for (bool throwing : {false, true}) {
        InteractionMemoryFixture host;
        const auto input = host.coordinator->submit_input(host.input("queued"));
        MIRA_CHECK(input && input.value().task_id);
        const auto task = *input.value().task_id;
        class Sink final : public IUserInputSink {
          public:
            bool throwing = false;
            bool refuse = true;
            Result<void> deliver(const InputEnvelope &, TaskId) override {
                if (throwing)
                    throw std::runtime_error("private delivery error");
                if (refuse)
                    return interaction_error(ErrorCode::Unavailable,
                                             InteractionDomainCode::SinkUnavailable, "refused");
                return {};
            }
        };
        const auto sink = std::make_shared<Sink>();
        sink->throwing = throwing;
        MIRA_CHECK(!host.coordinator->register_sink(task, sink));
        auto status = host.coordinator->memory_status(host.principal, task);
        MIRA_CHECK(status && status.value().suspended && !status.value().job);
        const auto spec = host.spec(input.value());
        MIRA_CHECK(!host.service->context_supplier()(memory_request(spec), memory_context(spec)));
        if (!throwing) {
            sink->refuse = false;
            MIRA_CHECK(host.coordinator->register_sink(task, sink));
            MIRA_CHECK(host.finish(task));
        } else {
            MIRA_CHECK(host.coordinator->record_settlement("queued", TaskState::Failed));
            status = host.coordinator->memory_status(host.principal, task);
            MIRA_CHECK(status && status.value().error && !status.value().job);
        }
    }
    return 0;
}

int correction_cancels_late_recall_without_unbounded_retry() {
    MemoryAgentProbe probe;
    probe.ignore_cancel = true;
    std::mutex mutex;
    std::vector<std::string> constraints;
    const auto process = probe.processor();
    InteractionMemoryFixture host(
        [&](const MemoryAgentInput &input, const OperationContext &context) {
            {
                const std::lock_guard lock(mutex);
                constraints = input.user_constraints;
            }
            return process(input, context);
        });
    const auto first = host.coordinator->submit_input(host.input("first"));
    MIRA_CHECK(first && first.value().task_id);
    const auto task = *first.value().task_id;
    MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
    MIRA_CHECK(probe.wait());
    const auto pending = host.coordinator->memory_status(host.principal, task).value().job;
    MIRA_CHECK(pending);
    auto corrected = host.input("correct", "volume=30");
    corrected.target_task = task;
    MIRA_CHECK(host.coordinator->submit_input(corrected));
    const auto unavailable = host.coordinator->memory_status(host.principal, task);
    MIRA_CHECK(unavailable && unavailable.value().error && !unavailable.value().job);
    MIRA_CHECK(unavailable.value().error->code == ErrorCode::ResourceExhausted);
    probe.release();
    MIRA_CHECK(pending->result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(!pending->result.get());
    auto refresh = host.input("refresh", "use corrected volume");
    refresh.target_task = task;
    MIRA_CHECK(host.coordinator->submit_input(refresh));
    MIRA_CHECK(host.finish(task));
    {
        const std::lock_guard lock(mutex);
        MIRA_CHECK(constraints.size() == 2 && constraints.back() == "use corrected volume");
    }
    return 0;
}

int failures_cancel_and_shutdown_are_observable() {
    for (int mode : {0, 1, 2, 3}) {
        MemoryAgentProbe probe;
        probe.throws = mode == 1;
        MemoryAgentConfig config;
        if (mode == 0)
            config.timeout = 10ms;
        InteractionMemoryFixture host(probe.processor(), config);
        const auto input = host.coordinator->submit_input(host.input("failure"));
        MIRA_CHECK(input && input.value().task_id);
        const auto task = *input.value().task_id;
        MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
        MIRA_CHECK(probe.wait());
        const auto status = host.coordinator->memory_status(host.principal, task);
        MIRA_CHECK(status);
        const auto held = status.value().job;
        MIRA_CHECK(held || (mode == 0 && status.value().error &&
                            status.value().error->code == ErrorCode::DeadlineExceeded));
        if (mode == 1)
            probe.release();
        if (mode == 2) {
            const auto stopped = host.coordinator->cancel_work(task);
            MIRA_CHECK(stopped && stopped.value().outcome(3s));
        }
        if (mode == 3)
            MIRA_CHECK(host.coordinator->close());
        if (held)
            MIRA_CHECK(held->result.wait_for(3s) == std::future_status::ready);
        const auto result = held ? held->result.get() : host.finish(task);
        MIRA_CHECK(!result);
        const auto expected = mode == 0   ? ErrorCode::DeadlineExceeded
                              : mode == 1 ? ErrorCode::Internal
                                          : ErrorCode::Cancelled;
        MIRA_CHECK(result.error().code == expected);
        MIRA_CHECK(result.error().safe_message.find("private") == std::string::npos);
        MIRA_CHECK(host.coordinator->input_record("failure").value().stage == InputStage::Routed);
    }
    InteractionMemoryFixture host;
    const auto input = host.coordinator->submit_input(host.input("executor-rejection"));
    MIRA_CHECK(input && input.value().task_id);
    MIRA_CHECK(host.storage.memory->close());
    (void)host.storage.executor.shutdown(true);
    // Delivery succeeded. Memory submission failure must not cause redelivery.
    MIRA_CHECK(host.coordinator->register_sink(*input.value().task_id,
                                               std::make_shared<m28::RecordingSink>()));
    const auto status = host.coordinator->memory_status(host.principal, *input.value().task_id);
    MIRA_CHECK(status && status.value().error);
    MIRA_CHECK(status.value().error->code == ErrorCode::ResourceExhausted);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(delivery_boundary_keeps_memory_suspended() == 0);
    MIRA_CHECK(correction_cancels_late_recall_without_unbounded_retry() == 0);
    MIRA_CHECK(failures_cancel_and_shutdown_are_observable() == 0);
    return 0;
}
