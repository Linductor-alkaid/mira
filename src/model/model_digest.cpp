#include <mira/model_digest.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace mira {

const WireDigestRules kDefaultWireDigestRules{};

namespace {

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] bool listed(std::span<const std::string> needles, std::string_view haystack) {
    return std::any_of(needles.begin(), needles.end(), [&](const std::string &needle) {
        return lowercase(needle) == lowercase(haystack);
    });
}

[[nodiscard]] JsonValue sanitize_value(const JsonValue &value, const WireDigestRules &rules) {
    switch (value.kind()) {
    case JsonValue::Kind::Object: {
        JsonValue::Object object;
        for (const auto &member : *value.as_object()) {
            object.emplace_back(member.first, sanitize_value(member.second, rules));
        }
        return JsonValue(std::move(object));
    }
    case JsonValue::Kind::Array: {
        JsonValue::Array array;
        for (const auto &item : *value.as_array()) {
            array.push_back(sanitize_value(item, rules));
        }
        return JsonValue(std::move(array));
    }
    case JsonValue::Kind::String: {
        const auto &text = *value.as_string();
        if (text.rfind("--", 0) == 0 && rules.redact_multipart_boundary &&
            text.find("\r\n") == std::string::npos && text.size() <= 64 &&
            std::all_of(text.begin(), text.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
                       c == '=';
            })) {
            return JsonValue("[boundary]");
        }
        return value;
    }
    default:
        return value;
    }
}

} // namespace

Hash wire_request_digest(const JsonValue &wire_body,
                         const std::vector<std::pair<std::string, std::string>> &headers,
                         const WireDigestRules &rules) {
    JsonValue::Object envelope;
    envelope.emplace_back("body", sanitize_value(wire_body, rules));

    JsonValue::Array header_json;
    for (const auto &header : sanitize_headers_for_events(headers, rules)) {
        header_json.emplace_back(
            JsonValue::Object{{header.first, header.second}});
    }
    envelope.emplace_back("headers", std::move(header_json));
    return canonical_json_digest(JsonValue(std::move(envelope)));
}

Hash prompt_digest(std::span<const ModelInputItem> input) {
    JsonValue::Array items;
    for (const auto &item : input) {
        JsonValue::Object item_json;
        switch (item.role) {
        case ModelRole::System:
            item_json.emplace_back("role", "system");
            break;
        case ModelRole::Developer:
            item_json.emplace_back("role", "developer");
            break;
        case ModelRole::User:
            item_json.emplace_back("role", "user");
            break;
        case ModelRole::Assistant:
            item_json.emplace_back("role", "assistant");
            break;
        case ModelRole::Unknown:
            item_json.emplace_back("role", "unknown");
            break;
        }
        item_json.emplace_back("provenance", item.provenance.source);
        JsonValue::Array parts;
        for (const auto &part : item.content) {
            JsonValue::Object part_json;
            if (const auto *text = std::get_if<TextPart>(&part)) {
                part_json.emplace_back("kind", "text");
                // Only the text digest contributes: prompts can be huge and the
                // digest must remain bounded.
                part_json.emplace_back("digest", digest_string(text->text).to_string());
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                part_json.emplace_back("kind", "image");
                part_json.emplace_back("digest", image->source.digest.to_string());
            } else if (const auto *file = std::get_if<FilePart>(&part)) {
                part_json.emplace_back("kind", "file");
                part_json.emplace_back("digest", file->source.digest.to_string());
            }
            parts.emplace_back(std::move(part_json));
        }
        item_json.emplace_back("content", std::move(parts));
        items.emplace_back(std::move(item_json));
    }
    return canonical_json_digest(JsonValue(std::move(items)));
}

Hash decision_digest(const SchemaId &schema_id, const SemanticVersion &version,
                     const JsonValue &decision) {
    JsonValue::Object root;
    root.emplace_back("schema_id", schema_id.to_string());
    root.emplace_back("schema_version",
                      JsonValue::Object{{"major", static_cast<std::int64_t>(version.major)},
                                        {"minor", static_cast<std::int64_t>(version.minor)},
                                        {"patch", static_cast<std::int64_t>(version.patch)}});
    root.emplace_back("decision", decision);
    return canonical_json_digest(JsonValue(std::move(root)));
}

Hash tool_snapshot_digest(std::span<const ExposedToolSpec> tools) {
    JsonValue::Array items;
    for (const auto &tool : tools) {
        JsonValue::Object item;
        item.emplace_back("tool_id", tool.tool_id.to_string());
        item.emplace_back("version",
                          JsonValue::Object{{"major", static_cast<std::int64_t>(tool.version.major)},
                                            {"minor", static_cast<std::int64_t>(tool.version.minor)},
                                            {"patch", static_cast<std::int64_t>(tool.version.patch)}});
        item.emplace_back("wire_name", tool.wire_name);
        item.emplace_back("parameters", tool.parameters_schema.root);
        items.emplace_back(std::move(item));
    }
    return canonical_json_digest(JsonValue(std::move(items)));
}

Hash data_policy_digest(const ModelDataPolicy &policy) {
    JsonValue::Object root;
    if (policy.store.has_value()) {
        root.emplace_back("store", *policy.store);
    } else {
        root.emplace_back("store", "unset");
    }
    root.emplace_back("allow_uploads", policy.allow_uploads);
    if (policy.region.has_value()) {
        root.emplace_back("region", *policy.region);
    }
    if (policy.organization.has_value()) {
        root.emplace_back("organization", *policy.organization);
    }
    if (policy.project.has_value()) {
        root.emplace_back("project", *policy.project);
    }
    root.emplace_back("remote_retention",
                      static_cast<std::int64_t>(policy.remote_retention.count()));
    return canonical_json_digest(JsonValue(std::move(root)));
}

JsonValue sanitize_wire_for_events(const JsonValue &wire_body, const WireDigestRules &rules) {
    return sanitize_value(wire_body, rules);
}

std::vector<std::pair<std::string, std::string>>
sanitize_headers_for_events(const std::vector<std::pair<std::string, std::string>> &headers,
                            const WireDigestRules &rules) {
    std::vector<std::pair<std::string, std::string>> sanitized;
    sanitized.reserve(headers.size());
    for (const auto &header : headers) {
        if (listed(rules.redacted_headers, header.first)) {
            sanitized.emplace_back(header.first, "[redacted]");
        } else {
            sanitized.emplace_back(header.first, header.second);
        }
    }
    return sanitized;
}

std::string redact_url_for_log(std::string_view url) {
    const auto scheme_end = url.find("://");
    const auto path_start =
        scheme_end == std::string_view::npos ? 0 : url.find('/', scheme_end + 3);
    const auto query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return std::string(url);
    }
    if (path_start == std::string_view::npos || query_start < path_start) {
        // No path at all: keep scheme://host only.
        const auto host_end = std::min(query_start, url.size());
        return std::string(url.substr(0, host_end));
    }
    return std::string(url.substr(0, query_start));
}

bool contains_none(std::string_view text, std::span<const std::string> needles) {
    return std::none_of(needles.begin(), needles.end(), [&](const std::string &needle) {
        return !needle.empty() && text.find(needle) != std::string_view::npos;
    });
}

} // namespace mira
