#include <mira/provider_continuation.hpp>

#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error continuation_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.provider_continuation";
    error.safe_message = std::move(message);
    return error;
}

} // namespace

std::string continuation_invalidation_name(ContinuationInvalidation reason) {
    switch (reason) {
    case ContinuationInvalidation::Valid:
        return "Valid";
    case ContinuationInvalidation::Expired:
        return "Expired";
    case ContinuationInvalidation::ProviderChanged:
        return "ProviderChanged";
    case ContinuationInvalidation::ProfileChanged:
        return "ProfileChanged";
    case ContinuationInvalidation::ConversationChanged:
        return "ConversationChanged";
    case ContinuationInvalidation::TaskChanged:
        return "TaskChanged";
    case ContinuationInvalidation::EpochAdvanced:
        return "EpochAdvanced";
    case ContinuationInvalidation::SchemaChanged:
        return "SchemaChanged";
    case ContinuationInvalidation::PolicyChanged:
        return "PolicyChanged";
    case ContinuationInvalidation::Invalidated:
        return "Invalidated";
    }
    return "Unknown";
}

ContinuationInvalidation evaluate_continuation(const ProviderContinuation &continuation,
                                               const ContinuationBinding &binding) {
    if (continuation.deleted_remotely) {
        return ContinuationInvalidation::Invalidated;
    }
    if (!binding.provider.empty() && !continuation.provider.empty() &&
        continuation.provider != binding.provider) {
        return ContinuationInvalidation::ProviderChanged;
    }
    if (!(continuation.profile_id == binding.profile_id)) {
        return ContinuationInvalidation::ProfileChanged;
    }
    if (!(continuation.profile_digest == Hash{}) && !(continuation.profile_digest == binding.profile_digest)) {
        return ContinuationInvalidation::ProfileChanged;
    }
    if (!binding.conversation.empty() && !continuation.conversation.empty() &&
        continuation.conversation != binding.conversation) {
        return ContinuationInvalidation::ConversationChanged;
    }
    if (!(continuation.task_id == binding.task_id)) {
        return ContinuationInvalidation::TaskChanged;
    }
    if (!continuation.session_id.is_nil() && !(continuation.session_id == binding.session_id)) {
        return ContinuationInvalidation::TaskChanged;
    }
    if (continuation.task_epoch != binding.task_epoch) {
        return ContinuationInvalidation::EpochAdvanced;
    }
    if (continuation.environment_epoch != 0 &&
        continuation.environment_epoch != binding.environment_epoch) {
        return ContinuationInvalidation::EpochAdvanced;
    }
    if (!(continuation.prompt_digest == Hash{}) &&
        !(continuation.prompt_digest == binding.prompt_digest)) {
        return ContinuationInvalidation::SchemaChanged;
    }
    if (!(continuation.schema_digest == Hash{}) &&
        !(continuation.schema_digest == binding.schema_digest)) {
        return ContinuationInvalidation::SchemaChanged;
    }
    if (!(continuation.tool_snapshot_digest == Hash{}) &&
        !(continuation.tool_snapshot_digest == binding.tool_snapshot_digest)) {
        return ContinuationInvalidation::SchemaChanged;
    }
    if (!continuation.data_policy_digest.empty() &&
        continuation.data_policy_digest != binding.data_policy_digest) {
        return ContinuationInvalidation::PolicyChanged;
    }
    if (continuation.remote_store_enabled != binding.remote_store_enabled) {
        return ContinuationInvalidation::PolicyChanged;
    }
    if (continuation.expires_at.time_since_epoch().count() == 0 ||
        binding.now >= continuation.expires_at) {
        return ContinuationInvalidation::Expired;
    }
    return ContinuationInvalidation::Valid;
}

Result<void> ContinuationCache::store(ProviderContinuation continuation) {
    if (continuation.provider_state.empty()) {
        return continuation_error(ErrorCode::InvalidArgument,
                                  "continuation requires provider state");
    }
    if (continuation.provider.empty()) {
        return continuation_error(ErrorCode::InvalidArgument,
                                  "cached continuations must assert their provider identity");
    }
    if (continuation.task_id.is_nil()) {
        return continuation_error(ErrorCode::InvalidArgument,
                                  "continuation must bind a task");
    }
    if (continuation.expires_at.time_since_epoch().count() == 0) {
        return continuation_error(ErrorCode::InvalidArgument,
                                  "cached continuations require a TTL");
    }
    std::lock_guard lock(state_->mutex);
    const auto key = std::make_pair(continuation.task_id.to_string(),
                                    continuation.conversation.empty()
                                        ? continuation.provider_state
                                        : continuation.conversation);
    state_->entries.insert_or_assign(key, Entry{std::move(continuation), false});
    return Result<void>{};
}

Result<std::optional<ProviderContinuation>>
ContinuationCache::lookup(const ContinuationBinding &binding) const {
    std::lock_guard lock(state_->mutex);
    const auto key = std::make_pair(
        binding.task_id.to_string(),
        binding.conversation.empty() ? std::string{} : binding.conversation);
    const auto found = state_->entries.find(key);
    if (found == state_->entries.end()) {
        return std::optional<ProviderContinuation>{};
    }
    if (found->second.invalidated) {
        state_->entries.erase(found);
        return continuation_error(ErrorCode::InvalidState,
                                  "continuation was invalidated; rebuild from the checkpoint");
    }
    const auto verdict = evaluate_continuation(found->second.continuation, binding);
    if (verdict != ContinuationInvalidation::Valid) {
        state_->entries.erase(found);
        return continuation_error(
            ErrorCode::InvalidState,
            "continuation unusable: " + continuation_invalidation_name(verdict));
    }
    return std::optional<ProviderContinuation>(found->second.continuation);
}

void ContinuationCache::invalidate_all() {
    std::lock_guard lock(state_->mutex);
    for (auto &[key, entry] : state_->entries) {
        entry.invalidated = true;
    }
}

std::size_t ContinuationCache::size() const {
    std::lock_guard lock(state_->mutex);
    return state_->entries.size();
}

bool profile_supports_exact_count(const ModelProfile &profile) {
    const auto &flag = profile.capabilities.exact_token_count;
    return flag.supported;
}

ExactCountGate::ExactCountGate(std::shared_ptr<IFinalTokenCounter> counter)
    : counter_(std::move(counter)) {}

Result<TokenEstimate> ExactCountGate::finalize(const PreparedModelContext &prepared,
                                               const ModelProfile &profile,
                                               const TokenEstimate &conservative) {
    if (counter_ == nullptr || !profile_supports_exact_count(profile)) {
        degraded_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        return conservative; // no capability: conservative bounds stand
    }
    auto exact = counter_->count(prepared, profile);
    if (!exact || exact.value().upper_bound == 0) {
        // A failed or nonsensical exact count degrades; it must never be
        // recorded as zero tokens (design Context/Memory §7.2).
        degraded_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        TokenEstimate degraded = conservative;
        degraded.quality = TokenCountQuality::DegradedEstimate;
        return degraded;
    }
    exact_successes_.fetch_add(1, std::memory_order_relaxed);
    return exact;
}

std::uint64_t ExactCountGate::exact_successes() const noexcept {
    return exact_successes_.load(std::memory_order_relaxed);
}

std::uint64_t ExactCountGate::degraded_fallbacks() const noexcept {
    return degraded_fallbacks_.load(std::memory_order_relaxed);
}

} // namespace mira
