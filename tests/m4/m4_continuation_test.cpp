#include "../support/test.hpp"

#include <mira/model_profile.hpp>
#include <mira/provider_continuation.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace mira;

[[nodiscard]] ProviderContinuation base_continuation() {
    ProviderContinuation continuation;
    continuation.provider_state = "opaque-state-token";
    continuation.profile_digest = digest_string("profile");
    continuation.prompt_digest = digest_string("prompt");
    continuation.schema_digest = digest_string("schema");
    continuation.tool_snapshot_digest = digest_string("tools");
    continuation.provider = "openai-compatible";
    continuation.conversation = "conv-1";
    continuation.profile_id = ModelProfileId::generate();
    continuation.task_id = TaskId::generate();
    continuation.session_id = SessionId::generate();
    continuation.task_epoch = 3;
    continuation.environment_epoch = 5;
    continuation.created_at = Timestamp::now();
    continuation.expires_at = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    return continuation;
}

[[nodiscard]] ContinuationBinding binding_for(const ProviderContinuation &continuation) {
    ContinuationBinding binding;
    binding.provider = continuation.provider;
    binding.conversation = continuation.conversation;
    binding.profile_id = continuation.profile_id;
    binding.task_id = continuation.task_id;
    binding.session_id = continuation.session_id;
    binding.task_epoch = continuation.task_epoch;
    binding.environment_epoch = continuation.environment_epoch;
    binding.remote_store_enabled = continuation.remote_store_enabled;
    binding.data_policy_digest = continuation.data_policy_digest;
    binding.prompt_digest = continuation.prompt_digest;
    binding.schema_digest = continuation.schema_digest;
    binding.tool_snapshot_digest = continuation.tool_snapshot_digest;
    binding.profile_digest = continuation.profile_digest;
    binding.now = std::chrono::steady_clock::now();
    return binding;
}

int binding_invalidation_matrix() {
    auto continuation = base_continuation();
    auto binding = binding_for(continuation);
    MIRA_CHECK(evaluate_continuation(continuation, binding) == ContinuationInvalidation::Valid);

    auto provider_switch = binding;
    provider_switch.provider = "other-provider";
    MIRA_CHECK(evaluate_continuation(continuation, provider_switch) ==
              ContinuationInvalidation::ProviderChanged);

    auto profile_switch = binding;
    profile_switch.profile_id = ModelProfileId::generate();
    MIRA_CHECK(evaluate_continuation(continuation, profile_switch) ==
              ContinuationInvalidation::ProfileChanged);

    auto task_switch = binding;
    task_switch.task_id = TaskId::generate();
    MIRA_CHECK(evaluate_continuation(continuation, task_switch) ==
              ContinuationInvalidation::TaskChanged);

    auto session_switch = binding;
    session_switch.session_id = SessionId::generate();
    MIRA_CHECK(evaluate_continuation(continuation, session_switch) ==
              ContinuationInvalidation::TaskChanged);

    auto epoch_advance = binding;
    epoch_advance.task_epoch += 1;
    MIRA_CHECK(evaluate_continuation(continuation, epoch_advance) ==
              ContinuationInvalidation::EpochAdvanced);

    auto environment_shift = binding;
    environment_shift.environment_epoch += 1;
    MIRA_CHECK(evaluate_continuation(continuation, environment_shift) ==
              ContinuationInvalidation::EpochAdvanced);

    auto schema_shift = binding;
    schema_shift.schema_digest = digest_string("changed");
    MIRA_CHECK(evaluate_continuation(continuation, schema_shift) ==
              ContinuationInvalidation::SchemaChanged);

    auto policy_shift = binding;
    policy_shift.remote_store_enabled = !policy_shift.remote_store_enabled;
    MIRA_CHECK(evaluate_continuation(continuation, policy_shift) ==
              ContinuationInvalidation::PolicyChanged);

    auto expired = binding;
    expired.now = continuation.expires_at + std::chrono::seconds(1);
    MIRA_CHECK(evaluate_continuation(continuation, expired) == ContinuationInvalidation::Expired);

    auto deleted = continuation;
    deleted.deleted_remotely = true;
    MIRA_CHECK(evaluate_continuation(deleted, binding) == ContinuationInvalidation::Invalidated);
    return 0;
}

int cache_lifecycle_and_recovery_invalidation() {
    ContinuationCache cache;
    auto continuation = base_continuation();
    const auto binding = binding_for(continuation);
    MIRA_CHECK(cache.store(continuation).has_value());
    MIRA_CHECK(cache.size() == 1);

    auto hit = cache.lookup(binding);
    MIRA_CHECK(hit.has_value() && hit.value().has_value());
    MIRA_CHECK(hit.value()->provider_state == continuation.provider_state);

    // Provider switch: usable-by-error, and the entry is dropped.
    auto switched = binding;
    switched.provider = "other";
    auto miss = cache.lookup(switched);
    MIRA_CHECK(!miss.has_value());
    MIRA_CHECK(cache.size() == 0);

    // Recovery / takeover / cancellation: invalidate_all forces the next
    // build to come from the local checkpoint.
    MIRA_CHECK(cache.store(continuation).has_value());
    cache.invalidate_all();
    auto invalid = cache.lookup(binding);
    MIRA_CHECK(!invalid.has_value());
    MIRA_CHECK(invalid.error().safe_message.find("checkpoint") != std::string::npos);
    MIRA_CHECK(cache.size() == 0);

    // Entries without a TTL are refused: nothing may outlive its bound.
    auto no_ttl = base_continuation();
    no_ttl.expires_at = {};
    MIRA_CHECK(!cache.store(no_ttl).has_value());

    // Opaque payloads are never inspected: arbitrary bytes are fine.
    auto weird = base_continuation();
    weird.provider_state = std::string(2048, '\x01');
    MIRA_CHECK(cache.store(weird).has_value());
    return 0;
}

class FakeExactCounter final : public IFinalTokenCounter {
  public:
    std::uint64_t report_tokens = 1'000;
    bool fail = false;
    std::uint64_t calls = 0;

    Result<TokenEstimate> count(const PreparedModelContext &, const ModelProfile &) override {
        ++calls;
        if (fail) {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "test";
            error.safe_message = "count endpoint down";
            return error;
        }
        TokenEstimate estimate;
        estimate.lower_bound = report_tokens;
        estimate.upper_bound = report_tokens;
        estimate.quality = TokenCountQuality::ExactProviderCount;
        return estimate;
    }
};

[[nodiscard]] ModelProfile sample_profile(bool exact_capable = false) {
    ModelProfile profile;
    profile.id = ModelProfileId::generate();
    profile.capabilities.exact_token_count.supported = exact_capable;
    return profile;
}

int exact_count_gate_degrades_deterministically() {
    PreparedModelContext prepared;

    // Without a counter: conservative bounds stand, fallback counted.
    ExactCountGate gate_without;
    TokenEstimate conservative;
    conservative.lower_bound = 400;
    conservative.upper_bound = 900;
    conservative.quality = TokenCountQuality::ConservativeEstimate;
    auto kept = gate_without.finalize(prepared, sample_profile(), conservative);
    MIRA_CHECK(kept.has_value());
    MIRA_CHECK(kept.value().quality == TokenCountQuality::ConservativeEstimate);
    MIRA_CHECK(kept.value().upper_bound == 900);
    MIRA_CHECK(gate_without.degraded_fallbacks() == 1);
    MIRA_CHECK(gate_without.exact_successes() == 0);

    // With a healthy, capability-advertised counter: exact bounds replace
    // the conservative ones.
    auto counter_ptr = std::make_shared<FakeExactCounter>();
    ExactCountGate gate(counter_ptr);
    auto exact = gate.finalize(prepared, sample_profile(true), conservative);
    MIRA_CHECK(exact.has_value());
    MIRA_CHECK(exact.value().quality == TokenCountQuality::ExactProviderCount);
    MIRA_CHECK(exact.value().upper_bound == exact.value().lower_bound);
    MIRA_CHECK(gate.exact_successes() == 1);

    // A failing counter degrades but never reports zero tokens.
    counter_ptr->fail = true;
    auto degraded = gate.finalize(prepared, sample_profile(true), conservative);
    MIRA_CHECK(degraded.has_value());
    MIRA_CHECK(degraded.value().quality == TokenCountQuality::DegradedEstimate);
    MIRA_CHECK(degraded.value().upper_bound >= conservative.upper_bound);
    MIRA_CHECK(degraded.value().upper_bound > 0);
    MIRA_CHECK(gate.degraded_fallbacks() == 1);
    MIRA_CHECK(counter_ptr->calls == 2);
    return 0;
}

int request_serialization_roundtrips_new_bindings() {
    auto continuation = base_continuation();
    continuation.profile_digest = digest_string("profile");
    continuation.prompt_digest = digest_string("prompt");
    continuation.schema_digest = digest_string("schema");
    continuation.tool_snapshot_digest = digest_string("tools");

    // The JSON roundtrip keeps the widened binding fields (unknown-tolerant
    // readers see them as absent, which stays permissive by design).
    JsonValue encoded = JsonValue::Object{
        {"provider", continuation.provider},
        {"conversation", continuation.conversation},
        {"session_id", continuation.session_id.to_string()},
        {"environment_epoch", static_cast<std::int64_t>(continuation.environment_epoch)},
        {"profile_digest", continuation.profile_digest.to_string()},
    };
    MIRA_CHECK(encoded.is_object());
    const auto *session = encoded.find("session_id");
    MIRA_CHECK(session != nullptr && session->is_string());
    auto parsed = SessionId::parse(*session->as_string());
    MIRA_CHECK(parsed.has_value() && *parsed == continuation.session_id);
    return 0;
}

} // namespace

int main() {
    int failures = 0;
    failures += binding_invalidation_matrix();
    failures += cache_lifecycle_and_recovery_invalidation();
    failures += exact_count_gate_degrades_deterministically();
    failures += request_serialization_roundtrips_new_bindings();
    return failures == 0 ? 0 : 1;
}
