#pragma once

#include "m28/m28_support.hpp"
#include <stdexcept>

namespace mira::m29 {

class FaultStore final : public IEventStore {
  public:
    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (fail)
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::LedgerUnavailable,
                                     "injected storage failure");
        auto result = store.append(request);
        if (result)
            result.value().achieved = buffered ? Durability::Buffered : request.required;
        return result;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return store.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return store.read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return store.recover(options);
    }
    Result<void> flush(Durability durability) override { return store.flush(durability); }
    MemoryEventStore store;
    bool fail = false;
    bool buffered = false;
};

class FailingSink final : public IUserInputSink {
  public:
    Result<void> deliver(const InputEnvelope &envelope, TaskId) override {
        ++attempts;
        if (envelope.input_id == blocked && fail)
            return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::SinkUnavailable,
                                     "private provider details");
        ids.push_back(envelope.input_id);
        if (envelope.input_id == blocked && throwing)
            throw std::runtime_error("private provider details");
        return Result<void>{};
    }
    std::string blocked;
    bool fail = false;
    bool throwing = false;
    int attempts = 0;
    std::vector<std::string> ids;
};

} // namespace mira::m29
