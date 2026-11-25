#pragma once

#include <mira/event_store.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>
#include <mira/social_identity.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace kairo {
class Executor;
}

namespace mira {

class SqliteMemoryStore;
class IModelProvider;
struct OperationContext;

struct SocialObjectIdentity final {
    std::uint64_t revision = 0;
    std::vector<std::string> aliases;
};

struct SocialAliasMatch final {
    SocialObjectKey key;
    std::uint64_t revision = 0;
    friend bool operator==(const SocialAliasMatch &, const SocialAliasMatch &) = default;
};

enum class SocialAliasResolution : std::uint8_t { Missing, Unique, Ambiguous };

struct SocialAliasResult final {
    SocialAliasResolution resolution = SocialAliasResolution::Missing;
    std::vector<SocialAliasMatch> matches;
    bool truncated = false;
    friend bool operator==(const SocialAliasResult &, const SocialAliasResult &) = default;
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

struct SocialMessageLocator final {
    SessionId session;
    SessionSequence sequence = 0;
    EventId event;
    friend bool operator==(const SocialMessageLocator &, const SocialMessageLocator &) = default;
};

struct SocialMessageEvidence final {
    SocialMessageLocator message;
    SocialObjectKey speaker;
    std::string source;
    Sha256Digest account_digest;
    Sha256Digest event_integrity;
    friend bool operator==(const SocialMessageEvidence &, const SocialMessageEvidence &) = default;
};

struct SocialMessageAttributeCandidate final {
    std::string field;
    std::string quote; // Exact value excerpt from the indexed speaker's message.
};

struct SocialMessageAttributeReport final {
    SocialMessageEvidence speaker;
    ConsolidationReport consolidation;
};

struct SocialMessagePage final {
    std::vector<SocialMessageEvidence> evidence;
    bool has_more = false;
};

// An authenticated account mention, an earlier message, or a name from the text.
using SocialMentionReference = std::variant<SocialAccountKey, SocialMessageLocator, std::string>;
using SocialMentionResult =
    std::variant<std::monostate, SocialAccountBinding, SocialMessageEvidence, SocialAliasResult>;

struct SocialMessageParticipants final {
    SocialMessageEvidence speaker;
    // One result per reference, in the supplied order. Alias candidates retain their source.
    std::vector<SocialMentionResult> mentions;
    friend bool operator==(const SocialMessageParticipants &,
                           const SocialMessageParticipants &) = default;
};

struct SocialMessageRelationshipCandidate final {
    std::string predicate;
    std::string alias; // Literal name or reference appearing in quote.
    std::string quote;
    bool speaker_is_subject = true;
    std::optional<SocialMessageLocator> prior_message = std::nullopt;
};

struct SocialMessageRelationshipReport final {
    SocialMessageParticipants participants;
    std::vector<std::size_t> unresolved; // Positions in the input candidate list.
    ConsolidationReport consolidation;   // Remaining positions, in order.
};

struct SqliteSocialDirectoryOptions final {
    std::filesystem::path path;
    std::size_t max_environments = 256;
    std::size_t max_objects = 4096; // Includes object erasure barriers.
    std::size_t max_pending_requests = 32;
    std::chrono::milliseconds operation_timeout{30000};
    std::size_t max_accounts = 16384; // Includes account erasure barriers.
    std::size_t max_messages = 65536; // Includes message erasure barriers.
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
    // The authenticated adapter supplies a committed event from its authorized session.
    [[nodiscard]] Result<SocialMessageEvidence> index_message(const SocialEnvironmentKey &,
                                                              const SocialAccountKey &sender,
                                                              const SocialMessageLocator &,
                                                              const IEventStore &events);
    // Reads the projected evidence inside (after, through] for one session.
    [[nodiscard]] Result<SocialMessagePage>
    read_message_evidence(const SocialEnvironmentKey &, SessionId session, SessionSequence after,
                          SessionSequence through, std::size_t limit = 32) const;
    [[nodiscard]] Result<SocialMessageParticipants>
    resolve_message_participants(const SocialEnvironmentKey &, const SocialMessageLocator &,
                                 std::span<const SocialMentionReference> mentions,
                                 std::size_t alias_limit = 8) const;
    // For a currently authorized, managed host operation with a paired store.
    // The indexed speaker and original message supply the scope and evidence.
    [[nodiscard]] Result<SocialMessageAttributeReport> record_inferred_message_attributes(
        const SocialEnvironmentKey &, const SocialMessageLocator &, const IEventStore &,
        std::span<const SocialMessageAttributeCandidate>, const OperationContext &);
    [[nodiscard]] Result<SocialMessageRelationshipReport> record_inferred_message_relationships(
        const SocialEnvironmentKey &, const SocialMessageLocator &, const IEventStore &,
        std::span<const SocialMessageRelationshipCandidate>, const OperationContext &);
    [[nodiscard]] Result<void> erase_object(const SocialObjectKey &);
    [[nodiscard]] Result<void> erase_environment(const SocialEnvironmentKey &);
    [[nodiscard]] Result<void> close() noexcept;

  private:
    friend Result<SocialMessageRelationshipReport> infer_and_record_social_message_relationships(
        SqliteSocialDirectory &, const IEventStore &, IModelProvider &,
        const SocialEnvironmentKey &, const SocialMessageLocator &, std::span<const std::string>,
        const OperationContext &);
    [[nodiscard]] Result<SocialMessageRelationshipReport> record_message_relationship_candidates(
        const SocialEnvironmentKey &, const SocialMessageLocator &, const IEventStore &,
        std::span<const SocialMessageRelationshipCandidate>, const OperationContext &,
        const SocialMessageEvidence *expected_speaker,
        std::span<const SocialMessageEvidence> history);
    class Impl;
    explicit SqliteSocialDirectory(std::unique_ptr<Impl> impl);
    [[nodiscard]] Result<ConsolidationReport>
    commit_inferred_message_candidates(const SocialEnvironmentKey &,
                                       const SocialMessageParticipants &,
                                       std::span<const SocialMentionReference>,
                                       std::vector<MemoryCandidate>, const OperationContext &);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
