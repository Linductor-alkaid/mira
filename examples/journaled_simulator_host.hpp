#pragma once

#include <mira/action_dispatcher.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>

#include <mutex>

namespace mira::examples {

// Fixed task/epoch policy for offline simulator hosts only. Real adapters
// need their own authority, lifecycle and cancellation/interrupt bridge.
// Hosts sharing an environment must also share the resource mutex.
inline std::shared_ptr<JournaledActionDispatcher>
simulator_dispatcher(RuntimeId runtime, SessionId session, TaskId task, std::uint64_t epoch,
                     std::shared_ptr<adapters::simulator::SimulatorEnvironment> environment,
                     std::shared_ptr<IEventStore> events, std::shared_ptr<std::mutex> resource) {
    class Lease final : public IActionDispatchLease {
      public:
        Lease(std::shared_ptr<std::mutex> resource, std::unique_lock<std::mutex> lock,
              std::shared_ptr<adapters::simulator::SimulatorEnvironment> environment, TaskId task,
              std::uint64_t epoch)
            : resource_(std::move(resource)), lock_(std::move(lock)),
              environment_(std::move(environment)), task_(task), epoch_(epoch) {}
        Result<void> validate(const ActionIntent &intent,
                              const OperationContext &context) override {
            if (lock_.owns_lock() && intent.task_id == task_ && intent.task_epoch == epoch_ &&
                context.task == task_ && context.task_epoch == epoch_ &&
                intent.environment_epoch == environment_->environment_epoch() &&
                !context.cancelled_or_expired(Timestamp::now())) {
                return {};
            }
            Error error;
            error.code = ErrorCode::InvalidState;
            error.domain = "mira.example";
            error.safe_message = "simulator lease is no longer valid";
            return error;
        }

      private:
        std::shared_ptr<std::mutex> resource_;
        std::unique_lock<std::mutex> lock_;
        std::shared_ptr<adapters::simulator::SimulatorEnvironment> environment_;
        TaskId task_;
        std::uint64_t epoch_;
    };
    return std::make_shared<JournaledActionDispatcher>(
        runtime, session, std::move(events),
        [resource = std::move(resource), environment = std::move(environment), task,
         epoch](const ActionIntent &,
                const OperationContext &) -> Result<std::unique_ptr<IActionDispatchLease>> {
            std::unique_lock lock(*resource, std::try_to_lock);
            if (!lock.owns_lock()) {
                Error error;
                error.code = ErrorCode::ResourceExhausted;
                error.domain = "mira.example";
                error.safe_message = "simulator input resource is busy";
                return error;
            }
            return std::unique_ptr<IActionDispatchLease>(
                std::make_unique<Lease>(resource, std::move(lock), environment, task, epoch));
        });
}
} // namespace mira::examples
