#include <mira/replay.hpp>

namespace mira {

OfflineReplayEnvironment::OfflineReplayEnvironment(std::vector<Observation> observations,
                                                   std::vector<ExecutionReceipt> receipts)
    : observations_(std::move(observations)), receipts_(std::move(receipts)) {}

Observation OfflineReplayEnvironment::observe() {
    if (observations_.empty()) return {};
    if (observation_index_ >= observations_.size()) return observations_.back();
    return observations_[observation_index_++];
}

ExecutionReceipt OfflineReplayEnvironment::execute(const InputSequence &) {
    // Recorded receipts are observable replay data, never permission to call
    // a live environment. The caller can inspect them via recorded_receipt().
    return {0, false, "offline replay denies live input"};
}

std::optional<ExecutionReceipt> OfflineReplayEnvironment::recorded_receipt() const {
    if (receipt_index_ >= receipts_.size()) return std::nullopt;
    return receipts_[receipt_index_];
}

void OfflineReplayEnvironment::interrupt() noexcept { interrupted_ = true; }
bool OfflineReplayEnvironment::interrupted() const noexcept { return interrupted_; }

} // namespace mira
