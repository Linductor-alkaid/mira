#pragma once

#include <mira/context_contracts.hpp>
#include <mira/core_contracts.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_profile.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mira {

// ---------------------------------------------------------------------------
// Provider opaque continuation lifecycle (M4-14)
// ---------------------------------------------------------------------------

// Why a continuation stopped being usable. Codes are the contract surface.
enum class ContinuationInvalidation : std::uint8_t {
    Valid,
    Expired,          // TTL elapsed
    ProviderChanged,  // different provider backend
    ProfileChanged,   // different profile id or manifest digest
    ConversationChanged,
    TaskChanged,   // task/session binding mismatch
    EpochAdvanced, // task or environment epoch moved past the binding
    SchemaChanged, // prompt/decision schema or tool snapshot digest mismatch
    PolicyChanged, // data policy or remote-store mode mismatch
    Invalidated,   // cancel, takeover, recovery or remote deletion
};

[[nodiscard]] std::string continuation_invalidation_name(ContinuationInvalidation reason);

// The request side of the binding check. Fields left at their empty/zero
// defaults are "not asserted" and never invalidate a legacy continuation.
struct ContinuationBinding final {
    std::string provider;
    std::string conversation;
    ModelProfileId profile_id;
    Hash profile_digest{};
    TaskId task_id;
    SessionId session_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    Hash prompt_digest{};
    Hash schema_digest{};
    Hash tool_snapshot_digest{};
    std::string data_policy_digest;
    bool remote_store_enabled = false;
    // Monotonic clock reading for TTL checks (steady, like the payload).
    std::chrono::steady_clock::time_point now{};
};

// Evaluates a continuation against a binding. Never inspects the payload;
// the opaque state is only ever compared by its recorded bindings.
[[nodiscard]] ContinuationInvalidation
evaluate_continuation(const ProviderContinuation &continuation,
                      const ContinuationBinding &binding);

// In-process continuation registry. All state stays rebuildable from local
// checkpoints; recovery clears the cache instead of trusting opaque bytes
// across process boundaries (design Context/Memory §15.2).
class ContinuationCache final {
  public:
    ContinuationCache() = default;
    ContinuationCache(const ContinuationCache &) = delete;
    ContinuationCache &operator=(const ContinuationCache &) = delete;

    // Stores one continuation keyed by (task, conversation-or-provider-state).
    // Requires provider identity, a live task binding and a real TTL.
    Result<void> store(ProviderContinuation continuation);

    // Returns the continuation when it is still valid for the binding;
    // otherwise an error describing the invalidation. Expired and invalidated
    // entries are dropped.
    Result<std::optional<ProviderContinuation>>
    lookup(const ContinuationBinding &binding) const;

    // Cancel, takeover and process recovery paths: every cached continuation
    // becomes unusable; the next build must come from the local checkpoint.
    void invalidate_all();

    [[nodiscard]] std::size_t size() const;

  private:
    struct Entry final {
        ProviderContinuation continuation;
        bool invalidated = false;
    };
    struct CacheState final {
        std::mutex mutex;
        std::map<std::pair<std::string, std::string>, Entry> entries;
    };

    mutable std::shared_ptr<CacheState> state_ = std::make_shared<CacheState>();
};

// ---------------------------------------------------------------------------
// Provider exact token count (M4-14)
// ---------------------------------------------------------------------------

// True when the profile advertises the exact_token_count capability with at
// least configured evidence (design Context/Memory §15.1). Routing never
// depends on this; it only gates the final-draft exact count attempt.
[[nodiscard]] bool profile_supports_exact_count(const ModelProfile &profile);

// Counts one fully assembled PreparedModelContext (the final draft). This is
// distinct from M4-02's per-item IExactTokenCounter: adapters may implement
// both; calls may do network work and honour cancellation at the adapter.
class IFinalTokenCounter {
  public:
    virtual ~IFinalTokenCounter() = default;
    [[nodiscard]] virtual Result<TokenEstimate>
    count(const PreparedModelContext &prepared, const ModelProfile &profile) = 0;
};

// Exact-count gate with deterministic degradation: when the capability is
// absent, no counter is attached, or the count fails, the conservative
// estimate is kept (never zero) and the fallback is counted for telemetry.
// A successful exact count replaces the bounds with equal lower/upper values.
class ExactCountGate final {
  public:
    explicit ExactCountGate(std::shared_ptr<IFinalTokenCounter> counter = nullptr);

    [[nodiscard]] Result<TokenEstimate>
    finalize(const PreparedModelContext &prepared, const ModelProfile &profile,
             const TokenEstimate &conservative);

    [[nodiscard]] std::uint64_t exact_successes() const noexcept;
    [[nodiscard]] std::uint64_t degraded_fallbacks() const noexcept;

  private:
    std::shared_ptr<IFinalTokenCounter> counter_;
    std::atomic<std::uint64_t> exact_successes_{0};
    std::atomic<std::uint64_t> degraded_fallbacks_{0};
};

} // namespace mira
