#pragma once

#include <mira/environment.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mira {

struct AgentPersonaKey final {
    TenantId tenant;
    Id128 agent;
    friend constexpr auto operator<=>(const AgentPersonaKey &, const AgentPersonaKey &) = default;
};

struct AgentPersonaRevision final {
    std::uint64_t version = 0;
    std::string instructions;
};

class AgentPersonaSnapshot final {
  public:
    [[nodiscard]] const AgentPersonaKey &key() const noexcept { return key_; }
    [[nodiscard]] const AgentPersonaRevision &revision() const noexcept { return *revision_; }
    [[nodiscard]] bool current() const noexcept { return !activation_.expired(); }

  private:
    friend class AgentPersonaCatalog;
    AgentPersonaSnapshot(AgentPersonaKey key, std::shared_ptr<const AgentPersonaRevision> revision,
                         std::weak_ptr<const bool> activation)
        : key_(key), revision_(std::move(revision)), activation_(std::move(activation)) {}

    AgentPersonaKey key_;
    std::shared_ptr<const AgentPersonaRevision> revision_;
    std::weak_ptr<const bool> activation_;
};

// Host-owned cache of committed configuration. A restore or rollback applies
// the revision loaded by the host; the catalog has no persistent writer.
class AgentPersonaCatalog final {
  public:
    explicit AgentPersonaCatalog(std::size_t max_agents = 64) : max_agents_(max_agents) {}

    [[nodiscard]] Result<void> activate(const AgentPersonaKey &, AgentPersonaRevision);
    [[nodiscard]] Result<AgentPersonaSnapshot> read(const AgentPersonaKey &) const;
    void deactivate(const AgentPersonaKey &);

  private:
    struct Entry final {
        std::shared_ptr<const AgentPersonaRevision> revision;
        std::shared_ptr<const bool> activation;
    };
    const std::size_t max_agents_;
    mutable std::mutex mutex_;
    std::map<AgentPersonaKey, Entry> active_;
};

struct AgentPersonaBinding final {
    AgentPersonaSnapshot snapshot;
    // Host-owned bounded authorization and task-binding probe. Required
    // even when the catalog revision is still shared by other work.
    std::function<bool()> current;
};

// Supply from authenticated task bindings and an already loaded catalog.
// An empty optional denotes intentionally unbound work.
using AgentPersonaSupplier =
    std::function<Result<std::optional<AgentPersonaBinding>>(const OperationContext &)>;

} // namespace mira
