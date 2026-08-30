#pragma once

#include <mira/environment.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace mira {

// Replays recorded observations and receipts without re-capturing and
// without dispatching real input, network or tool side effects.
class OfflineReplayEnvironment final : public IEnvironment {
  public:
    OfflineReplayEnvironment(std::vector<Observation> observations = {},
                             std::vector<ExecutionReceipt> receipts = {},
                             EnvironmentCapabilities capabilities = {});
    ~OfflineReplayEnvironment() override;

    OfflineReplayEnvironment(const OfflineReplayEnvironment &) = delete;
    OfflineReplayEnvironment &operator=(const OfflineReplayEnvironment &) = delete;
    OfflineReplayEnvironment(OfflineReplayEnvironment &&) noexcept;
    OfflineReplayEnvironment &operator=(OfflineReplayEnvironment &&) noexcept;

    EnvironmentCapabilities capabilities() const override;
    // Returns the next recorded observation verbatim; replay never captures.
    Result<Observation> observe(const ObservationRequest &request,
                                const OperationContext &context) override;
    // Never dispatches. Returns the next recorded receipt, or an
    // ExecutionUncertain error when the journal has no receipt for it.
    Result<ExecutionReceipt> execute(const InputSequence &input,
                                     const OperationContext &context) override;
    Result<void> interrupt(const OperationContext &context) override;

    [[nodiscard]] std::optional<ExecutionReceipt> recorded_receipt() const;
    [[nodiscard]] bool interrupted() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
