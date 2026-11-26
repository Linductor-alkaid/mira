#pragma once

#include <mira/context_retrieval.hpp>
#include <mira/environment.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace kairo {
class Executor;
}

namespace mira {

struct SqliteConversationIndexOptions final {
    std::filesystem::path path;
    std::size_t max_sessions = 128;
    std::size_t max_segments = 4096;
    std::size_t max_erasure_barriers = 4096;
    // New events and payload bytes allowed per update, independent of history length.
    std::size_t max_source_events = 4096;
    std::size_t max_source_bytes = 4 * 1024 * 1024;
    std::size_t max_pending_requests = 32;
    std::chrono::milliseconds operation_timeout{30000};
    ConversationSegmentationOptions segmentation;
    // Invoked on the calling worker at the first rebuild/advance capacity failure.
    std::function<void(const Error &)> capacity_warning{};
};

struct ConversationIndexProgress final {
    SessionSequence through = 0;
    SessionSequence snapshot_through = 0;
};

// A rebuildable SQLite/FTS5 projection for authenticated session history.
// The host owns source authorization, calls rebuild on managed I/O work,
// and closes the index before shutting down its Executor. A query with text
// comes back most relevant first; without one, newest first (DEC-147).
class SqliteConversationIndex final : public IConversationContextIndex {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteConversationIndex>>
    open(kairo::Executor &executor, SqliteConversationIndexOptions options);
    ~SqliteConversationIndex() override;

    SqliteConversationIndex(const SqliteConversationIndex &) = delete;
    SqliteConversationIndex &operator=(const SqliteConversationIndex &) = delete;

    // context.session must identify the authorized target session. A missing
    // binding is InvalidArgument; a different binding is PermissionDenied.
    [[nodiscard]] Result<SessionSequence> rebuild(const TenantId &tenant, SessionId session,
                                                  const IEventStore &source,
                                                  const OperationContext &context);
    // Publish the next bounded prefix of the current snapshot.
    [[nodiscard]] Result<ConversationIndexProgress>
    advance(const TenantId &tenant, SessionId, const IEventStore &, const OperationContext &);
    [[nodiscard]] Result<ContextRetrievalResult> retrieve(const ContextQuery &query,
                                                          const RetrievalBudget &budget) override;
    [[nodiscard]] Result<void> erase_scope(const Sha256Digest &) override;
    [[nodiscard]] Result<void> close() noexcept;

  private:
    [[nodiscard]] Result<ConversationIndexProgress> update(const TenantId &, SessionId,
                                                           const IEventStore &,
                                                           const OperationContext &, bool partial);
    class Impl;
    explicit SqliteConversationIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
