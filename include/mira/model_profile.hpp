#pragma once

#include <mira/model_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// Fixed wire dialects; a new dialect always requires a new RouteDecision and
// never an in-flight fallback between endpoints.
enum class ProtocolDialect : std::uint8_t {
    OpenAIResponsesV1,
    OpenAIChatCompletionsV1,
};

[[nodiscard]] std::string protocol_dialect_name(ProtocolDialect dialect);
[[nodiscard]] std::optional<ProtocolDialect> protocol_dialect_from(std::string_view name);

// Evidence levels for capability claims; production claims require at least
// FixtureVerified, interop statements require InteropVerified.
enum class CapabilityEvidence : std::uint8_t {
    Configured = 0,
    Documented = 1,
    FixtureVerified = 2,
    InteropVerified = 3,
};

[[nodiscard]] std::string capability_evidence_name(CapabilityEvidence evidence);

// One declared capability with the evidence backing it. A profile that cannot
// honor a capability must not declare it.
struct CapabilityFlag final {
    bool supported = false;
    CapabilityEvidence evidence = CapabilityEvidence::Configured;
    std::string note;
};

// How one canonical generation parameter maps onto a dialect.
enum class ParamMapping : std::uint8_t {
    Native,      // Verified native field on the wire dialect.
    Mapped,      // Documented, fixture-verified equivalent field.
    OmitIfUnset, // Sent only when the canonical request sets it.
    Unsupported, // Setting the parameter fails with CapabilityMismatch.
};

[[nodiscard]] std::string param_mapping_name(ParamMapping mapping);

// Hard profile limits enforced before any bytes are sent.
struct ProfileLimits final {
    std::uint64_t max_context_tokens = 128'000;
    std::uint64_t max_output_tokens = 16'384;
    std::uint64_t max_request_bytes = 8ULL * 1024ULL * 1024ULL;
    std::uint64_t max_response_bytes = 8ULL * 1024ULL * 1024ULL;
    std::size_t max_input_items = 256;
    std::size_t max_tools_per_request = 64;
    std::size_t max_images_per_request = 16;
    std::uint64_t max_image_bytes = 16ULL * 1024ULL * 1024ULL;
    std::size_t max_output_items = 256;
};

struct GenerationParamPolicy final {
    ParamMapping max_output_tokens = ParamMapping::Native;
    ParamMapping temperature = ParamMapping::Native;
    ParamMapping top_p = ParamMapping::Native;
    ParamMapping seed = ParamMapping::Unsupported;
    ParamMapping reasoning_effort = ParamMapping::Unsupported;
    ParamMapping service_tier = ParamMapping::Unsupported;
};

struct ModelProfileCapabilities final {
    CapabilityFlag text;
    CapabilityFlag image_input;
    CapabilityFlag file_input;
    CapabilityFlag strict_json_schema;
    CapabilityFlag function_tools;
    CapabilityFlag parallel_tool_calls;
    CapabilityFlag sse;
    CapabilityFlag exact_token_count;
    CapabilityFlag continuation;
    CapabilityFlag remote_retention;
    CapabilityFlag upload;
    GenerationParamPolicy generation;
    ProfileLimits limits;
};

// Reference to a secret resolved only at the transport boundary.
struct SecretRef final {
    std::string name;
    friend bool operator==(const SecretRef &, const SecretRef &) noexcept = default;
};

// Staged network deadlines (design LLM API §10.1). All optional; the final
// deadline is the minimum of task, operation, budget and profile deadlines.
struct TransportDeadlines final {
    std::chrono::milliseconds dns{5'000};
    std::chrono::milliseconds connect{5'000};
    std::chrono::milliseconds tls{10'000};
    std::chrono::milliseconds write{10'000};
    std::chrono::milliseconds first_byte{30'000};
    std::chrono::milliseconds idle_read{30'000};
    std::chrono::milliseconds total{120'000};
};

// A fixed provider endpoint profile. The endpoint origin and API prefix are
// configuration; mappers never accept model- or response-supplied URLs.
struct ModelProfile final {
    ModelProfileId id;
    std::string display_name;
    SemanticVersion version;
    ProtocolDialect dialect = ProtocolDialect::OpenAIResponsesV1;
    std::string endpoint_origin; // e.g. "https://api.example.com"
    std::string api_prefix;      // e.g. "/v1"
    std::string model_selector;  // Model alias sent on the wire.
    std::optional<std::string> model_revision;
    SecretRef credential;
    ModelProfileCapabilities capabilities;
    ModelDataPolicy default_data_policy;
    TransportDeadlines deadlines;
    std::uint32_t max_redirects = 2;

    // Dialect-specific request path; never derived from model output.
    [[nodiscard]] std::string request_path() const;
    [[nodiscard]] std::string endpoint_url() const;
    // Versioned manifest digest covering identity, dialect, endpoint and
    // capabilities; used to bind RouteDecisions and continuations.
    [[nodiscard]] Hash profile_digest() const;
    [[nodiscard]] JsonValue manifest_to_json() const;
    [[nodiscard]] Result<void> validate() const;
};

// What a routed request needs from a profile.
struct RouteQuery final {
    bool needs_text = true;
    bool needs_image_input = false;
    bool needs_file_input = false;
    bool needs_strict_schema = false;
    bool needs_tools = false;
    bool needs_sse = false;
    Sensitivity max_sensitivity = Sensitivity::Internal;
    CapabilityEvidence min_evidence = CapabilityEvidence::Configured;
    std::uint64_t required_context_tokens = 0;
    const ModelBudget *budget = nullptr;
    const ModelDataPolicy *data_policy = nullptr;
};

struct RouteRejection final {
    ModelProfileId profile_id;
    std::string reason;
};

struct RouteDecision final {
    ModelProfileId selected_profile;
    ProtocolDialect dialect = ProtocolDialect::OpenAIResponsesV1;
    Hash profile_digest{};
    std::vector<RouteRejection> rejections;
    CapabilityEvidence evidence = CapabilityEvidence::Configured;
    [[nodiscard]] bool selected() const noexcept { return !selected_profile.value.is_nil(); }
};

// Selects a fixed profile snapshot for a request. Routing happens before any
// bytes are sent; capability, data-policy and budget mismatches are rejected
// here rather than discovered on the wire.
class ModelRouter final {
  public:
    void register_profile(std::shared_ptr<const ModelProfile> profile);
    [[nodiscard]] Result<RouteDecision> route(const RouteQuery &query) const;
    [[nodiscard]] std::shared_ptr<const ModelProfile>
    find(const ModelProfileId &profile_id) const;
    [[nodiscard]] std::vector<std::shared_ptr<const ModelProfile>> profiles() const;

  private:
    std::vector<std::shared_ptr<const ModelProfile>> profiles_;
};

// Checks one profile against one query; shared by routing and by tests.
[[nodiscard]] std::vector<std::string> profile_mismatches(const ModelProfile &profile,
                                                          const RouteQuery &query);

// Rejects generation parameters the profile cannot represent. Empty result
// means every set parameter has a usable mapping.
[[nodiscard]] std::vector<std::string>
unsupported_generation_parameters(const GenerationParamPolicy &policy,
                                  const ModelGenerationOptions &generation);

} // namespace mira
