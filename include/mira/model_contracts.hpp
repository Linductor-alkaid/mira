#pragma once

#include <mira/artifact_store.hpp>
#include <mira/core_contracts.hpp>
#include <mira/environment.hpp>
#include <mira/json.hpp>
#include <mira/observation.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// Stable content hash used across model contracts, profiles and digests.
using Hash = Sha256Digest;

struct SemanticVersion final {
    std::uint16_t major = 1;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
    friend constexpr bool operator==(const SemanticVersion &,
                                     const SemanticVersion &) noexcept = default;
    friend constexpr auto operator<=>(const SemanticVersion &,
                                      const SemanticVersion &) noexcept = default;
};

#define MIRA_DEFINE_MODEL_ID(name)                                                                    \
    struct name final {                                                                               \
        Id128 value{};                                                                                \
        static name generate() { return name{Id128::generate()}; }                                    \
        static std::optional<name> parse(std::string_view text) noexcept {                            \
            const auto parsed = Id128::parse(text);                                                   \
            return parsed ? std::optional<name>(name{*parsed}) : std::nullopt;                        \
        }                                                                                             \
        [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }                         \
        [[nodiscard]] std::string to_string() const { return value.to_string(); }                     \
        friend constexpr bool operator==(const name &, const name &) noexcept = default;              \
        friend constexpr auto operator<=>(const name &, const name &) noexcept = default;             \
    }

MIRA_DEFINE_MODEL_ID(ModelRequestId);
MIRA_DEFINE_MODEL_ID(ModelProfileId);
MIRA_DEFINE_MODEL_ID(SchemaId);
MIRA_DEFINE_MODEL_ID(ToolId);
MIRA_DEFINE_MODEL_ID(EvalCaseId);
MIRA_DEFINE_MODEL_ID(EvalRunId);

#undef MIRA_DEFINE_MODEL_ID

// Provider-scoped identifier of one tool invocation on the wire.
struct ProviderToolCallId final {
    std::string value;
    friend bool operator==(const ProviderToolCallId &, const ProviderToolCallId &) = default;
};

// Stable, payload-free reference to stored content. Payload bytes never
// travel inside requests, events or digests; only their digest does.
struct ArtifactRef final {
    ArtifactId id;
    Sha256Digest digest{};
    std::uint64_t byte_size = 0;
    std::string media_type;
    Sensitivity sensitivity = Sensitivity::Internal;
};

// ---------------------------------------------------------------------------
// Roles, content and output modes
// ---------------------------------------------------------------------------

enum class ModelRole : std::uint8_t {
    System,
    Developer,
    User,
    Assistant,
    Unknown,
};

enum class ImageDetail : std::uint8_t { Auto, Low, High, Original };

struct TextPart final {
    std::string text;
    Sensitivity sensitivity = Sensitivity::Internal;
};

struct ImagePart final {
    ArtifactRef source;
    ImageDetail detail = ImageDetail::Auto;
    std::string media_type;
};

struct FilePart final {
    ArtifactRef source;
    std::string media_type;
    std::string display_name;
};

using ModelContentPart = std::variant<TextPart, ImagePart, FilePart>;

struct ModelInputItem final {
    ModelRole role = ModelRole::User;
    std::vector<ModelContentPart> content;
    Provenance provenance;
    // Authority carried by this item; only PromptAssembler output may carry
    // System/Developer authority. Untrusted observation text stays labeled.
    Sensitivity authority = Sensitivity::Internal;
};

enum class OutputMode : std::uint8_t {
    StrictJsonSchema,
    StrictFunctionTool,
    JsonObject,
    Text,
};

enum class ReasoningEffort : std::uint8_t { Minimal, Low, Medium, High };
enum class ServiceTier : std::uint8_t { Auto, Default, Flex, Priority };

struct JsonSchema final {
    JsonValue root;
    [[nodiscard]] bool valid() const noexcept { return root.is_object(); }
};

struct ModelOutputContract final {
    OutputMode mode = OutputMode::Text;
    SchemaId schema_id;
    SemanticVersion schema_version;
    JsonSchema schema;
    Hash canonical_schema_digest{};
};

struct ModelGenerationOptions final {
    std::optional<std::uint64_t> max_output_tokens;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<std::uint64_t> seed;
    std::optional<ReasoningEffort> reasoning_effort;
    std::optional<ServiceTier> service_tier;
};

// ---------------------------------------------------------------------------
// Tools exposed to a single request
// ---------------------------------------------------------------------------

struct ExposedToolSpec final {
    ToolId tool_id;
    SemanticVersion version;
    std::string wire_name;       // Provider-visible alias, unique per request.
    std::string description;
    JsonSchema parameters_schema;
    Hash spec_digest{};
    bool has_side_effects = false;
};

enum class ToolChoiceMode : std::uint8_t { Auto, None, Required, Named };

struct ToolChoice final {
    ToolChoiceMode mode = ToolChoiceMode::Auto;
    ToolId required_tool; // Only meaningful for Named.
};

// ---------------------------------------------------------------------------
// Budget, data policy and provenance
// ---------------------------------------------------------------------------

struct ModelBudget final {
    // Upper bounds reserved before dispatch; zero means "no separate limit".
    std::uint64_t max_input_tokens = 0;
    std::uint64_t max_output_tokens = 0;
    std::uint64_t max_total_cost_micros = 0; // Currency minor units x 1e6.
    std::string currency = "USD";
    std::uint32_t max_requests = 0;
    std::uint64_t max_image_bytes = 0;
};

struct ModelDataPolicy final {
    // Remote storage of request/response payloads. `store` must be an explicit
    // decision; adapters never rely on provider defaults.
    std::optional<bool> store;
    bool allow_uploads = false;
    std::optional<std::string> region;
    std::optional<std::string> organization;
    std::optional<std::string> project;
    std::chrono::seconds local_raw_retention{0};
    std::chrono::seconds remote_retention{0};
};

struct PromptProvenance final {
    SemanticVersion system_template_version;
    Hash system_template_digest{};
    Hash decision_schema_digest{};
    Hash tool_snapshot_digest{};
    Hash context_selection_digest{};
    SemanticVersion redaction_policy_version;
};

// Opaque provider-side continuation state; never a Mira source of truth.
// M4-14 widens the binding: provider identity, conversation, session and
// environment epoch join profile/task/schema/tool/policy so that provider
// switches, takeover, cancellation and recovery all invalidate reuse and the
// next build must come from the local checkpoint. Legacy payloads without
// the new fields decode with empty/zero values and readers treat them as
// "not asserted" rather than trusting them.
struct ProviderContinuation final {
    std::string provider_state;
    std::optional<std::string> previous_response_id;
    // Provider backend identity (e.g. "openai-compatible"); empty on legacy.
    std::string provider;
    // Provider conversation identifier; empty when the backend has none.
    std::string conversation;
    ModelProfileId profile_id;
    // Manifest digest binding the continuation to one profile revision.
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
    Timestamp created_at;
    std::chrono::steady_clock::time_point expires_at{};
    bool deleted_remotely = false;
};

struct ModelRequest final {
    SchemaVersion contract_version;
    ModelRequestId request_id;
    OperationId operation_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    ModelProfileId profile_id;
    std::vector<ModelInputItem> input;
    ModelOutputContract output_contract;
    std::vector<ExposedToolSpec> tools;
    ToolChoice tool_choice;
    ModelGenerationOptions generation;
    std::optional<ProviderContinuation> continuation;
    ModelBudget budget;
    ModelDataPolicy data_policy;
    PromptProvenance prompt_provenance;
};

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

enum class ModelCompletionStatus : std::uint8_t {
    Completed,
    Incomplete,
    Refused,
    ContentFiltered,
    Failed,
    Cancelled,
    Unknown,
};

enum class IncompleteReason : std::uint8_t {
    MaxOutputTokens,
    ContextWindow,
    Other,
};

struct OutputTextPart final {
    std::string text;
    std::optional<std::string> annotations_digest;
};

struct OutputRefusalPart final {
    std::string safe_summary;
};

using ModelOutputContentPart = std::variant<OutputTextPart, OutputRefusalPart>;

struct MessageOutput final {
    ModelRole role = ModelRole::Assistant;
    std::vector<ModelOutputContentPart> content;
};

struct ToolCallOutput final {
    ProviderToolCallId provider_call_id;
    ToolId tool_id;
    std::string provider_name;
    JsonValue arguments;
    Hash arguments_digest{};
};

struct RefusalOutput final {
    std::string safe_summary;
    std::optional<std::string> provider_code;
};

struct UnknownOutput final {
    std::string provider_type;
    Hash payload_digest{};
    std::optional<ArtifactRef> protected_payload;
};

using ModelOutputItem = std::variant<MessageOutput, ToolCallOutput, RefusalOutput, UnknownOutput>;

enum class UsageQuality : std::uint8_t { Exact, ProviderReported, Estimated, Partial, Missing };

struct ModelUsage final {
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
    std::optional<std::uint64_t> cached_input_tokens;
    std::optional<std::uint64_t> reasoning_tokens;
    UsageQuality quality = UsageQuality::Missing;
};

struct RateLimitMetadata final {
    std::optional<std::uint64_t> remaining_requests;
    std::optional<std::uint64_t> remaining_tokens;
    std::optional<std::chrono::seconds> reset_after;
};

struct ModelResponse final {
    SchemaVersion contract_version;
    ModelRequestId request_id;
    OperationId operation_id;
    ModelProfileId profile_id;
    std::string requested_model;
    std::optional<std::string> resolved_model;
    std::optional<std::string> provider_response_id;
    std::optional<std::string> provider_request_id;
    ModelCompletionStatus status = ModelCompletionStatus::Unknown;
    std::optional<IncompleteReason> incomplete_reason;
    std::vector<ModelOutputItem> output;
    ModelUsage usage;
    RateLimitMetadata rate_limit;
    std::optional<ProviderContinuation> continuation;
    std::optional<ArtifactRef> protected_raw_response;
};

// ---------------------------------------------------------------------------
// Model error domain (stable domain_code symbols, see LLM API design §11)
// ---------------------------------------------------------------------------

enum class ModelDomainCode : std::int32_t {
    EndpointPolicyDenied = 1,
    AuthenticationFailed = 2,
    ProviderPermissionDenied = 3,
    InvalidModelRequest = 4,
    CapabilityMismatch = 5,
    ContextLimitExceeded = 6,
    RateLimited = 7,
    ProviderOverloaded = 8,
    TransportFailed = 9,
    ProtocolViolation = 10,
    ResponseTooLarge = 11,
    ModelRefused = 12,
    ContentFiltered = 13,
    IncompleteModelOutput = 14,
    MalformedStructuredOutput = 15,
    AmbiguousModelOutput = 16,
    AmbiguousCompletion = 17,
    ModelCancelled = 18,
    ModelDeadlineExceeded = 19,
    ModelResourceExhausted = 20,
};

[[nodiscard]] std::string model_domain_code_name(ModelDomainCode code);
[[nodiscard]] Error make_model_error(ModelDomainCode code, std::string safe_message,
                                     bool retryable = false,
                                     std::optional<OperationId> operation = std::nullopt);

// ---------------------------------------------------------------------------
// Validation and canonical JSON serialization (schema "mira.model.request.v1"
// and "mira.model.response.v1")
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void> validate_model_request(const ModelRequest &request);
[[nodiscard]] Result<void> validate_model_response(const ModelResponse &response);

[[nodiscard]] JsonValue model_request_to_json(const ModelRequest &request);
[[nodiscard]] Result<ModelRequest> model_request_from_json(const JsonValue &json);
[[nodiscard]] JsonValue model_response_to_json(const ModelResponse &response);
[[nodiscard]] Result<ModelResponse> model_response_from_json(const JsonValue &json);

// Digest of the canonical request with secrets already excluded: artifact
// references contribute their digest, never inline payload bytes.
[[nodiscard]] Hash model_request_canonical_digest(const ModelRequest &request);

} // namespace mira
