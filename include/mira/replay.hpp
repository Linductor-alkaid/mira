#pragma once

#include <mira/environment.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace mira {

class OfflineReplayEnvironment final : public IEnvironment {
public:
    explicit OfflineReplayEnvironment(std::vector<Observation> observations = {},
                                      std::vector<ExecutionReceipt> receipts = {});
    Observation observe() override;
    ExecutionReceipt execute(const InputSequence &) override;
    [[nodiscard]] std::optional<ExecutionReceipt> recorded_receipt() const;
    void interrupt() noexcept override;
    [[nodiscard]] bool interrupted() const noexcept;

private:
    std::vector<Observation> observations_;
    std::vector<ExecutionReceipt> receipts_;
    std::size_t observation_index_ = 0;
    std::size_t receipt_index_ = 0;
    bool interrupted_ = false;
};

} // namespace mira
