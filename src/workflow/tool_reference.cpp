#include <mira/tool_reference.hpp>

#include <mira/json.hpp>
#include <mira/model_schema.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace mira {
namespace {

// Deterministic domain codes for the mira.tool_reference domain: 1 = invalid
// reference string, 2 = extraction failure, 3 = projection input failure.
// safe_message carries bounded identifiers only, never view content.
[[nodiscard]] Error make_reference_error(std::int32_t domain_code, ErrorCode code,
                                         std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_reference";
    error.domain_code = domain_code;
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] Error invalid_reference(std::string message) {
    return make_reference_error(1, ErrorCode::InvalidArgument, std::move(message));
}
[[nodiscard]] Error extraction_failed(std::string message) {
    return make_reference_error(2, ErrorCode::InvalidArgument, std::move(message));
}
[[nodiscard]] Error projection_rejected(std::string message) {
    return make_reference_error(3, ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] std::string truncate(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
        text += "...";
    }
    return text;
}

// Governed vocabulary charset shared with the module layer (capability ids,
// module ids, member tool names): lowercase alphanumeric dot-separated
// segments, '_' and '-' inside segments. Mirrors tool_module.cpp so a
// reference wire name can never drift from the wire namespace rules.
[[nodiscard]] bool is_valid_vocabulary_id(std::string_view id) {
    if (id.empty() || id.front() == '.' || id.back() == '.') {
        return false;
    }
    bool segment_nonempty = false;
    for (const char character : id) {
        if (character == '.') {
            if (!segment_nonempty) {
                return false;
            }
            segment_nonempty = false;
            continue;
        }
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '_' ||
                             character == '-';
        if (!allowed) {
            return false;
        }
        segment_nonempty = true;
    }
    return segment_nonempty;
}

[[nodiscard]] bool is_lowercase_hex(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        const bool allowed =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<Hash> parse_pinned_digest(std::string_view text) {
    if (text.size() != 64 || !is_lowercase_hex(text)) {
        return std::nullopt;
    }
    return digest_from_hex(text);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue digest_to_json(const Hash &digest) { return JsonValue{digest.to_string()}; }

[[nodiscard]] Result<Hash> digest_field(const JsonValue &object, std::string_view key) {
    const auto *value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return invalid_reference("missing digest field '" + std::string(key) + "'");
    }
    const auto digest = digest_from_hex(*value->as_string());
    if (!digest.has_value()) {
        return invalid_reference("digest field '" + std::string(key) + "' is not a digest");
    }
    return Result<Hash>{*digest};
}

[[nodiscard]] JsonValue mode_to_json(ToolReferenceMode mode) {
    return JsonValue{std::string(tool_reference_mode_name(mode))};
}

[[nodiscard]] Result<ToolReferenceMode> mode_field(const JsonValue &object) {
    const auto *value = object.find("mode");
    if (value == nullptr || !value->is_string()) {
        return invalid_reference("reference entry requires a string mode");
    }
    return parse_tool_reference_mode(*value->as_string());
}

// The manifest/projection digest input: everything except the digest field.
[[nodiscard]] JsonValue refs_content_to_json(const WorkflowToolRefManifest &manifest) {
    JsonValue::Array entries;
    entries.reserve(manifest.entries.size());
    for (const auto &entry : manifest.entries) {
        JsonValue::Object object;
        object.emplace_back("step_id", entry.step_id);
        object.emplace_back("wire_name", entry.wire_name);
        object.emplace_back("mode", mode_to_json(entry.mode));
        if (entry.pinned_spec_digest.has_value()) {
            object.emplace_back("pinned_spec_digest", digest_to_json(*entry.pinned_spec_digest));
        }
        entries.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kWorkflowToolRefsSchema));
    root.emplace_back("workflow_id", manifest.workflow_id.to_string());
    root.emplace_back("definition_digest", digest_to_json(manifest.definition_digest));
    root.emplace_back("entries", JsonValue{std::move(entries)});
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue compat_content_to_json(const WorkflowToolCompatProjection &projection) {
    JsonValue::Array entries;
    entries.reserve(projection.entries.size());
    for (const auto &entry : projection.entries) {
        JsonValue::Object object;
        object.emplace_back("step_id", entry.step_id);
        object.emplace_back("wire_name", entry.wire_name);
        object.emplace_back("mode", mode_to_json(entry.mode));
        object.emplace_back("status", std::string(tool_reference_compat_name(entry.status)));
        if (entry.pinned_spec_digest.has_value()) {
            object.emplace_back("pinned_spec_digest", digest_to_json(*entry.pinned_spec_digest));
        }
        if (entry.current_spec_digest.has_value()) {
            object.emplace_back("current_spec_digest", digest_to_json(*entry.current_spec_digest));
        }
        if (!entry.detail.empty()) {
            object.emplace_back("detail", entry.detail);
        }
        entries.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kWorkflowToolCompatSchema));
    root.emplace_back("workflow_id", projection.workflow_id.to_string());
    root.emplace_back("definition_digest", digest_to_json(projection.definition_digest));
    root.emplace_back("state", std::string(workflow_tool_compat_state_name(projection.state)));
    root.emplace_back("entries", JsonValue{std::move(entries)});
    return JsonValue{std::move(root)};
}

// ---------------------------------------------------------------------------
// View lookup shared by extraction and projection
// ---------------------------------------------------------------------------

[[nodiscard]] Result<std::map<std::string, const ExposedToolSpec *>>
indexed_view(std::span<const ExposedToolSpec> view, std::int32_t domain_code) {
    std::map<std::string, const ExposedToolSpec *> index;
    for (const auto &tool : view) {
        const auto [position, inserted] = index.emplace(tool.wire_name, &tool);
        if (!inserted) {
            return make_reference_error(domain_code, ErrorCode::InvalidArgument,
                                        "exposure view contains duplicate wire name '" +
                                            tool.wire_name + "'");
        }
    }
    return Result<std::map<std::string, const ExposedToolSpec *>>{std::move(index)};
}

// ---------------------------------------------------------------------------
// Skeleton bindability (design §7.2): placeholder materialization
// ---------------------------------------------------------------------------

[[nodiscard]] std::string materialized_string(const JsonValue &schema) {
    std::size_t minimum = 1;
    std::size_t maximum = 64;
    if (const auto *value = schema.find("minLength"); value != nullptr && value->is_integer()) {
        const auto length = value->as_integer().value();
        if (length > 0) {
            minimum = static_cast<std::size_t>(length);
        }
    }
    if (const auto *value = schema.find("maxLength"); value != nullptr && value->is_integer()) {
        const auto length = value->as_integer().value();
        if (length >= 0) {
            maximum = static_cast<std::size_t>(length);
        }
    }
    // Keep the probe bounded (RULE-08); a minLength above the cap stays a
    // bounded string and the validator decides — fail closed, never absorb.
    minimum = std::min(minimum, maximum);
    if (minimum > maximum) {
        minimum = maximum;
    }
    if (maximum > 64) {
        maximum = 64;
    }
    if (minimum > maximum) {
        minimum = maximum;
    }
    return std::string(minimum, 'a');
}

[[nodiscard]] JsonValue materialized_number(const JsonValue &schema, bool integral) {
    double value = 0.0;
    const auto *minimum = schema.find("minimum");
    const auto *maximum = schema.find("maximum");
    if (minimum != nullptr && minimum->is_number() && value < minimum->as_number().value()) {
        value = minimum->as_number().value();
    }
    if (maximum != nullptr && maximum->is_number() && value > maximum->as_number().value()) {
        value = maximum->as_number().value();
    }
    if (integral) {
        const auto bounded = value < 0.0 ? -value : value;
        const auto whole = static_cast<std::int64_t>(bounded);
        return JsonValue{value < 0.0 ? -whole : whole};
    }
    return JsonValue{value};
}

struct Materializer {
    std::size_t max_depth;

    [[nodiscard]] JsonValue materialize(const JsonValue &instance, const JsonValue &schema,
                                        std::size_t depth) const {
        if (depth >= max_depth) {
            return JsonValue{}; // Unsatisfiable at this depth; validator decides.
        }

        // A placeholder node takes the minimal instance the schema at its
        // position deterministically admits (design §7.2).
        if (is_placeholder(instance)) {
            return minimal_instance(schema, depth);
        }
        if (instance.is_object()) {
            JsonValue::Object object;
            if (const auto *members = instance.as_object()) {
                object.reserve(members->size());
                for (const auto &member : *members) {
                    const JsonValue *member_schema = nullptr;
                    if (const auto *properties = schema.find("properties");
                        properties != nullptr && properties->is_object()) {
                        member_schema = properties->find(member.first);
                    }
                    object.emplace_back(
                        member.first,
                        materialize(member.second,
                                    member_schema == nullptr ? JsonValue{} : *member_schema,
                                    depth + 1));
                }
            }
            return JsonValue{std::move(object)};
        }
        if (instance.is_array()) {
            JsonValue::Array array;
            if (const auto *items = instance.as_array()) {
                array.reserve(items->size());
                const auto *items_schema = schema.find("items");
                for (const auto &element : *items) {
                    array.emplace_back(materialize(
                        element, items_schema == nullptr ? JsonValue{} : *items_schema, depth + 1));
                }
            }
            return JsonValue{std::move(array)};
        }
        return instance;
    }

  private:
    [[nodiscard]] static bool is_placeholder(const JsonValue &instance) {
        if (!instance.is_object()) {
            return false;
        }
        const auto *members = instance.as_object();
        if (members->size() != 1) {
            return false;
        }
        const auto &member = members->front();
        return member.first == "$param" && member.second.is_string();
    }

    [[nodiscard]] static const JsonValue *declared_type(const JsonValue &schema) {
        const auto *type = schema.find("type");
        if (type == nullptr) {
            return nullptr;
        }
        if (type->is_string()) {
            return type;
        }
        if (const auto *candidates = type->as_array();
            candidates != nullptr && !candidates->empty() && candidates->front().is_string()) {
            return &candidates->front();
        }
        return nullptr;
    }

    [[nodiscard]] JsonValue minimal_instance(const JsonValue &schema, std::size_t depth) const {
        if (const auto *value = schema.find("const"); value != nullptr) {
            return *value;
        }
        if (const auto *value = schema.find("enum");
            value != nullptr && value->is_array() && !value->as_array()->empty()) {
            return value->as_array()->front();
        }
        const auto *type = declared_type(schema);
        if (type == nullptr) {
            return JsonValue{};
        }
        const std::string_view name = *type->as_string();
        if (name == "object") {
            JsonValue::Object object;
            if (const auto *required = schema.find("required");
                required != nullptr && required->is_array()) {
                const auto *properties = schema.find("properties");
                for (const auto &name_value : *required->as_array()) {
                    if (!name_value.is_string()) {
                        continue;
                    }
                    const JsonValue *member_schema = nullptr;
                    if (properties != nullptr && properties->is_object()) {
                        member_schema = properties->find(*name_value.as_string());
                    }
                    object.emplace_back(
                        *name_value.as_string(),
                        minimal_instance(member_schema == nullptr ? JsonValue{} : *member_schema,
                                         depth + 1));
                }
            }
            return JsonValue{std::move(object)};
        }
        if (name == "array") {
            JsonValue::Array array;
            std::size_t count = 1;
            if (const auto *value = schema.find("minItems");
                value != nullptr && value->is_integer()) {
                const auto requested = value->as_integer().value();
                if (requested > 0) {
                    count = static_cast<std::size_t>(std::min<std::int64_t>(requested, 8));
                }
            }
            if (const auto *value = schema.find("maxItems");
                value != nullptr && value->is_integer() && value->as_integer().value() <= 0) {
                count = 0;
            }
            const auto *items_schema = schema.find("items");
            array.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                array.emplace_back(minimal_instance(
                    items_schema == nullptr ? JsonValue{} : *items_schema, depth + 1));
            }
            return JsonValue{std::move(array)};
        }
        if (name == "string") {
            return JsonValue{materialized_string(schema)};
        }
        if (name == "integer") {
            return materialized_number(schema, true);
        }
        if (name == "number") {
            return materialized_number(schema, false);
        }
        if (name == "boolean") {
            return JsonValue{false};
        }
        return JsonValue{}; // "null" and anything unrecognized.
    }
};

// Validates one step's tool input against the current parameters schema.
// Returns nullopt when the skeleton still binds, or the first violation as a
// bounded detail string.
[[nodiscard]] std::optional<std::string> skeleton_violation(const WorkflowStep &step,
                                                            const ExposedToolSpec &tool,
                                                            std::size_t max_detail_bytes) {
    JsonValue input(JsonValue::Object{});
    if (const auto *object = step.arguments.as_object()) {
        for (const auto &member : *object) {
            if (member.first != "tool") {
                input.set(member.first, member.second);
            }
        }
    }
    // Same gate order as dispatch: schemas in the view already passed the
    // subset gate; re-gating here keeps the projection honest if a view was
    // assembled by hand.
    if (!gate_schema_subset(tool.parameters_schema).has_value()) {
        return std::string("parameters schema outside the supported subset");
    }
    const Materializer materializer{kDefaultSchemaSubsetLimits.max_depth};
    const JsonValue probe = materializer.materialize(input, tool.parameters_schema.root, 0);
    const auto violations = validate_instance_against_schema(probe, tool.parameters_schema);
    if (violations.empty()) {
        return std::nullopt;
    }
    const auto &first = violations.front();
    std::string detail = "path '" + first.path + "' keyword '" + first.keyword + "'";
    return truncate(std::move(detail), max_detail_bytes);
}

} // namespace

// ---------------------------------------------------------------------------
// Reference syntax (design §4)
// ---------------------------------------------------------------------------

std::string_view tool_reference_mode_name(ToolReferenceMode mode) {
    switch (mode) {
    case ToolReferenceMode::PinnedDigest:
        return "pinned_digest";
    case ToolReferenceMode::FollowLatest:
        return "follow_latest";
    }
    return "pinned_digest";
}

Result<ToolReferenceMode> parse_tool_reference_mode(std::string_view name) {
    if (name == "pinned_digest") {
        return ToolReferenceMode::PinnedDigest;
    }
    if (name == "follow_latest") {
        return ToolReferenceMode::FollowLatest;
    }
    return invalid_reference("unknown tool reference mode");
}

Result<ToolReference> parse_tool_reference(std::string_view text) {
    if (text.size() > kDefaultToolReferenceLimits.max_reference_bytes) {
        return invalid_reference("tool reference exceeds the length limit");
    }
    if (text.size() <= kToolReferenceScheme.size() ||
        text.substr(0, kToolReferenceScheme.size()) != kToolReferenceScheme) {
        return invalid_reference("tool reference requires the 'toolref:' scheme");
    }
    std::string_view body = text.substr(kToolReferenceScheme.size());
    std::string_view digest_part;
    bool has_digest_part = false;
    if (const auto at = body.find('@'); at != std::string_view::npos) {
        has_digest_part = true;
        digest_part = body.substr(at + 1);
        body = body.substr(0, at);
    }
    if (body.empty() || body.size() > kDefaultToolReferenceLimits.max_wire_name_bytes) {
        return invalid_reference("tool reference wire name is empty or over the length limit");
    }
    if (!is_valid_vocabulary_id(body)) {
        return invalid_reference("tool reference wire name violates the vocabulary charset");
    }
    ToolReference reference;
    reference.wire_name = std::string(body);
    if (!has_digest_part) {
        reference.mode = ToolReferenceMode::FollowLatest;
        return reference;
    }
    auto digest = parse_pinned_digest(digest_part);
    if (!digest.has_value()) {
        return invalid_reference(
            "pinned reference requires exactly 64 lowercase hex digest characters");
    }
    reference.mode = ToolReferenceMode::PinnedDigest;
    reference.pinned_spec_digest = *digest;
    return reference;
}

std::string tool_reference_to_string(const ToolReference &reference) {
    std::string text(kToolReferenceScheme);
    text += reference.wire_name;
    if (reference.mode == ToolReferenceMode::PinnedDigest &&
        reference.pinned_spec_digest.has_value()) {
        text += '@';
        text += reference.pinned_spec_digest->to_string();
    }
    return text;
}

// ---------------------------------------------------------------------------
// Extraction (design §5)
// ---------------------------------------------------------------------------

Result<WorkflowToolRefManifest> extract_workflow_tool_references(
    const WorkflowDefinition &definition, std::span<const ExposedToolSpec> view,
    const ToolRefExtractionOptions &options, const ToolReferenceLimits &limits) {
    if (!validate_workflow_definition(definition).has_value()) {
        return extraction_failed("definition failed structural validation");
    }
    if (definition.workflow_id.value.is_nil()) {
        return extraction_failed("definition requires a workflow id");
    }
    if (options.per_tool_modes.size() > limits.max_entries) {
        return extraction_failed("per-tool mode overrides exceed the entry limit");
    }
    std::map<std::string, ToolReferenceMode> overrides;
    for (const auto &override : options.per_tool_modes) {
        if (override.first.empty()) {
            return extraction_failed("per-tool mode override name must not be empty");
        }
        const auto [position, inserted] = overrides.emplace(override.first, override.second);
        if (!inserted) {
            return extraction_failed("duplicate per-tool mode override for '" + override.first +
                                     "'");
        }
    }

    auto indexed = indexed_view(view, 2);
    if (!indexed.has_value()) {
        return indexed.error();
    }

    const Sha256Digest definition_digest = workflow_definition_digest(definition);
    // TR2 (design §18.1): v1.1 definitions may carry an explicit toolref:
    // string in the reserved "tool" member; the reference's own mode wins over
    // the extraction options and a pinned entry records the digest the
    // reference pins (not the view observation). v1.0 definitions keep the
    // bare-wire-name-only behavior below, so the toolref: form fails closed on
    // the vocabulary charset exactly as before.
    const bool reference_form_allowed = definition.schema_version.minor >= 1;
    std::vector<WorkflowToolRefEntry> entries;
    for (const auto &step : definition.steps) {
        if (step.kind != WorkflowStepKind::ToolCall) {
            continue;
        }
        const auto *tool_member = step.arguments.find("tool");
        if (tool_member == nullptr || !tool_member->is_string()) {
            return extraction_failed("step '" + step.id.to_string() +
                                     "' requires a string 'tool' member");
        }
        const std::string &raw_tool = *tool_member->as_string();
        WorkflowToolRefEntry entry;
        entry.step_id = step.id.to_string();
        if (reference_form_allowed &&
            raw_tool.compare(0, kToolReferenceScheme.size(), kToolReferenceScheme) == 0) {
            auto reference = parse_tool_reference(raw_tool);
            if (!reference.has_value()) {
                return extraction_failed("step '" + step.id.to_string() +
                                         "' carries an invalid tool reference");
            }
            const auto found = indexed.value().find(reference.value().wire_name);
            if (found == indexed.value().end()) {
                return extraction_failed("step '" + step.id.to_string() + "' references '" +
                                         reference.value().wire_name +
                                         "' which is absent from the exposure view");
            }
            entry.wire_name = reference.value().wire_name;
            entry.mode = reference.value().mode;
            if (entry.mode == ToolReferenceMode::PinnedDigest) {
                entry.pinned_spec_digest = reference.value().pinned_spec_digest;
            }
            entries.push_back(std::move(entry));
            continue;
        }
        const std::string &wire_name = raw_tool;
        if (wire_name.size() > limits.max_wire_name_bytes || !is_valid_vocabulary_id(wire_name)) {
            return extraction_failed("step '" + step.id.to_string() +
                                     "' carries a wire name outside the vocabulary charset");
        }
        const auto found = indexed.value().find(wire_name);
        if (found == indexed.value().end()) {
            return extraction_failed("step '" + step.id.to_string() + "' references '" + wire_name +
                                     "' which is absent from the exposure view");
        }
        entry.wire_name = wire_name;
        const auto override = overrides.find(wire_name);
        entry.mode = override != overrides.end() ? override->second : options.default_mode;
        if (entry.mode == ToolReferenceMode::PinnedDigest) {
            entry.pinned_spec_digest = found->second->spec_digest;
        }
        entries.push_back(std::move(entry));
    }
    if (entries.size() > limits.max_entries) {
        return extraction_failed("definition exceeds the reference entry limit");
    }
    std::sort(entries.begin(), entries.end(),
              [](const WorkflowToolRefEntry &left, const WorkflowToolRefEntry &right) {
                  return left.step_id < right.step_id;
              });

    WorkflowToolRefManifest manifest;
    manifest.workflow_id = definition.workflow_id;
    manifest.definition_digest = definition_digest;
    manifest.entries = std::move(entries);
    manifest.digest = canonical_json_digest(refs_content_to_json(manifest));
    return manifest;
}

JsonValue workflow_tool_refs_to_json(const WorkflowToolRefManifest &manifest) {
    JsonValue content = refs_content_to_json(manifest);
    JsonValue::Object root = *content.as_object();
    root.emplace_back("digest", digest_to_json(manifest.digest));
    return JsonValue{std::move(root)};
}

Result<WorkflowToolRefManifest> workflow_tool_refs_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return invalid_reference("tool refs manifest must be an object");
    }
    const auto *schema = json.find("schema");
    if (schema == nullptr || !schema->is_string() ||
        *schema->as_string() != kWorkflowToolRefsSchema) {
        return invalid_reference("tool refs manifest requires schema '" +
                                 std::string(kWorkflowToolRefsSchema) + "'");
    }
    // Closed field set: anything unknown fails closed (DEC-002).
    for (const auto &member : *json.as_object()) {
        const bool known = member.first == "schema" || member.first == "workflow_id" ||
                           member.first == "definition_digest" || member.first == "entries" ||
                           member.first == "digest";
        if (!known) {
            return invalid_reference("unknown manifest field '" + member.first + "'");
        }
    }
    const auto *workflow_id = json.find("workflow_id");
    if (workflow_id == nullptr || !workflow_id->is_string()) {
        return invalid_reference("tool refs manifest requires a workflow id");
    }
    const auto parsed_id = WorkflowId::parse(*workflow_id->as_string());
    if (!parsed_id.has_value() || parsed_id->value.is_nil()) {
        return invalid_reference("tool refs manifest workflow id is not a valid id");
    }
    auto definition_digest = digest_field(json, "definition_digest");
    if (!definition_digest.has_value()) {
        return definition_digest.error();
    }
    const auto *entries_value = json.find("entries");
    if (entries_value == nullptr || !entries_value->is_array()) {
        return invalid_reference("tool refs manifest requires an entries array");
    }
    WorkflowToolRefManifest manifest;
    manifest.workflow_id = *parsed_id;
    manifest.definition_digest = definition_digest.value();
    for (const auto &element : *entries_value->as_array()) {
        if (!element.is_object()) {
            return invalid_reference("reference entry must be an object");
        }
        for (const auto &member : *element.as_object()) {
            const bool known = member.first == "step_id" || member.first == "wire_name" ||
                               member.first == "mode" || member.first == "pinned_spec_digest";
            if (!known) {
                return invalid_reference("unknown reference entry field '" + member.first + "'");
            }
        }
        const auto *step_id = element.find("step_id");
        const auto *wire_name = element.find("wire_name");
        if (step_id == nullptr || !step_id->is_string() || step_id->as_string()->empty() ||
            wire_name == nullptr || !wire_name->is_string() ||
            !is_valid_vocabulary_id(*wire_name->as_string())) {
            return invalid_reference("reference entry requires a step id and a wire name");
        }
        auto mode = mode_field(element);
        if (!mode.has_value()) {
            return mode.error();
        }
        WorkflowToolRefEntry entry;
        entry.step_id = *step_id->as_string();
        entry.wire_name = *wire_name->as_string();
        entry.mode = mode.value();
        if (entry.mode == ToolReferenceMode::PinnedDigest) {
            auto pinned = digest_field(element, "pinned_spec_digest");
            if (!pinned.has_value()) {
                return pinned.error();
            }
            entry.pinned_spec_digest = pinned.value();
        } else if (element.find("pinned_spec_digest") != nullptr) {
            return invalid_reference("follow-latest entry must not carry a pinned digest");
        }
        manifest.entries.push_back(std::move(entry));
    }
    auto digest = digest_field(json, "digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    std::sort(manifest.entries.begin(), manifest.entries.end(),
              [](const WorkflowToolRefEntry &left, const WorkflowToolRefEntry &right) {
                  return left.step_id < right.step_id;
              });
    manifest.digest = canonical_json_digest(refs_content_to_json(manifest));
    if (digest.value() != manifest.digest) {
        return invalid_reference("tool refs manifest digest does not match its content");
    }
    return manifest;
}

Result<void> verify_workflow_tool_refs(const WorkflowToolRefManifest &manifest,
                                       const WorkflowId &workflow_id,
                                       const Sha256Digest &definition_digest) {
    if (manifest.workflow_id.value != workflow_id.value) {
        return projection_rejected("tool refs manifest belongs to a different workflow");
    }
    if (!(manifest.definition_digest == definition_digest)) {
        return projection_rejected("tool refs manifest belongs to a different definition digest");
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Compatibility projection (design §6/§7)
// ---------------------------------------------------------------------------

std::string_view tool_reference_compat_name(ToolReferenceCompat status) {
    switch (status) {
    case ToolReferenceCompat::Resolved:
        return "resolved";
    case ToolReferenceCompat::EvolvedCompatible:
        return "evolved_compatible";
    case ToolReferenceCompat::EvolvedIncompatible:
        return "evolved_incompatible";
    case ToolReferenceCompat::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

std::string_view workflow_tool_compat_state_name(WorkflowToolCompatState state) {
    switch (state) {
    case WorkflowToolCompatState::Runnable:
        return "runnable";
    case WorkflowToolCompatState::Degraded:
        return "degraded";
    case WorkflowToolCompatState::Invalid:
        return "invalid";
    }
    return "invalid";
}

Result<WorkflowToolCompatProjection> project_workflow_tool_compatibility(
    const WorkflowDefinition &definition, const WorkflowToolRefManifest &refs,
    std::span<const ExposedToolSpec> view, const ToolReferenceLimits &limits) {
    const Sha256Digest definition_digest = workflow_definition_digest(definition);
    auto binding = verify_workflow_tool_refs(refs, definition.workflow_id, definition_digest);
    if (!binding.has_value()) {
        return binding.error();
    }
    if (!validate_workflow_definition(definition).has_value()) {
        return projection_rejected("definition failed structural validation");
    }
    if (refs.entries.size() > limits.max_entries) {
        return projection_rejected("manifest exceeds the reference entry limit");
    }

    auto indexed = indexed_view(view, 3);
    if (!indexed.has_value()) {
        return indexed.error();
    }

    WorkflowToolCompatProjection projection;
    projection.workflow_id = refs.workflow_id;
    projection.definition_digest = refs.definition_digest;
    bool degraded = false;
    bool invalid = false;
    for (const auto &entry : refs.entries) {
        ToolRefCompatEntry compat;
        compat.step_id = entry.step_id;
        compat.wire_name = entry.wire_name;
        compat.mode = entry.mode;
        compat.pinned_spec_digest = entry.pinned_spec_digest;

        const auto found = indexed.value().find(entry.wire_name);
        if (found == indexed.value().end()) {
            compat.status = ToolReferenceCompat::Unresolved;
            invalid = true;
        } else {
            compat.current_spec_digest = found->second->spec_digest;
            if (entry.mode == ToolReferenceMode::FollowLatest ||
                entry.pinned_spec_digest == found->second->spec_digest) {
                compat.status = ToolReferenceCompat::Resolved;
            } else {
                const auto step = std::find_if(definition.steps.begin(), definition.steps.end(),
                                               [&](const WorkflowStep &candidate) {
                                                   return candidate.id.to_string() == entry.step_id;
                                               });
                if (step == definition.steps.end()) {
                    return projection_rejected("manifest entry '" + entry.step_id +
                                               "' has no matching step in the definition");
                }
                auto violation = skeleton_violation(*step, *found->second, limits.max_detail_bytes);
                if (violation.has_value()) {
                    compat.status = ToolReferenceCompat::EvolvedIncompatible;
                    compat.detail = std::move(*violation);
                    invalid = true;
                } else {
                    compat.status = ToolReferenceCompat::EvolvedCompatible;
                    degraded = true;
                }
            }
        }
        projection.entries.push_back(std::move(compat));
    }
    projection.state = invalid    ? WorkflowToolCompatState::Invalid
                       : degraded ? WorkflowToolCompatState::Degraded
                                  : WorkflowToolCompatState::Runnable;
    projection.digest = canonical_json_digest(compat_content_to_json(projection));
    return projection;
}

WorkflowToolCompatDecision
admit_workflow_run_by_tool_compat(const WorkflowToolCompatProjection &projection) {
    WorkflowToolCompatDecision decision;
    decision.state = projection.state;
    decision.admitted = projection.state != WorkflowToolCompatState::Invalid;
    if (!decision.admitted) {
        for (const auto &entry : projection.entries) {
            if (entry.status == ToolReferenceCompat::Unresolved ||
                entry.status == ToolReferenceCompat::EvolvedIncompatible) {
                decision.reason =
                    truncate("step '" + entry.step_id + "' " +
                                 std::string(tool_reference_compat_name(entry.status)),
                             kDefaultToolReferenceLimits.max_detail_bytes);
                break;
            }
        }
    }
    return decision;
}

JsonValue workflow_tool_compat_to_json(const WorkflowToolCompatProjection &projection) {
    JsonValue content = compat_content_to_json(projection);
    JsonValue::Object root = *content.as_object();
    root.emplace_back("digest", digest_to_json(projection.digest));
    return JsonValue{std::move(root)};
}

// Strict inverse (TR2): closed field set, canonical digest recomputed over the
// content, statuses/modes constrained to the frozen name sets. Used by the
// tool-compat-degraded event parser so the embedded artifact cannot drift.
Result<WorkflowToolCompatProjection> workflow_tool_compat_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return invalid_reference("tool compat projection must be an object");
    }
    const auto *schema = json.find("schema");
    if (schema == nullptr || !schema->is_string() ||
        *schema->as_string() != kWorkflowToolCompatSchema) {
        return invalid_reference("tool compat projection requires schema '" +
                                 std::string(kWorkflowToolCompatSchema) + "'");
    }
    for (const auto &member : *json.as_object()) {
        const bool known = member.first == "schema" || member.first == "workflow_id" ||
                           member.first == "definition_digest" || member.first == "state" ||
                           member.first == "entries" || member.first == "digest";
        if (!known) {
            return invalid_reference("unknown projection field '" + member.first + "'");
        }
    }
    const auto *workflow_id = json.find("workflow_id");
    if (workflow_id == nullptr || !workflow_id->is_string()) {
        return invalid_reference("tool compat projection requires a workflow id");
    }
    const auto parsed_id = WorkflowId::parse(*workflow_id->as_string());
    if (!parsed_id.has_value() || parsed_id->value.is_nil()) {
        return invalid_reference("tool compat projection workflow id is not a valid id");
    }
    auto definition_digest = digest_field(json, "definition_digest");
    if (!definition_digest.has_value()) {
        return definition_digest.error();
    }
    const auto *state_value = json.find("state");
    if (state_value == nullptr || !state_value->is_string()) {
        return invalid_reference("tool compat projection requires a string state");
    }
    WorkflowToolCompatState state = WorkflowToolCompatState::Invalid;
    if (*state_value->as_string() == "runnable") {
        state = WorkflowToolCompatState::Runnable;
    } else if (*state_value->as_string() == "degraded") {
        state = WorkflowToolCompatState::Degraded;
    } else if (*state_value->as_string() != "invalid") {
        return invalid_reference("tool compat projection state is unknown");
    }
    const auto *entries_value = json.find("entries");
    if (entries_value == nullptr || !entries_value->is_array()) {
        return invalid_reference("tool compat projection requires an entries array");
    }
    WorkflowToolCompatProjection projection;
    projection.workflow_id = *parsed_id;
    projection.definition_digest = definition_digest.value();
    projection.state = state;
    for (const auto &element : *entries_value->as_array()) {
        if (!element.is_object()) {
            return invalid_reference("compat entry must be an object");
        }
        for (const auto &member : *element.as_object()) {
            const bool known = member.first == "step_id" || member.first == "wire_name" ||
                               member.first == "mode" || member.first == "status" ||
                               member.first == "pinned_spec_digest" ||
                               member.first == "current_spec_digest" || member.first == "detail";
            if (!known) {
                return invalid_reference("unknown compat entry field '" + member.first + "'");
            }
        }
        const auto *step_id = element.find("step_id");
        const auto *wire_name = element.find("wire_name");
        if (step_id == nullptr || !step_id->is_string() || step_id->as_string()->empty() ||
            wire_name == nullptr || !wire_name->is_string() ||
            !is_valid_vocabulary_id(*wire_name->as_string())) {
            return invalid_reference("compat entry requires a step id and a wire name");
        }
        auto mode = mode_field(element);
        if (!mode.has_value()) {
            return mode.error();
        }
        const auto *status_value = element.find("status");
        if (status_value == nullptr || !status_value->is_string()) {
            return invalid_reference("compat entry requires a string status");
        }
        ToolReferenceCompat status = ToolReferenceCompat::Unresolved;
        if (*status_value->as_string() == "resolved") {
            status = ToolReferenceCompat::Resolved;
        } else if (*status_value->as_string() == "evolved_compatible") {
            status = ToolReferenceCompat::EvolvedCompatible;
        } else if (*status_value->as_string() == "evolved_incompatible") {
            status = ToolReferenceCompat::EvolvedIncompatible;
        } else if (*status_value->as_string() != "unresolved") {
            return invalid_reference("compat entry status is unknown");
        }
        ToolRefCompatEntry entry;
        entry.step_id = *step_id->as_string();
        entry.wire_name = *wire_name->as_string();
        entry.mode = mode.value();
        entry.status = status;
        if (element.find("pinned_spec_digest") != nullptr) {
            auto pinned = digest_field(element, "pinned_spec_digest");
            if (!pinned.has_value()) {
                return pinned.error();
            }
            entry.pinned_spec_digest = pinned.value();
        }
        if (element.find("current_spec_digest") != nullptr) {
            auto current = digest_field(element, "current_spec_digest");
            if (!current.has_value()) {
                return current.error();
            }
            entry.current_spec_digest = current.value();
        }
        if (const auto *detail = element.find("detail"); detail != nullptr) {
            if (!detail->is_string()) {
                return invalid_reference("compat entry detail must be a string");
            }
            entry.detail = *detail->as_string();
        }
        projection.entries.push_back(std::move(entry));
    }
    auto digest = digest_field(json, "digest");
    if (!digest.has_value()) {
        return digest.error();
    }
    std::sort(projection.entries.begin(), projection.entries.end(),
              [](const ToolRefCompatEntry &left, const ToolRefCompatEntry &right) {
                  return left.step_id < right.step_id;
              });
    projection.digest = canonical_json_digest(compat_content_to_json(projection));
    if (digest.value() != projection.digest) {
        return invalid_reference("tool compat projection digest does not match its content");
    }
    return projection;
}

} // namespace mira
