#pragma once

#include <mira/environment.hpp>
#include <mira/memory_contracts.hpp>

#include <functional>
#include <memory>

namespace mira {

struct VerifiedFactClaim final {
    EventId publication_id;
    TaskId task;
    std::uint64_t task_epoch = 0;
    EnvironmentEpoch environment_epoch = 0;
    std::string key;
    std::string value;
    EventId evidence_event;
    Sha256Digest evidence_digest;
    std::string verifier;
    WallTimePoint valid_from;
    WallTimePoint valid_until;
};

struct PublishedTaskFact final {
    RuntimeId runtime;
    SessionId session;
    MemoryScope scope;
    VerifiedFactClaim claim;
    [[nodiscard]] bool valid_at(WallTimePoint now) const noexcept {
        return now >= claim.valid_from && now < claim.valid_until;
    }
};

// Strict v1 only. Legacy flat events are handled explicitly by existing readers.
[[nodiscard]] Result<PublishedTaskFact> parse_published_task_fact(const EventEnvelope &event);

// Trusted host authority: verify the claim semantically against the evidence,
// reserve its lifecycle/authority frame through append, and serialize all
// withdrawal/cancel/takeover/terminal operations with that reservation.
class IVerifiedFactLease {
  public:
    virtual ~IVerifiedFactLease() = default;
    [[nodiscard]] virtual Result<void> validate(const PublishedTaskFact &fact,
                                                const EventEnvelope &evidence,
                                                const OperationContext &context) = 0;
};
using VerifiedFactLeaseAcquirer = std::function<Result<std::unique_ptr<IVerifiedFactLease>>(
    const PublishedTaskFact &, const EventEnvelope &, const OperationContext &)>;

struct VerifiedFactLimits final {
    std::size_t page_size = 128;
    std::size_t max_events_scanned = 4096;
};

// Synchronous bounded protocol, invoked by an Executor-managed host. Requires
// real evidence and an independently trusted semantic lease, never just a model
// verdict. Reuse publication_id on retry; errors may follow a committed append.
class VerifiedFactPublisher final {
  public:
    VerifiedFactPublisher(RuntimeId runtime, SessionId session, MemoryScope scope,
                          std::shared_ptr<IEventStore> events, VerifiedFactLeaseAcquirer acquire,
                          VerifiedFactLimits limits = {});
    [[nodiscard]] Result<AppendReceipt> publish(const VerifiedFactClaim &claim,
                                                const OperationContext &context);

  private:
    RuntimeId runtime_;
    SessionId session_;
    MemoryScope scope_;
    std::shared_ptr<IEventStore> events_;
    VerifiedFactLeaseAcquirer acquire_;
    VerifiedFactLimits limits_;
};

} // namespace mira
