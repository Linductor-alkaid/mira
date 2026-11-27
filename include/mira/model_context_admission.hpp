#pragma once

#include <mira/context_manager.hpp>
#include <mira/model_profile.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace mira {

struct MemoryUseReference final {
    Sha256Digest scope_digest;
    MemoryId record;
    std::uint64_t version = 0;
    friend bool operator==(const MemoryUseReference &, const MemoryUseReference &) = default;
};

struct MemoryUseCandidate final {
    ContextItemId item;
    std::vector<MemoryUseReference> records;
};

// DEC-101: immutable, request-scoped optional data. A supplier must perform
// only bounded cache reads. Never fetch models/storage on the infer caller.
struct ModelContextSnapshot final {
    std::vector<ContextItem> candidates;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    // A false probe invalidates dispatch and any late model completion.
    std::function<bool()> current;
    // Optional: the same task identity remains active and may rebuild after
    // a revision/expiry. False for terminal, takeover, deletion or pending input.
    std::function<bool()> refreshable;
    // Wire-role replay items may precede the first original User message.
    // Other candidates retain their normal trailing position.
    bool prepend_history = false;
    // Metadata paired with optional RetrievedMemory items. It stays local.
    std::vector<MemoryUseCandidate> memory_candidates{};
};
using ModelContextSupplier =
    std::function<Result<ModelContextSnapshot>(const ModelRequest &, const OperationContext &)>;

struct ModelContextAdmissionConfig final {
    std::uint64_t safety_margin_tokens = 1'024;
    std::size_t max_candidate_items = 64;
    std::size_t max_candidate_bytes = 64 * 1024;
    // Default is one byte/token, with provisional image/file estimates.
    // Hosts may supply a calibrated profile-specific counter. Not a claim
    // of exact tokenizer or vision-cost support for every provider.
    std::shared_ptr<const ITokenCounter> counter;
};

struct AdmittedModelRequest final {
    ModelRequest request;
    PreparedModelContext context;
    std::vector<MemoryUseReference> selected_memory{};
};

// Accounts for the entire selected request and output/schema overhead using
// Layer 0. Original messages (including native tool/thinking replay) retain
// their exact envelope/order and are protected. Optional P3/P4 text remains
// User/Public data. Tool truncation fails closed, never silently changes the
// caller's selected tools. Called separately for each provider profile.
[[nodiscard]] Result<AdmittedModelRequest>
admit_model_request(const ModelRequest &request, const ModelProfile &profile, SessionId session,
                    const ModelContextSnapshot &snapshot,
                    const ModelContextAdmissionConfig &config = {});

} // namespace mira
