#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Versioned Context/Checkpoint/Memory public contracts (M4-01)
// ---------------------------------------------------------------------------

#define MIRA_DEFINE_CONTEXT_ID(name)                                                               \
    struct name final {                                                                            \
        Id128 value{};                                                                             \
        static name generate() { return name{Id128::generate()}; }                                 \
        static std::optional<name> parse(std::string_view text) noexcept {                         \
            const auto parsed = Id128::parse(text);                                                \
            return parsed ? std::optional<name>(name{*parsed}) : std::nullopt;                     \
        }                                                                                          \
        [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }                      \
        [[nodiscard]] std::string to_string() const { return value.to_string(); }                  \
        friend constexpr bool operator==(const name &, const name &) noexcept = default;           \
        friend constexpr auto operator<=>(const name &, const name &) noexcept = default;          \
    }

MIRA_DEFINE_CONTEXT_ID(ContextItemId);
MIRA_DEFINE_CONTEXT_ID(CheckpointId);
MIRA_DEFINE_CONTEXT_ID(MemoryId);
MIRA_DEFINE_CONTEXT_ID(MutationId);

#undef MIRA_DEFINE_CONTEXT_ID

// Current context contract schema; readers must accept the current and the
// previous compatible major and must ignore unknown members (safe degrade).
[[nodiscard]] constexpr SchemaVersion context_contract_version() noexcept { return {1, 0}; }

// Per-profile budget configuration (design Context/Memory §7.1). Watermarks
// are ratios of the usable input budget, not of the raw context window.
struct ContextLimits final {
    std::uint64_t context_window_tokens = 128'000;
    std::uint64_t reserved_output_tokens = 16'384;
    std::uint64_t safety_margin_tokens = 1'024;
    // Provider framing overhead that is not visible in item content.
    std::uint64_t provider_overhead_tokens = 0;
    std::uint64_t max_image_tokens = 20'480;
    std::uint64_t max_tool_schema_tokens = 8'192;
    double trim_watermark = 0.70;
    double checkpoint_watermark = 0.85;
    double hard_watermark = 0.95;

    // window - reserved - safety - provider overhead, saturating at zero.
    [[nodiscard]] std::uint64_t input_budget_tokens() const noexcept;
    // Absolute token threshold for one watermark ratio, saturating at zero.
    [[nodiscard]] std::uint64_t watermark_tokens(double watermark) const noexcept;
    [[nodiscard]] Result<void> validate() const;
};

enum class TokenCountQuality : std::uint8_t {
    ExactProviderCount,
    ExactLocalTokenizer,
    ConservativeEstimate,
    DegradedEstimate, // A better counter failed; a conservative bound is used.
};

[[nodiscard]] std::string token_count_quality_name(TokenCountQuality quality);

// All watermark decisions use upper_bound. A failed exact count degrades to a
// conservative estimate; it must never be reported as zero tokens.
struct TokenEstimate final {
    std::uint64_t lower_bound = 0;
    std::uint64_t upper_bound = 0;
    TokenCountQuality quality = TokenCountQuality::ConservativeEstimate;
    ModelProfileId profile_id;
    [[nodiscard]] bool empty() const noexcept { return upper_bound == 0; }
};

enum class ContextAuthority : std::uint8_t {
    SystemPolicy,
    UserConstraint,
    VerifiedState,
    RetrievedMemory,
    UntrustedExternalData,
};

[[nodiscard]] std::string context_authority_name(ContextAuthority authority);

enum class ContextPriority : std::uint8_t {
    Essential = 0,
    High,
    Normal,
    Low,
    Disposable,
};

enum class ContextItemKind : std::uint8_t {
    SystemPolicy,        // P0
    Goal,                // P1
    UserConstraint,      // P1
    TaskLimits,          // P1
    CurrentObservation,  // P2
    UncertainSideEffect, // P2, pinned
    VerificationResult,  // P2
    CheckpointSummary,   // P3
    RecentAction,        // P3
    RecentError,         // P3
    RetrievedMemory,     // P4
    ToolCall,            // P5/paired
    ToolResult,          // P5/paired, unconsumed results are minimum set
    HistoricalPayload,   // P5
};

[[nodiscard]] std::string context_item_kind_name(ContextItemKind kind);
[[nodiscard]] std::optional<ContextItemKind> context_item_kind_from(std::string_view name);

enum class ContextPartition : std::uint8_t {
    P0Policy = 0,
    P1TaskFrame = 1,
    P2State = 2,
    P3Progress = 3,
    P4Memory = 4,
    P5History = 5,
};

[[nodiscard]] ContextPartition partition_of(ContextItemKind kind) noexcept;

// One candidate unit of model context. External text (web, OCR, tool output)
// enters as UntrustedExternalData and can never carry SystemPolicy authority.
struct ContextItem final {
    ContextItemId id;
    ContextItemKind kind = ContextItemKind::CurrentObservation;
    ContextAuthority authority = ContextAuthority::VerifiedState;
    ContextPriority priority = ContextPriority::Normal;
    std::vector<ModelContentPart> content;
    // Large payload referenced by this item; replaced-by-reference keeps the
    // ArtifactRef and drops inline content.
    std::optional<ArtifactRef> payload;
    std::vector<EventId> provenance;
    bool pinned = false;
    bool replaceable_by_reference = false;
    // Tool results not yet followed by a model decision must be retained.
    bool consumed = true;
    // Pairs ToolCall and ToolResult items; required for both kinds.
    std::optional<std::string> tool_call_key;
    // Ordering key inside one partition (event sequence or supplied order).
    std::uint64_t sequence = 0;
    // Epochs observed when the item was produced; a mismatch with the request
    // boundary makes the whole build stale instead of silently mixing epochs.
    std::optional<std::uint64_t> task_epoch;
    std::optional<std::uint64_t> environment_epoch;

    [[nodiscard]] bool is_tool_result_pending() const noexcept {
        return kind == ContextItemKind::ToolResult && !consumed;
    }
    [[nodiscard]] Result<void> validate() const;
};

// The consistency boundary fixed at the start of one context build.
struct ContextRequest final {
    SchemaVersion contract_version = context_contract_version();
    TaskId task_id;
    SessionId session_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    ModelProfileId profile_id;
    ContextLimits limits;
    std::vector<ContextItem> items;
    std::vector<ExposedToolSpec> tools;
    // Explicitly authorized fallback for the minimum-set overflow path; the
    // manager never routes to an unlisted profile on its own.
    std::optional<ModelProfileId> authorized_large_window_profile;
    ContextLimits authorized_large_window_limits;

    [[nodiscard]] Result<void> validate() const;
};

enum class ContextItemDisposition : std::uint8_t {
    Selected,
    SelectedByReference,
    Compressed,
    Dropped,
    RejectedMinimumSet,
};

[[nodiscard]] std::string context_item_disposition_name(ContextItemDisposition disposition);

// Per-item accounting: every input item appears exactly once with a stable,
// safe reason code. Codes are contract surface, free text is not allowed.
struct ContextItemAudit final {
    ContextItemId id;
    ContextItemKind kind = ContextItemKind::HistoricalPayload;
    ContextItemDisposition disposition = ContextItemDisposition::Selected;
    std::string reason;
    std::uint64_t estimated_tokens = 0;
};

struct ContextToolAudit final {
    ToolId tool_id;
    bool selected = false;
    std::string reason;
    std::uint64_t estimated_tokens = 0;
};

enum class ContextWatermark : std::uint8_t { Normal, Trim, Checkpoint, Hard };

[[nodiscard]] std::string context_watermark_name(ContextWatermark watermark);

struct ContextBudgetReport final {
    std::uint64_t input_budget_tokens = 0;
    std::uint64_t estimated_tokens = 0; // upper bound of the final selection
    TokenCountQuality quality = TokenCountQuality::ConservativeEstimate;
    double utilization = 0.0;
    ContextWatermark watermark = ContextWatermark::Normal;
    bool routed_to_large_window = false;
    std::optional<ModelProfileId> routed_profile;
    bool checkpoint_recommended = false;
    std::size_t selected_items = 0;
    std::size_t replaced_items = 0;
    std::size_t compressed_items = 0;
    std::size_t dropped_items = 0;
};

// The frozen output of one context build. The echoed epochs let the caller
// detect staleness between build and request submission.
struct PreparedModelContext final {
    SchemaVersion contract_version = context_contract_version();
    TaskId task_id;
    SessionId session_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;
    ModelProfileId profile_id;
    std::vector<ModelInputItem> input;
    std::vector<ExposedToolSpec> tools;
    ContextBudgetReport budget;
    std::vector<ContextItemAudit> item_audit;
    std::vector<ContextToolAudit> tool_audit;
    TokenEstimate total_estimate;
    Hash selection_digest{};
};

// ---------------------------------------------------------------------------
// Stable error domain (schema "mira.context.error.v1")
// ---------------------------------------------------------------------------

enum class ContextDomainCode : std::int32_t {
    MinimumSetTooLarge = 1,
    StaleBuild = 2,
    InvalidLimits = 3,
    InvalidItem = 4,
    TokenCountUnavailable = 5,
    ToolPairingBroken = 6,
    SchemaUnsupported = 7,
};

[[nodiscard]] std::string context_domain_code_name(ContextDomainCode code);
[[nodiscard]] Error make_context_error(ContextDomainCode code, std::string safe_message,
                                       bool retryable = false,
                                       std::optional<OperationId> operation = std::nullopt);

// ---------------------------------------------------------------------------
// Versioned JSON serialization (schema "mira.context.item.v1",
// "mira.context.limits.v1", "mira.context.prepared.v1"). Readers ignore
// unknown members and reject unsupported schema majors.
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue context_limits_to_json(const ContextLimits &limits);
[[nodiscard]] Result<ContextLimits> context_limits_from_json(const JsonValue &json);
[[nodiscard]] JsonValue context_item_to_json(const ContextItem &item);
[[nodiscard]] Result<ContextItem> context_item_from_json(const JsonValue &json);
[[nodiscard]] JsonValue prepared_context_to_json(const PreparedModelContext &prepared);
[[nodiscard]] Result<PreparedModelContext> prepared_context_from_json(const JsonValue &json);

} // namespace mira
