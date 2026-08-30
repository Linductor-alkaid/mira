#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_profile.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// Secret- and volatile-exclusion rules for digests and events. Wire digests
// must be stable across reruns, so Authorization headers, signed-URL query
// parameters, upload tokens and random multipart boundaries never contribute.
struct WireDigestRules final {
    // Header names (case-insensitive) replaced by "[redacted]" before hashing.
    std::vector<std::string> redacted_headers = {
        "authorization", "x-api-key", "cookie", "proxy-authorization",
    };
    // Query parameter names (case-insensitive) dropped before hashing.
    std::vector<std::string> redacted_query_parameters = {
        "sig", "signature", "token", "access_token", "api_key", "apikey", "expires",
    };
    // Multipart boundary marker replaced with a fixed placeholder.
    bool redact_multipart_boundary = true;
};

extern const WireDigestRules kDefaultWireDigestRules;

// Computes the wire request digest after applying the exclusion rules. The
// input is the exact JSON body plus headers the transport would send.
[[nodiscard]] Hash wire_request_digest(const JsonValue &wire_body,
                                       const std::vector<std::pair<std::string, std::string>> &headers,
                                       const WireDigestRules &rules = kDefaultWireDigestRules);

// Prompt digest over authoritative and untrusted input; artifact parts
// contribute their stored digest, never payload bytes.
[[nodiscard]] Hash prompt_digest(std::span<const ModelInputItem> input);

// Decision digest binds schema identity and the canonical decision payload.
[[nodiscard]] Hash decision_digest(const SchemaId &schema_id, const SemanticVersion &version,
                                   const JsonValue &decision);

// Digest over the tools snapshot exposed to one request.
[[nodiscard]] Hash tool_snapshot_digest(std::span<const ExposedToolSpec> tools);

// Digest over the data policy actually applied to a request.
[[nodiscard]] Hash data_policy_digest(const ModelDataPolicy &policy);

// Replaces secret-bearing content with "[redacted]" markers. Used before any
// wire payload or header set enters an event, log or diagnostic artifact.
[[nodiscard]] JsonValue sanitize_wire_for_events(const JsonValue &wire_body,
                                                 const WireDigestRules &rules = kDefaultWireDigestRules);

[[nodiscard]] std::vector<std::pair<std::string, std::string>>
sanitize_headers_for_events(const std::vector<std::pair<std::string, std::string>> &headers,
                            const WireDigestRules &rules = kDefaultWireDigestRules);

// Strips the query component for safe logging: "https://h/p?x=1" -> "https://h/p".
[[nodiscard]] std::string redact_url_for_log(std::string_view url);

// True when the text contains no secret marker strings; defensive check used
// by redaction tests.
[[nodiscard]] bool contains_none(std::string_view text, std::span<const std::string> needles);

} // namespace mira
