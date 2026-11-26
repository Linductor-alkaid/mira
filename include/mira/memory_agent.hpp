#pragma once

#include <mira/conversation_loop.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_embedding.hpp>
#include <mira/model_context_admission.hpp>
#include <mira/security.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kairo {
class Executor;
}
namespace mira {
class IUserInputSink;
class IConversationContextIndex;

// Stable internal scope mapping, domain-separated by tenant and identity.
[[nodiscard]] MemoryScope memory_user_scope(const PrincipalContext &principal);
[[nodiscard]] MemoryScope memory_session_scope(const PrincipalContext &principal,
                                               SessionId session);
[[nodiscard]] MemoryScope memory_application_scope(const PrincipalContext &principal,
                                                   std::string_view application_id);
[[nodiscard]] Sha256Digest memory_erasure_scope_digest(const MemoryScope &scope);

enum class MemoryAgentMode : std::uint8_t { Recall, Record, Compress };
inline constexpr std::size_t kMaxRecordPagesPerJob = 8;

enum class PreferenceProposalState : std::uint8_t {
    Pending,
    Approving,
    Applied,
    NoChange,
    Rejected,
    Stale,
    Erased,
    Expired
};
struct PreferenceProposalRequest final {
    MutationId id;
    std::string statement;
    std::optional<MemoryId> target;
    std::optional<std::uint64_t> expected_version;
};
struct PreferenceProposal final {
    MutationId id;
    Sha256Digest digest;
    MemoryScope scope;
    PreferenceProposalState state = PreferenceProposalState::Pending;
    WallTimePoint expires_at;
    std::string statement;
    std::optional<MemoryRecord> previous;
    std::optional<MemoryMutation> mutation;
    std::optional<MemoryMutationResult> applied;
};

struct MemoryWorkBinding final {
    PrincipalContext principal;
    AgentLoopSpec work;
    // Host-authorized exact allowlist, tenant-bound; no model scope strings.
    std::vector<MemoryScope> scopes;
    std::uint64_t environment_epoch = 0;
    // Optional trusted host probe: bounded, non-reentrant, no I/O. A false
    // result/exception closes request admission, including cached snapshots.
    // Record after settlement is independent of this request-only probe.
    std::function<bool()> request_current;
    std::vector<std::string> user_constraints;
    // Any-of label prefilter applied to every Recall query of this work
    // (DEC-112 metadata): only records carrying at least one of these labels
    // participate. Host-defined data tags; bounds mirror MemoryQuery::labels.
    std::vector<std::string> labels;
};

struct MemoryAgentNote final {
    std::string text;
    // Index into source events (Record) or captured prefix items (Compress).
    std::vector<std::size_t> citations;
};
struct MemoryAgentMemory final {
    MemoryKind kind = MemoryKind::Preference;
    MemoryAgentNote note;
    std::optional<MemoryClaim> claim{};
};
enum class MemoryAgentRecordRejectReason : std::uint8_t {
    UnknownKind,
    InvalidNote,
    InvalidClaim,
    InvalidCitation,
};
struct MemoryAgentRecordRejection final {
    std::size_t row = 0; // Zero-based within the original proposal page.
    MemoryAgentRecordRejectReason reason = MemoryAgentRecordRejectReason::InvalidNote;
};
struct MemoryAgentProposal final {
    // Recall: optional subset of the supplied records; empty retains all.
    std::vector<MemoryId> selected_memories;
    // Recall: at most four additional scoped lexical queries.
    std::vector<std::string> queries;
    std::vector<MemoryAgentMemory> memories;
    std::vector<MemoryAgentNote> summary;
    std::size_t rejected_record_rows = 0;
    // Optional detail for the rows already counted above. The other raw rows
    // correspond to memories in order.
    std::vector<MemoryAgentRecordRejection> record_rejections{};
};

struct MemoryAgentInput final {
    MemoryAgentMode mode = MemoryAgentMode::Recall;
    AgentLoopSpec work;
    std::vector<MemoryRecord> records;
    std::vector<EventEnvelope> events;
    std::vector<ModelInputItem> history;
    std::vector<std::string> user_constraints;
};
// Runs on the service's managed I/O worker, with cooperative cancellation.
// Output is only a proposal. No direct storage/role/authority capabilities.
using MemoryAgentProcessor =
    std::function<Result<MemoryAgentProposal>(const MemoryAgentInput &, const OperationContext &)>;

struct MemoryAgentModelConfig final {
    ModelProfile model;
    // Exclusive provider per invocation. Must cooperate with context timeout.
    std::function<Result<std::unique_ptr<IModelProvider>>(const OperationContext &)>
        create_provider;
    std::uint64_t max_output_tokens = 2'048;
};
// Real isolated no-tool ConversationLoop with a host-owned system template;
// every invocation has fresh inference state and the ordinary Gateway gate.
[[nodiscard]] MemoryAgentProcessor make_model_memory_agent(kairo::Executor &executor,
                                                           MemoryAgentModelConfig config);

struct MemoryAgentRequest final {
    MemoryAgentMode mode = MemoryAgentMode::Recall;
    TaskId task;
    std::string query;
    // Record only; must belong to the work's exact allowlist.
    std::optional<MemoryScope> write_scope;
    // Compress: only the settled prefix is eligible; protect at least the
    // last item. Complete native tool groups must not straddle the cut.
    std::vector<ModelInputItem> history;
    std::size_t protected_items = 1;
    // Foreground jobs may flow back through continuation_source().
    bool foreground = false;
    std::optional<std::uint64_t> expected_revision;
    // Record only. Additional pages share the first page's event snapshot and deadline.
    std::size_t record_page_limit = 1;
};

struct MemoryAgentRecordPageReport final {
    SessionSequence after = 0;
    SessionSequence through = 0;
    std::vector<MemoryAgentRecordRejection> rejected;
};

struct MemoryAgentResult final {
    OperationId operation;
    MemoryAgentMode mode = MemoryAgentMode::Recall;
    std::uint64_t revision = 0;
    std::uint64_t through_event_sequence = 0;
    bool more_pending = false;
    bool degraded = false;
    std::vector<ContextItem> candidates;
    ConsolidationReport consolidation;
    std::size_t compressed_items = 0;
    std::optional<ArtifactDescriptor> projection_artifact;
    bool deterministic_record_only = false;
    std::size_t rejected_record_rows = 0;
    std::vector<MemoryAgentRecordPageReport> record_pages{};
};
struct MemoryAgentHandle final {
    OperationId operation;
    // Submission is not success. Host must consume this result.
    std::shared_future<Result<MemoryAgentResult>> result;
};

// A host-configured candidate pool for Recall. Each kind may appear in only
// one group. Limits apply across the initial query and all follow-up queries.
struct MemoryRecallGroup final {
    std::vector<MemoryKind> kinds;
    std::size_t max_records = 0;
    std::size_t max_bytes = 0; // Includes the framed context item.
};

struct MemoryAgentConfig final {
    std::size_t max_works = 64;
    std::size_t max_jobs = 64;
    std::size_t max_records = 16;
    std::size_t max_events = 128;
    std::size_t max_history_items = 128;
    std::size_t max_source_bytes = 128 * 1024;
    std::size_t max_result_bytes = 32 * 1024;
    std::chrono::milliseconds timeout{30'000};
    std::chrono::seconds cache_ttl{60};
    // Required for Compress: source prefix and cited proposal are preserved
    // outside events. Host must apply the same ACL as the work's scopes.
    std::shared_ptr<IArtifactStore> artifacts;
    std::size_t max_projection_artifacts = 512;
    ConsolidationPolicy consolidation;
    std::size_t max_preference_proposals = 256;
    std::chrono::seconds preference_proposal_ttl{std::chrono::hours(24)};
    // Empty preserves the existing unfiltered recall. Nonempty groups run in
    // order within the same work scopes. Their limits must fit the job limits.
    std::vector<MemoryRecallGroup> recall_groups;
    // Optional durable participant in session-scope erasure and startup replay.
    std::shared_ptr<IConversationContextIndex> conversation_index;
    // Recover Record page progress per work and exact write scope. Requires
    // an indexed, ProcessCrash-capable event store and one owner per work.
    bool durable_record_pages = false;
};

// DEC-101: host owner, not a perpetual child of the frontend run. Stores and
// Executor outlive close(). Registration/control methods are trusted host API;
// tools derive identity from an already registered OperationContext instead.
// Configure work bindings before exposing tools. No implicit scope expansion.
class MemoryAgentService final {
  public:
    MemoryAgentService(kairo::Executor &executor, std::shared_ptr<IMemory> memory,
                       std::shared_ptr<IEventStore> events, RuntimeId runtime,
                       MemoryAgentProcessor processor = {}, MemoryAgentConfig config = {});
    ~MemoryAgentService();
    MemoryAgentService(const MemoryAgentService &) = delete;
    MemoryAgentService &operator=(const MemoryAgentService &) = delete;

    // Rebuild durable scope erasure barriers before admission. Erased scopes
    // cannot be rebound/re-extracted, including after process restart.
    [[nodiscard]] Result<void> initialize();

    // Opt-in semantic prior for Recall queries (DEC-111): before initialize(),
    // the host may inject an embedding producer. Recall then embeds the query
    // text once per query; an embed failure (timeout included) degrades the
    // vector leg and marks the result degraded — it never fails the recall.
    // The producer is invoked on the job's Executor worker; the transport's
    // deadlines bound the blocking call.
    [[nodiscard]] Result<void> set_recall_embedder(std::shared_ptr<IEmbeddingProvider> embedder);
    [[nodiscard]] Result<void> bind(MemoryWorkBinding binding);
    // DEC-107: atomically bind/update a work frame and suspend candidates
    // until the entire corresponding input queue has been accepted. Existing
    // identity/session/scopes are immutable; retired/revoked work stays closed.
    [[nodiscard]] Result<std::uint64_t> stage_input(MemoryWorkBinding binding);
    [[nodiscard]] Result<void> acknowledge_input(TaskId task, std::uint64_t revision);
    // Increments the revision, cancels old work and invalidates projections.
    [[nodiscard]] Result<void> revise(TaskId task, std::uint64_t task_epoch,
                                      std::uint64_t environment_epoch);
    // Terminal: request projection closes; Record remains explicitly allowed.
    [[nodiscard]] Result<void> retire(TaskId task);
    // Takeover/revocation: closes every mode including Record.
    [[nodiscard]] Result<void> revoke(TaskId task);
    // Permanently relinquish a retired/revoked binding once its work has
    // settled. Keeps scope erasure barriers and any outstanding result futures.
    // The trusted host must never reuse TaskIds. Pending jobs reject release.
    [[nodiscard]] Result<void> release_work(TaskId task);
    // Must precede out-of-service mutations; invalidates all dependent work.
    [[nodiscard]] Result<void> invalidate_scope(const MemoryScope &scope);
    [[nodiscard]] Result<ErasureResult> erase_scope(const MemoryScope &scope);

    // DEC-109: finite synchronous I/O; invoke via the injected Executor and
    // consume the result. Propose is bound to an active work; preview/decide
    // require a current mira.memory.manage grant and an exact owner/digest.
    // No model tool can approve. Approval authorizes storage, not a claim of
    // HumanConfirmed truth; all writes use consolidation/apply_pending.
    [[nodiscard]] Result<PreferenceProposal> propose_preference(PreferenceProposalRequest,
                                                                const OperationContext &);
    [[nodiscard]] Result<PreferenceProposal>
    preference_proposal(const PrincipalContext &, MutationId, const OperationContext &) const;
    [[nodiscard]] Result<PreferenceProposal> decide_preference(const PrincipalContext &, MutationId,
                                                               Sha256Digest, ConfirmationDecision,
                                                               const OperationContext &);

    [[nodiscard]] Result<MemoryAgentHandle> submit(MemoryAgentRequest request,
                                                   OperationContext context);
    [[nodiscard]] Result<std::optional<MemoryAgentResult>> result(OperationId operation) const;
    [[nodiscard]] Result<void> cancel(OperationId operation);
    // External-owner maintenance: consume settled futures and join/release
    // their worker handles. A caller's shared_future remains usable.
    [[nodiscard]] Result<std::size_t> reap();
    [[nodiscard]] Result<void> close(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Wire to Gateway and ConversationLoop respectively. Bounded, nonblocking
    // cache reads; capture no raw pointer to this service.
    [[nodiscard]] ModelContextSupplier context_supplier() const;
    [[nodiscard]] HistoryTransformer history_transformer(TaskId task) const;
    [[nodiscard]] ConversationContinuationSource continuation_source(TaskId task) const;
    // Decorates the existing coordinator -> Loop input sink. Invalidation
    // precedes delivery; failed delivery keeps request admission suspended
    // until the same routed input can be delivered successfully.
    [[nodiscard]] std::shared_ptr<IUserInputSink>
    input_sink(TaskId task, std::shared_ptr<IUserInputSink> downstream) const;
    // memory_start / memory_result / memory_cancel, all bounded/nonblocking.
    [[nodiscard]] std::vector<BuiltinToolRegistration> tools() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace mira
