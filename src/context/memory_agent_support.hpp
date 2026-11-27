#pragma once

#include <mira/memory_agent.hpp>
#include <mira/model_digest.hpp>

#include <kairo/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace mira {
namespace memory_agent_support {
inline Error error(ErrorCode code, const char *message) {
    Error e;
    e.code = code;
    e.domain = "mira.memory_agent";
    e.safe_message = message;
    return e;
}
inline bool same_owner(const OperationContext &context, const AgentLoopSpec &spec) {
    return context.task == spec.task_id && context.session == spec.session_id &&
           context.task_epoch == spec.task_epoch;
}
inline bool request_current(const MemoryWorkBinding &binding) noexcept {
    try {
        return !binding.request_current || binding.request_current();
    } catch (...) {
        return false;
    }
}
[[nodiscard]] JsonValue history_json(const std::vector<ModelInputItem> &history);
[[nodiscard]] bool complete_history(const std::vector<ModelInputItem> &history);
[[nodiscard]] std::string scope_key(const MemoryScope &scope);
[[nodiscard]] SessionId control_partition();
[[nodiscard]] Result<void> validate_proposal(const MemoryAgentProposal &proposal,
                                             const MemoryAgentInput &input, std::size_t bytes);
[[nodiscard]] std::optional<MemoryAgentRecordRejectReason>
record_candidate_rejection(const MemoryAgentMemory &proposal, std::size_t event_count);
class Worker final : public kairo::IBlockingIoWorker {
  public:
    explicit Worker(std::function<void(kairo::StopToken)> run) : run_(std::move(run)) {}
    void run(kairo::StopToken stop) override { run_(stop); }
    void wakeup() noexcept override {} // Provider waits use the bounded OperationContext.
  private:
    std::function<void(kairo::StopToken)> run_;
};
} // namespace memory_agent_support

struct MemoryAgentService::State final : std::enable_shared_from_this<State> {
    struct Work final {
        MemoryWorkBinding binding;
        std::uint64_t revision = 1;
        struct RecordCursor final {
            SessionSequence through = 0;
            std::optional<SessionSequence> snapshot;
        };
        std::map<std::string, RecordCursor> record_cursors;
        bool active = true;
        bool revoked = false;
        bool input_pending = false;
        std::vector<ContextItem> candidates;
        std::vector<MemoryUseCandidate> memory_candidates;
        std::chrono::system_clock::time_point cache_until{};
        std::vector<ModelInputItem> prefix;
        std::vector<ModelInputItem> compacted;
        std::vector<ModelInputItem> captured;
    };
    struct Job final {
        OperationId id = OperationId::generate();
        MemoryAgentRequest request;
        MemoryWorkBinding binding;
        std::uint64_t revision = 0;
        std::uint64_t after_sequence = 0;
        std::optional<SessionSequence> record_snapshot;
        bool record_blocked = false;
        OperationContext context;
        std::atomic<bool> cancelled{false};
        bool done = false;
        bool claimed = false;
        std::thread::id worker_id;
        std::optional<kairo::WorkerHandle> worker;
        std::promise<Result<MemoryAgentResult>> promise;
        std::shared_future<Result<MemoryAgentResult>> future = promise.get_future().share();
    };
    State(kairo::Executor &ex, std::shared_ptr<IMemory> store, std::shared_ptr<IEventStore> log,
          RuntimeId id, MemoryAgentProcessor model, MemoryAgentConfig bounds)
        : executor(ex), memory(std::move(store)), events(std::move(log)), runtime(id),
          processor(std::move(model)), config(std::move(bounds)) {}
    kairo::Executor &executor;
    std::shared_ptr<IMemory> memory;
    std::shared_ptr<IEventStore> events;
    RuntimeId runtime;
    MemoryAgentProcessor processor;
    MemoryAgentConfig config;
    mutable std::mutex mutex;
    // Serializes version checks with mutation/erase; never held across models.
    std::mutex commit_mutex;
    std::mutex close_mutex;
    std::condition_variable cv;
    std::map<TaskId, Work> works;
    std::map<OperationId, std::shared_ptr<Job>> jobs;
    bool closing = false;
    bool initialized = false;
    std::set<std::string> blocked_scopes;
    struct RecordForget final {
        std::string scope;
        Sha256Digest identity;
        std::vector<MutationId> preferences;
        bool durable = false;
        bool complete = false;
        bool target_changed = false;
    };
    // Receipts use commit_mutex; the temporary holds also use mutex for admission.
    std::map<OperationId, RecordForget> record_forgets;
    std::map<std::string, OperationId> record_forget_holds;
    [[nodiscard]] Result<MemoryWorkBinding> record_intent_binding(const PrincipalContext &,
                                                                  const MemoryScope &,
                                                                  const OperationContext &) const;
    [[nodiscard]] bool record_forget_held(const MemoryWorkBinding &) const; // mutex held
    [[nodiscard]] Result<bool> restore_record_forget(const EventEnvelope &);
    [[nodiscard]] Result<void> clean_record_forget(const RecordForget &);
    struct Projection final {
        ArtifactDescriptor artifact;
        std::vector<std::string> scopes;
    };
    std::map<ArtifactId, Projection> projections;
    std::optional<Error> audit_error;
    // Optional recall embedder (DEC-111); guarded by mutex and frozen at
    // initialize() so jobs never observe a mid-flight producer swap.
    std::shared_ptr<IEmbeddingProvider> recall_embedder;
    struct PreferenceEntry final {
        PreferenceProposal proposal;
        PrincipalContext owner; // Only identity/version populated on replay; never grants.
        Sha256Digest request_digest;
        ArtifactDescriptor artifact;
    };
    // Protected by commit_mutex; the initializer also holds mutex. Bounded
    // receipts are projections of the control stream, not a second database.
    std::map<MutationId, PreferenceEntry> preference_entries;
    std::optional<Error> preference_fence;
    [[nodiscard]] Result<void> preference_available(const OperationContext &) const;
    [[nodiscard]] Result<void> append_preference(const char *, JsonValue::Object,
                                                 EventId = EventId{});
    [[nodiscard]] Result<bool> restore_preference(const EventEnvelope &);
    [[nodiscard]] Result<PreferenceProposal> read_preference(const PreferenceEntry &,
                                                             bool for_erasure = false) const;
    [[nodiscard]] Result<void> erase_preference_bodies(const std::string &, bool privacy);
    [[nodiscard]] Result<void> invalidate_preference_proposals(const MemoryScope &);
    [[nodiscard]] Result<void> clean_preference_bodies();
    [[nodiscard]] Result<void> clean_preference_body(const PreferenceEntry &);
    struct PreferenceIntentAuthority final {
        const PrincipalContext &principal;
        PreferenceIntent intent;
        std::optional<ControlIngressId> input_source;
    };
    [[nodiscard]] Result<PreferenceProposal>
    propose_preference(PreferenceProposalRequest, const OperationContext &,
                       std::optional<PreferenceIntentAuthority> = std::nullopt);
    [[nodiscard]] Result<PreferenceProposal> decide_preference(const PrincipalContext &, MutationId,
                                                               Sha256Digest, ConfirmationDecision,
                                                               const OperationContext &);

    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<void> validate_binding(const MemoryWorkBinding &) const;
    [[nodiscard]] Result<void> emit(const Job &job, const char *type,
                                    const MemoryAgentResult *result = nullptr);
    [[nodiscard]] Result<void> restore_record_cursor(Job &);
    [[nodiscard]] Result<void> save_record_cursor(const Job &, const Work::RecordCursor &,
                                                  const MemoryAgentResult &);
    [[nodiscard]] bool current(const Job &job) const; // mutex held
    void invalidate(Work &work);                      // mutex held
    [[nodiscard]] Result<MemoryAgentHandle> submit(MemoryAgentRequest request,
                                                   OperationContext context);
    void run(const std::shared_ptr<Job> &job, kairo::StopToken stop);
    [[nodiscard]] Result<MemoryAgentResult> execute(const std::shared_ptr<Job> &job,
                                                    const OperationContext &context);
    [[nodiscard]] Result<std::optional<MemoryAgentResult>> result(OperationId id) const;
    [[nodiscard]] Result<ArtifactDescriptor> store_projection(const Job &job,
                                                              const MemoryAgentInput &input,
                                                              const MemoryAgentProposal &proposal);
    [[nodiscard]] Result<void> restore_projection(const JsonValue &payload);
    [[nodiscard]] Result<void> erase_projections(const std::string &scope);
};

[[nodiscard]] BuiltinToolRegistration preference_proposal_tool(
    std::function<Result<PreferenceProposal>(PreferenceProposalRequest, const OperationContext &)>);
} // namespace mira
