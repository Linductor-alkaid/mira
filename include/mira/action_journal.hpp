#pragma once

#include <mira/event_store.hpp>
#include <mira/environment.hpp>
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
    ActionJournal(RuntimeId runtime_id, IEventStore &store) : runtime_id_(runtime_id), store_(store) {}

    [[nodiscard]] Result<AppendReceipt> prepare(const ActionIntent &intent);
    [[nodiscard]] Result<AppendReceipt> dispatch_started(const ActionIntent &intent);
    [[nodiscard]] Result<AppendReceipt> receipt(const ActionIntent &intent,
                                                const ExecutionReceipt &execution);
    [[nodiscard]] Result<AppendReceipt> execution_uncertain(const ActionIntent &intent,
                                                             std::string reason);
    [[nodiscard]] Result<ActionJournalState> recover(const ActionIntent &intent) const;

private:
    [[nodiscard]] Result<AppendReceipt> append(const ActionIntent &, std::string type,
                                               std::string data, EventClass classification,
                                               Durability durability);
    RuntimeId runtime_id_;
    IEventStore &store_;
    mutable std::mutex mutex_;
    std::unordered_map<ActionId, ActionJournalState, StrongIdHash<ActionId>> states_;
};

} // namespace mira
