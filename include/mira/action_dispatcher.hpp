#pragma once

#include <mira/action_journal.hpp>

#include <functional>
#include <memory>

namespace mira {

// Host-owned reservation of the right to dispatch one bounded input sequence.
// The host serializes acquisition/withdrawal, checks task/environment epochs,
// and keeps the resource reserved until destruction. validate() runs after
// durable start and must reject withdrawn/expired leases. An in-flight
// withdrawal must also cancel the context and interrupt the environment.
// Destruction must release the reservation without throwing.
class IActionDispatchLease {
  public:
    virtual ~IActionDispatchLease() = default;
    [[nodiscard]] virtual Result<void> validate(const ActionIntent &intent,
                                                const OperationContext &context) = 0;
};

using ActionLeaseAcquirer = std::function<Result<std::unique_ptr<IActionDispatchLease>>(
    const ActionIntent &, const OperationContext &)>;

// Synchronous bounded protocol, invoked by an Executor-managed loop. Owns
// the injected store, never an Executor or a background worker. Each dispatch
// owns a one-action journal, so local journal state does not grow per session.
// A dispatcher is bound to a runtime and session; construct one per host
// session. It must share the loop's event stream for checkpoint/recovery.
class JournaledActionDispatcher final {
  public:
    JournaledActionDispatcher(RuntimeId runtime, SessionId session,
                              std::shared_ptr<IEventStore> events, ActionLeaseAcquirer acquire);

    [[nodiscard]] Result<ExecutionReceipt> dispatch(IEnvironment &environment,
                                                    const InputSequence &input,
                                                    const OperationContext &context,
                                                    EnvironmentEpoch observed_epoch);

  private:
    RuntimeId runtime_;
    SessionId session_;
    std::shared_ptr<IEventStore> events_;
    ActionLeaseAcquirer acquire_;
};

} // namespace mira
