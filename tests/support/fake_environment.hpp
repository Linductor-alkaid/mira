#pragma once

#include <mira/environment.hpp>

#include <utility>

namespace mira::test {

// Minimal in-memory environment for Runtime-level tests. It serves one
// canned observation per observe call and records executed input.
class FakeEnvironment final : public IEnvironment {
  public:
    explicit FakeEnvironment(Observation observation = {}) : observation_(std::move(observation)) {}

    EnvironmentCapabilities capabilities() const override {
        EnvironmentCapabilities capabilities;
        capabilities.screen_capture = true;
        capabilities.discrete_input = true;
        capabilities.input_release = true;
        capabilities.epoch_invalidation = true;
        return capabilities;
    }

    Result<Observation> observe(const ObservationRequest & /*request*/,
                                const OperationContext & /*context*/) override {
        if (observation_.id.is_nil()) {
            observation_.id = ObservationId::generate();
        }
        observation_.environment_epoch = epoch_;
        return observation_;
    }

    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext & /*context*/) override {
        if (input.events.empty()) {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "mira.test";
            error.safe_message = "input sequence must not be empty";
            return error;
        }
        executed_ += input.events.size();
        ExecutionReceipt receipt;
        receipt.status = ExecutionStatus::Completed;
        receipt.environment_epoch = epoch_;
        receipt.safe_message = "fake input accepted";
        return receipt;
    }

    Result<void> interrupt(const OperationContext & /*context*/) override {
        interrupted_ = true;
        return Result<void>{};
    }

    void bump_epoch() { ++epoch_; }
    [[nodiscard]] std::size_t executed_event_count() const noexcept { return executed_; }
    [[nodiscard]] bool interrupted() const noexcept { return interrupted_; }

  private:
    Observation observation_;
    std::size_t executed_ = 0;
    EnvironmentEpoch epoch_ = 1;
    bool interrupted_ = false;
};

} // namespace mira::test
