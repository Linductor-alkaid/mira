#include <mira/model_profile.hpp>

#include "model_profile_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error route_error(std::string message) {
    Error error;
    error.code = ErrorCode::UnsupportedCapability;
    error.domain = "mira.model";
    error.domain_code = static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch);
    error.safe_message = std::move(message);
    return error;
}

// Reserve optional standard controls even when this mapper does not emit them.
// Adding a mapper-owned root field requires updating this list and its tests.
constexpr std::array kChatReservedKeys = {
    "model",
    "messages",
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "response_format",
    "max_tokens",
    "max_completion_tokens",
    "temperature",
    "top_p",
    "seed",
    "reasoning_effort",
    "service_tier",
    "stream",
    "stream_options",
    "store",
    "metadata",
    "user",
    "n",
    "stop",
    "presence_penalty",
    "frequency_penalty",
    "logit_bias",
    "logprobs",
    "top_logprobs",
    "modalities",
    "audio",
    "prediction",
    "web_search_options",
    "functions",
    "function_call",
    "safety_identifier",
    "prompt_cache_key",
    "prompt_cache_retention",
    "verbosity",
    // Canonical fields with no Chat representation must not bypass their gates.
    "max_output_tokens",
    "thinking",
    "input",
    "instructions",
    "previous_response_id",
    "conversation",
    "extra_body",
};

// JsonValue can be constructed in C++ without the strict parser. Bound recursive
// serialization first and reject non-finite numbers before its integer conversion.
[[nodiscard]] bool serializable_extra_value(const JsonValue &value, std::size_t depth) {
    if (depth >= JsonLimits{}.max_depth) {
        return false;
    }
    if (value.kind() == JsonValue::Kind::Number && !std::isfinite(*value.as_number())) {
        return false;
    }
    if (const auto *object = value.as_object()) {
        for (const auto &[key, child] : *object) {
            (void)key;
            if (!serializable_extra_value(child, depth + 1)) {
                return false;
            }
        }
    } else if (const auto *array = value.as_array()) {
        for (const auto &child : *array) {
            if (!serializable_extra_value(child, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool evidence_at_least(CapabilityEvidence provided, CapabilityEvidence required) {
    return static_cast<std::uint8_t>(provided) >= static_cast<std::uint8_t>(required);
}

[[nodiscard]] JsonValue flag_to_json(const CapabilityFlag &flag) {
    JsonValue::Object object;
    object.emplace_back("supported", flag.supported);
    object.emplace_back("evidence", capability_evidence_name(flag.evidence));
    object.emplace_back("note", flag.note);
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue limits_to_json(const ProfileLimits &limits) {
    JsonValue::Object object;
    object.emplace_back("max_context_tokens", static_cast<std::int64_t>(limits.max_context_tokens));
    object.emplace_back("max_output_tokens", static_cast<std::int64_t>(limits.max_output_tokens));
    object.emplace_back("max_request_bytes", static_cast<std::int64_t>(limits.max_request_bytes));
    object.emplace_back("max_response_bytes", static_cast<std::int64_t>(limits.max_response_bytes));
    object.emplace_back("max_input_items", static_cast<std::int64_t>(limits.max_input_items));
    object.emplace_back("max_tools_per_request",
                        static_cast<std::int64_t>(limits.max_tools_per_request));
    object.emplace_back("max_images_per_request",
                        static_cast<std::int64_t>(limits.max_images_per_request));
    object.emplace_back("max_image_bytes", static_cast<std::int64_t>(limits.max_image_bytes));
    object.emplace_back("max_output_items", static_cast<std::int64_t>(limits.max_output_items));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue version_to_json(const SemanticVersion &version) {
    JsonValue::Object object;
    object.emplace_back("major", static_cast<std::int64_t>(version.major));
    object.emplace_back("minor", static_cast<std::int64_t>(version.minor));
    object.emplace_back("patch", static_cast<std::int64_t>(version.patch));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue data_policy_to_json(const ModelDataPolicy &policy) {
    JsonValue::Object object;
    if (policy.store.has_value()) {
        object.emplace_back("store", *policy.store);
    }
    object.emplace_back("allow_uploads", policy.allow_uploads);
    if (policy.region.has_value()) {
        object.emplace_back("region", *policy.region);
    }
    if (policy.organization.has_value()) {
        object.emplace_back("organization", *policy.organization);
    }
    if (policy.project.has_value()) {
        object.emplace_back("project", *policy.project);
    }
    object.emplace_back("remote_retention",
                        static_cast<std::int64_t>(policy.remote_retention.count()));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue deadlines_to_json(const TransportDeadlines &deadlines) {
    JsonValue::Object object;
    object.emplace_back("dns_ms", static_cast<std::int64_t>(deadlines.dns.count()));
    object.emplace_back("connect_ms", static_cast<std::int64_t>(deadlines.connect.count()));
    object.emplace_back("tls_ms", static_cast<std::int64_t>(deadlines.tls.count()));
    object.emplace_back("write_ms", static_cast<std::int64_t>(deadlines.write.count()));
    object.emplace_back("first_byte_ms", static_cast<std::int64_t>(deadlines.first_byte.count()));
    object.emplace_back("idle_read_ms", static_cast<std::int64_t>(deadlines.idle_read.count()));
    object.emplace_back("total_ms", static_cast<std::int64_t>(deadlines.total.count()));
    return JsonValue(std::move(object));
}

} // namespace

Result<void> validate_profile_extra_body(const ModelProfile &profile) {
    if (profile.extra_body.empty()) {
        return Result<void>{};
    }
    if (profile.dialect != ProtocolDialect::OpenAIChatCompletionsV1) {
        return route_error("extra_body requires Chat Completions dialect");
    }
    for (const auto &[key, value] : profile.extra_body) {
        if (key.empty() || std::find(kChatReservedKeys.begin(), kChatReservedKeys.end(), key) !=
                               kChatReservedKeys.end()) {
            return route_error("extra_body contains an empty or reserved root key");
        }
        if (value.is_array()) {
            return route_error("extra_body root values must be JSON scalars or objects");
        }
    }
    const JsonValue object(profile.extra_body);
    if (!serializable_extra_value(object, 0)) {
        return route_error("extra_body contains a non-finite number or excessive nesting");
    }
    const auto encoded = to_json_string(object);
    if (encoded.size() > profile.capabilities.limits.max_request_bytes) {
        return route_error("extra_body exceeds the profile request byte limit");
    }
    // Reuse the strict parser for duplicate keys (also nested), UTF-8 and the
    // default JSON resource limits. Do not surface its host-controlled details.
    if (!parse_json(encoded)) {
        return route_error("extra_body must contain valid JSON within JsonLimits");
    }
    return Result<void>{};
}

std::string protocol_dialect_name(ProtocolDialect dialect) {
    switch (dialect) {
    case ProtocolDialect::OpenAIResponsesV1:
        return "openai.responses.v1";
    case ProtocolDialect::OpenAIChatCompletionsV1:
        return "openai.chat-completions.v1";
    case ProtocolDialect::AnthropicMessagesV1:
        return "anthropic.messages.v1";
    }
    return "unknown";
}

std::optional<ProtocolDialect> protocol_dialect_from(std::string_view name) {
    if (name == "openai.responses.v1") {
        return ProtocolDialect::OpenAIResponsesV1;
    }
    if (name == "openai.chat-completions.v1") {
        return ProtocolDialect::OpenAIChatCompletionsV1;
    }
    if (name == "anthropic.messages.v1")
        return ProtocolDialect::AnthropicMessagesV1;
    return std::nullopt;
}

std::string capability_evidence_name(CapabilityEvidence evidence) {
    switch (evidence) {
    case CapabilityEvidence::Configured:
        return "Configured";
    case CapabilityEvidence::Documented:
        return "Documented";
    case CapabilityEvidence::FixtureVerified:
        return "FixtureVerified";
    case CapabilityEvidence::InteropVerified:
        return "InteropVerified";
    }
    return "Configured";
}

std::string param_mapping_name(ParamMapping mapping) {
    switch (mapping) {
    case ParamMapping::Native:
        return "Native";
    case ParamMapping::Mapped:
        return "Mapped";
    case ParamMapping::OmitIfUnset:
        return "OmitIfUnset";
    case ParamMapping::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

std::string ModelProfile::request_path() const {
    std::string prefix = api_prefix;
    switch (dialect) {
    case ProtocolDialect::OpenAIResponsesV1:
        prefix += "/responses";
        return prefix;
    case ProtocolDialect::OpenAIChatCompletionsV1:
        prefix += "/chat/completions";
        return prefix;
    case ProtocolDialect::AnthropicMessagesV1:
        return prefix + "/messages";
    }
    return prefix;
}

std::string ModelProfile::endpoint_url() const { return endpoint_origin + request_path(); }

std::string ModelProfile::endpoint_host() const {
    const auto scheme = endpoint_origin.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const auto begin = scheme + 3;
    if (begin < endpoint_origin.size() && endpoint_origin[begin] == '[') {
        const auto bracket = endpoint_origin.find(']', begin + 1);
        return bracket == std::string::npos
                   ? std::string{}
                   : endpoint_origin.substr(begin + 1, bracket - begin - 1);
    }
    const auto end = endpoint_origin.find_first_of(":/", begin);
    return endpoint_origin.substr(begin,
                                  end == std::string::npos ? std::string::npos : end - begin);
}

JsonValue ModelProfile::manifest_to_json() const {
    JsonValue::Object root;
    root.emplace_back("id", id.to_string());
    root.emplace_back("display_name", display_name);
    root.emplace_back("version", version_to_json(version));
    root.emplace_back("dialect", protocol_dialect_name(dialect));
    root.emplace_back("endpoint_origin", endpoint_origin);
    root.emplace_back("allow_private_endpoint", allow_private_endpoint);
    root.emplace_back("api_prefix", api_prefix);
    root.emplace_back("model_selector", model_selector);
    if (model_revision.has_value()) {
        root.emplace_back("model_revision", *model_revision);
    }
    root.emplace_back("credential_name", credential.name);

    JsonValue::Object capabilities_json;
    capabilities_json.emplace_back("text", flag_to_json(capabilities.text));
    capabilities_json.emplace_back("image_input", flag_to_json(capabilities.image_input));
    capabilities_json.emplace_back("file_input", flag_to_json(capabilities.file_input));
    capabilities_json.emplace_back("strict_json_schema",
                                   flag_to_json(capabilities.strict_json_schema));
    capabilities_json.emplace_back("function_tools", flag_to_json(capabilities.function_tools));
    capabilities_json.emplace_back("parallel_tool_calls",
                                   flag_to_json(capabilities.parallel_tool_calls));
    capabilities_json.emplace_back("sse", flag_to_json(capabilities.sse));
    capabilities_json.emplace_back("exact_token_count",
                                   flag_to_json(capabilities.exact_token_count));
    capabilities_json.emplace_back("continuation", flag_to_json(capabilities.continuation));
    capabilities_json.emplace_back("remote_retention", flag_to_json(capabilities.remote_retention));
    capabilities_json.emplace_back("upload", flag_to_json(capabilities.upload));

    JsonValue::Object generation_json;
    generation_json.emplace_back("max_output_tokens",
                                 param_mapping_name(capabilities.generation.max_output_tokens));
    generation_json.emplace_back("temperature",
                                 param_mapping_name(capabilities.generation.temperature));
    generation_json.emplace_back("top_p", param_mapping_name(capabilities.generation.top_p));
    generation_json.emplace_back("seed", param_mapping_name(capabilities.generation.seed));
    generation_json.emplace_back("reasoning_effort",
                                 param_mapping_name(capabilities.generation.reasoning_effort));
    generation_json.emplace_back("thinking", param_mapping_name(capabilities.generation.thinking));
    generation_json.emplace_back("service_tier",
                                 param_mapping_name(capabilities.generation.service_tier));
    capabilities_json.emplace_back("generation", std::move(generation_json));
    capabilities_json.emplace_back("limits", limits_to_json(capabilities.limits));
    root.emplace_back("capabilities", std::move(capabilities_json));

    root.emplace_back("default_data_policy", data_policy_to_json(default_data_policy));
    root.emplace_back("deadlines", deadlines_to_json(deadlines));
    if (proxy.has_value()) {
        JsonValue::Object proxy_json;
        proxy_json.emplace_back("url", proxy->url);
        proxy_json.emplace_back("allow_private_endpoint", proxy->allow_private_endpoint);
        JsonValue::Array allowed_hosts;
        for (const auto &host : proxy->allowed_hosts) {
            allowed_hosts.emplace_back(host);
        }
        proxy_json.emplace_back("allowed_hosts", std::move(allowed_hosts));
        if (proxy->authorization.has_value()) {
            proxy_json.emplace_back("credential_name", proxy->authorization->name);
        }
        root.emplace_back("proxy", std::move(proxy_json));
    }
    if (!extra_body.empty()) {
        root.emplace_back("extra_body", extra_body);
    }
    root.emplace_back("max_redirects", static_cast<std::int64_t>(max_redirects));
    return JsonValue(std::move(root));
}

Hash ModelProfile::profile_digest() const { return canonical_json_digest(manifest_to_json()); }

Result<void> ModelProfile::validate() const {
    if (auto gate = validate_profile_extra_body(*this); !gate) {
        return gate.error();
    }
    if (id.value.is_nil() || display_name.empty() || model_selector.empty()) {
        return route_error("profile identity fields must be set");
    }
    if (!protocol_dialect_from(protocol_dialect_name(dialect)).has_value()) {
        return route_error("profile dialect is unknown");
    }
    if (endpoint_origin.rfind("https://", 0) != 0 && endpoint_origin.rfind("http://", 0) != 0) {
        return route_error("profile endpoint origin must be an absolute http(s) URL");
    }
    if (endpoint_origin.find('@') != std::string::npos ||
        endpoint_origin.find(' ') != std::string::npos) {
        return route_error("profile endpoint origin must not carry credentials or spaces");
    }
    if (endpoint_host().empty()) {
        return route_error("profile endpoint origin must carry a valid host");
    }
    if (!api_prefix.empty() && api_prefix.front() != '/') {
        return route_error("profile api prefix must start with '/'");
    }
    if (proxy.has_value()) {
        if (proxy->url.rfind("http://", 0) != 0 || proxy->url.find('@') != std::string::npos ||
            proxy->url.find(' ') != std::string::npos) {
            return route_error("profile proxy must be an absolute http origin without credentials");
        }
        const auto authority_end = proxy->url.find('/', 7);
        if (authority_end != std::string::npos && authority_end + 1 != proxy->url.size()) {
            return route_error("profile proxy must not carry a path");
        }
    }
    if (capabilities.limits.max_tools_per_request > kModelMaxToolsPerRequest) {
        return route_error("profile max_tools_per_request=" +
                           std::to_string(capabilities.limits.max_tools_per_request) +
                           " exceeds limit=" + std::to_string(kModelMaxToolsPerRequest));
    }
    const auto &limits = capabilities.limits;
    if (limits.max_context_tokens == 0 || limits.max_output_tokens == 0 ||
        limits.max_request_bytes == 0 || limits.max_response_bytes == 0) {
        return route_error("profile limits must be positive");
    }
    if (deadlines.total.count() <= 0 || deadlines.connect.count() <= 0) {
        return route_error("profile deadlines must be positive");
    }
    if (default_data_policy.store.has_value() && *default_data_policy.store &&
        default_data_policy.remote_retention.count() <= 0) {
        return route_error("profiles with remote storage require an explicit retention period");
    }
    return Result<void>{};
}

Result<void> validate_model_tool_limit(const ModelRequest &request, const ModelProfile &profile) {
    const auto limit = profile.capabilities.limits.max_tools_per_request;
    if (limit > kModelMaxToolsPerRequest) {
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "profile max_tools_per_request=" + std::to_string(limit) +
                                    " exceeds limit=" + std::to_string(kModelMaxToolsPerRequest));
    }
    if (request.tools.size() > limit) {
        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                "model request tool count=" + std::to_string(request.tools.size()) +
                                    " exceeds profile limit=" + std::to_string(limit) +
                                    "; configure a per-request tool selector");
    }
    return {};
}

std::vector<std::string> profile_mismatches(const ModelProfile &profile, const RouteQuery &query) {
    std::vector<std::string> mismatches;
    const auto &capabilities = profile.capabilities;
    const auto check = [&mismatches, &query](const char *name, const CapabilityFlag &flag) {
        if (!flag.supported) {
            mismatches.push_back(std::string("capability not supported: ") + name);
            return;
        }
        if (!evidence_at_least(flag.evidence, query.min_evidence)) {
            mismatches.push_back(std::string("capability evidence below requirement: ") + name);
        }
    };
    if (query.needs_text) {
        check("text", capabilities.text);
    }
    if (query.needs_image_input) {
        check("image_input", capabilities.image_input);
    }
    if (query.needs_file_input) {
        check("file_input", capabilities.file_input);
    }
    if (query.needs_strict_schema) {
        check("strict_json_schema", capabilities.strict_json_schema);
    }
    if (capabilities.limits.max_tools_per_request > kModelMaxToolsPerRequest) {
        mismatches.push_back("profile max_tools_per_request=" +
                             std::to_string(capabilities.limits.max_tools_per_request) +
                             " exceeds limit=" + std::to_string(kModelMaxToolsPerRequest));
    }
    if (query.required_tools > capabilities.limits.max_tools_per_request) {
        mismatches.push_back(
            "model request tool count=" + std::to_string(query.required_tools) +
            " exceeds profile limit=" + std::to_string(capabilities.limits.max_tools_per_request) +
            "; configure a per-request tool selector");
    }
    if (query.needs_tools || query.required_tools != 0) {
        check("function_tools", capabilities.function_tools);
    }
    if (query.needs_sse) {
        check("sse", capabilities.sse);
    }
    if (query.required_context_tokens > capabilities.limits.max_context_tokens) {
        mismatches.push_back("required context exceeds the profile limit");
    }
    if (query.max_sensitivity == Sensitivity::Secret) {
        mismatches.push_back("secret-classified content cannot be routed to a model");
    }
    if (query.max_sensitivity == Sensitivity::Sensitive) {
        const auto &policy = profile.default_data_policy;
        if (policy.store.has_value() && *policy.store) {
            mismatches.push_back("sensitive content requires remote storage to be disabled");
        }
        if (policy.allow_uploads) {
            mismatches.push_back("sensitive content cannot use remote uploads");
        }
    }
    if (query.data_policy != nullptr) {
        const auto &requested = *query.data_policy;
        if (requested.store.has_value() && *requested.store &&
            !capabilities.remote_retention.supported) {
            mismatches.push_back("remote retention requested but not supported");
        }
        if (requested.allow_uploads && !capabilities.upload.supported) {
            mismatches.push_back("uploads requested but not supported");
        }
    }
    if (query.budget != nullptr) {
        const auto &budget = *query.budget;
        if (budget.max_output_tokens > capabilities.limits.max_output_tokens) {
            mismatches.push_back("requested output budget exceeds the profile limit");
        }
        if (budget.max_input_tokens > capabilities.limits.max_context_tokens) {
            mismatches.push_back("requested input budget exceeds the profile context limit");
        }
    }
    return mismatches;
}

std::vector<std::string>
unsupported_generation_parameters(const GenerationParamPolicy &policy,
                                  const ModelGenerationOptions &generation) {
    std::vector<std::string> unsupported;
    const auto check = [&unsupported](const char *name, ParamMapping mapping, bool set) {
        if (set && mapping == ParamMapping::Unsupported) {
            unsupported.push_back(std::string("generation parameter is unsupported: ") + name);
        }
    };
    check("max_output_tokens", policy.max_output_tokens, generation.max_output_tokens.has_value());
    check("temperature", policy.temperature, generation.temperature.has_value());
    check("top_p", policy.top_p, generation.top_p.has_value());
    check("seed", policy.seed, generation.seed.has_value());
    check("reasoning_effort", policy.reasoning_effort, generation.reasoning_effort.has_value());
    check("thinking", policy.thinking, generation.thinking.has_value());
    check("service_tier", policy.service_tier, generation.service_tier.has_value());
    return unsupported;
}

void ModelRouter::register_profile(std::shared_ptr<const ModelProfile> profile) {
    if (profile == nullptr) {
        return;
    }
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &candidate) {
        return candidate->id == profile->id;
    });
    if (found != profiles_.end()) {
        *found = std::move(profile);
        return;
    }
    profiles_.push_back(std::move(profile));
}

std::shared_ptr<const ModelProfile> ModelRouter::find(const ModelProfileId &profile_id) const {
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &candidate) {
        return candidate->id == profile_id;
    });
    return found == profiles_.end() ? nullptr : *found;
}

std::vector<std::shared_ptr<const ModelProfile>> ModelRouter::profiles() const { return profiles_; }

Result<RouteDecision> ModelRouter::route(const RouteQuery &query) const {
    RouteDecision decision;
    for (const auto &profile : profiles_) {
        if (!profile->validate()) {
            decision.rejections.push_back(
                RouteRejection{profile->id, "profile manifest is invalid"});
            continue;
        }
        auto mismatches = profile_mismatches(*profile, query);
        if (!mismatches.empty()) {
            for (auto &mismatch : mismatches) {
                decision.rejections.push_back(RouteRejection{profile->id, std::move(mismatch)});
            }
            continue;
        }
        decision.selected_profile = profile->id;
        decision.dialect = profile->dialect;
        decision.profile_digest = profile->profile_digest();
        decision.evidence = query.min_evidence;
        return decision;
    }
    std::string message = "no registered profile satisfies the route query";
    if (!decision.rejections.empty()) {
        message += ": ";
        for (std::size_t index = 0; index < decision.rejections.size(); ++index) {
            if (index > 0) {
                message += "; ";
            }
            message += decision.rejections[index].reason;
        }
    }
    return route_error(std::move(message));
}

} // namespace mira
