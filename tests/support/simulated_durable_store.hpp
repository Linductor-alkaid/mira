#pragma once

#include <mira/event_store.hpp>

namespace mira::testing {

// Test-only acknowledgement simulator. The backing bytes are process-local:
// this exercises callers' protocol, never crash or power-loss durability.
class SimulatedDurableStore final : public IEventStore {
  public:
    explicit SimulatedDurableStore(std::shared_ptr<MemoryEventStore> data)
        : data_(std::move(data)) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        auto result = data_->append(request);
        if (result)
            result.value().achieved = request.required;
        return result;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        std::vector<AppendReceipt> receipts;
        for (const auto &request : requests) {
            auto result = append(request);
            if (!result)
                return result.error();
            receipts.push_back(result.value());
        }
        return receipts;
    }
    Result<EventPage> read(const EventQuery &query) const override { return data_->read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return data_->recover(options);
    }
    Result<void> flush(Durability) override { return {}; }

  private:
    std::shared_ptr<MemoryEventStore> data_;
};

} // namespace mira::testing
