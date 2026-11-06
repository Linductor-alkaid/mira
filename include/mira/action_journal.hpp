#pragma once

#include <mira/environment.hpp>
#include <mira/event_store.hpp>
#include <mira/security.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace mira {

struct ActionIntent final {
    ActionId action_id;
    RuntimeId runtime_id;
    SessionId session_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    Sha256Digest action_digest;
    ResourceDescriptor target;
};

enum class ActionJournalPhase : std::uint8_t {
    Prepared,
    DispatchStarted,
    Receipt,
    ExecutionUncertain,
};

struct ActionJournalState final {
    ActionId action_id;
    ActionJournalPhase phase = ActionJournalPhase::Prepared;
    std::optional<AppendReceipt> last_receipt;
};

class ActionJournal final {
  public:
    struct RecoveryPolicy final {
        std::size_t page_size = 1024;
        std::size_t max_events_scanned = 1'000'000;
    };
    ActionJournal(RuntimeId runtime_id, IEventStore &store)
        : ActionJournal(runtime_id, store, RecoveryPolicy{}) {}
    ActionJournal(RuntimeId runtime_id, IEventStore &store, RecoveryPolicy policy)
        : runtime_id_(runtime_id), store_(store), recovery_policy_(policy) {}

    // Requires a store acknowledgement at least as strong as ProcessCrash.
    // A weaker acknowledgement is DataLoss and never advances local phase.
    // The host must supply a genuinely durable store for crash recovery;
    // MemoryEventStore is only a process-local test double.
    [[nodiscard]] Result<AppendReceipt> prepare(const ActionIntent &intent);
    [[nodiscard]] Result<AppendReceipt> dispatch_started(const ActionIntent &intent);
    [[nodiscard]] Result<AppendReceipt> receipt(const ActionIntent &intent,
                                                const ExecutionReceipt &execution);
    [[nodiscard]] Result<AppendReceipt> execution_uncertain(const ActionIntent &intent,
                                                            std::string reason);
    [[nodiscard]] Result<ActionJournalState> recover(const ActionIntent &intent) const;

  private:
    struct Entry final {
        std::string binding;
        ActionJournalState state;
    };
    [[nodiscard]] Result<void> validate(const ActionIntent &intent) const;
    [[nodiscard]] Result<AppendReceipt> append(const ActionIntent &, std::string type,
                                               std::string data, EventClass classification,
                                               Durability durability);
    RuntimeId runtime_id_;
    IEventStore &store_;
    RecoveryPolicy recovery_policy_;
    mutable std::mutex mutex_;
    std::unordered_map<ActionId, Entry, StrongIdHash<ActionId>> states_;
};

} // namespace mira
