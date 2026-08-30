#include <mira/replay.hpp>

#include <mutex>
#include <utility>

namespace mira {

namespace {

Error replay_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.replay";
    error.safe_message = std::move(message);
    return error;
}

} // namespace

class OfflineReplayEnvironment::Impl final {
  public:
    Impl(std::vector<Observation> recorded_observations,
         std::vector<ExecutionReceipt> recorded_receipts, EnvironmentCapabilities capabilities)
        : observations(std::move(recorded_observations)), receipts(std::move(recorded_receipts)),
          declared_capabilities(capabilities) {}

    mutable std::mutex mutex;
    std::vector<Observation> observations;
    std::vector<ExecutionReceipt> receipts;
    EnvironmentCapabilities declared_capabilities;
    std::size_t observation_index = 0;
    std::size_t receipt_index = 0;
    bool interrupted = false;
};

OfflineReplayEnvironment::OfflineReplayEnvironment(std::vector<Observation> observations,
                                                   std::vector<ExecutionReceipt> receipts,
                                                   EnvironmentCapabilities capabilities)
    : impl_(std::make_unique<Impl>(std::move(observations), std::move(receipts), capabilities)) {}

OfflineReplayEnvironment::~OfflineReplayEnvironment() = default;
OfflineReplayEnvironment::OfflineReplayEnvironment(OfflineReplayEnvironment &&) noexcept = default;
OfflineReplayEnvironment &
OfflineReplayEnvironment::operator=(OfflineReplayEnvironment &&) noexcept = default;

EnvironmentCapabilities OfflineReplayEnvironment::capabilities() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->declared_capabilities;
}

Result<Observation> OfflineReplayEnvironment::observe(const ObservationRequest & /*request*/,
                                                      const OperationContext &context) {
    std::lock_guard lock(impl_->mutex);
    if (context.cancelled()) {
        return replay_error(ErrorCode::Cancelled, "replay observe was cancelled");
    }
    if (impl_->observation_index >= impl_->observations.size()) {
        return replay_error(ErrorCode::NotFound, "replay has no recorded observation left");
    }
    // The recorded observation is returned verbatim; replay must never
    // re-capture or refresh components from any live environment.
    return impl_->observations[impl_->observation_index++];
}

Result<ExecutionReceipt> OfflineReplayEnvironment::execute(const InputSequence &input,
                                                           const OperationContext & /*context*/) {
    std::lock_guard lock(impl_->mutex);
    if (input.events.empty()) {
        return replay_error(ErrorCode::InvalidArgument, "input sequence must not be empty");
    }
    if (impl_->receipt_index >= impl_->receipts.size()) {
        return replay_error(ErrorCode::ExecutionUncertain,
                            "replay has no recorded receipt for this input");
    }
    return impl_->receipts[impl_->receipt_index++];
}

Result<void> OfflineReplayEnvironment::interrupt(const OperationContext & /*context*/) {
    std::lock_guard lock(impl_->mutex);
    impl_->interrupted = true;
    return Result<void>{};
}

std::optional<ExecutionReceipt> OfflineReplayEnvironment::recorded_receipt() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->receipt_index == 0 || impl_->receipt_index > impl_->receipts.size()) {
        return std::nullopt;
    }
    return impl_->receipts[impl_->receipt_index - 1];
}

bool OfflineReplayEnvironment::interrupted() const noexcept { return impl_->interrupted; }

} // namespace mira
