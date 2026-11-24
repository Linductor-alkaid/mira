#pragma once

#include <mira/memory_contracts.hpp>
#include <mira/social_identity.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kairo {
class Executor;
}

namespace mira {

class SqliteMemoryStore;

struct SocialObjectIdentity final {
    std::uint64_t revision = 0;
    std::vector<std::string> aliases;
};

struct SocialAliasMatch final {
    SocialObjectKey key;
    std::uint64_t revision = 0;
};

enum class SocialAliasResolution : std::uint8_t { Missing, Unique, Ambiguous };

struct SocialAliasResult final {
    SocialAliasResolution resolution = SocialAliasResolution::Missing;
    std::vector<SocialAliasMatch> matches;
    bool truncated = false;
};

struct SocialAccountBinding final {
    SocialObjectKey object;
    std::uint64_t object_revision = 0;
    EventId source_event; // First admitted source or link evidence.
    friend bool operator==(const SocialAccountBinding &, const SocialAccountBinding &) = default;
};

struct SocialAccountAttributeRecall final {
    SocialAccountBinding binding;
    MemoryQueryResult assertions;
};

struct SqliteSocialDirectoryOptions final {
    std::filesystem::path path;
    std::size_t max_environments = 256;
    std::size_t max_objects = 4096; // Includes object erasure barriers.
    std::size_t max_pending_requests = 32;
    std::chrono::milliseconds operation_timeout{30000};
    std::size_t max_accounts = 16384; // Includes account erasure barriers.
};

// Host-owned authoritative identity directory. All entry points require a
// current authorized environment from the host, never one supplied by a model.
class SqliteSocialDirectory final {
  public:
    // A paired directory completes outstanding erasure before open succeeds.
    // The host opens the memory store first and gates raw memory access on
    // current directory authorization until both stores are ready.
    [[nodiscard]] static Result<std::unique_ptr<SqliteSocialDirectory>>
    open(kairo::Executor &executor, SqliteSocialDirectoryOptions options,
         std::shared_ptr<SqliteMemoryStore> memory = nullptr);
    ~SqliteSocialDirectory();

    SqliteSocialDirectory(const SqliteSocialDirectory &) = delete;
    SqliteSocialDirectory &operator=(const SqliteSocialDirectory &) = delete;

    [[nodiscard]] Result<void> activate(const SocialObjectKey &, SocialObjectIdentity);
    [[nodiscard]] Result<std::optional<SocialObjectIdentity>> read(const SocialObjectKey &) const;
    [[nodiscard]] Result<SocialAliasResult> resolve(const SocialEnvironmentKey &, std::string alias,
                                                    std::size_t limit = 8) const;
    [[nodiscard]] Result<std::optional<SocialAccountBinding>>
    resolve_account(const SocialEnvironmentKey &, const SocialAccountKey &) const;
    // source_event is a committed event authorized and validated by the host.
    // Supply a known object when evidence establishes the account's owner.
    [[nodiscard]] Result<SocialAccountBinding>
    observe_account(const SocialEnvironmentKey &, const SocialAccountKey &, EventId source_event,
                    std::optional<SocialObjectKey> known_object = std::nullopt);
    // Requires a paired memory store. The caller supplies the authorized
    // environment and exact field from the agent's current conversation.
    [[nodiscard]] Result<std::optional<SocialAccountAttributeRecall>>
    recall_account_attribute(const SocialEnvironmentKey &, const SocialAccountKey &,
                             std::string field, std::size_t limit = 8) const;
    [[nodiscard]] Result<void> erase_object(const SocialObjectKey &);
    [[nodiscard]] Result<void> erase_environment(const SocialEnvironmentKey &);
    [[nodiscard]] Result<void> close() noexcept;

  private:
    class Impl;
    explicit SqliteSocialDirectory(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
