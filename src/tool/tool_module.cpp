#include <mira/tool_module.hpp>

#include <mira/model_schema.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace mira {

namespace {

Error make_module_error(ErrorCode code, std::string safe_message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_module";
    error.safe_message = std::move(safe_message);
    return error;
}

// Governed charset shared by capability ids, module ids and member tool
// names: lowercase alphanumeric segments separated by single dots, with '_'
// and '-' allowed inside segments. Fail closed on everything else so an id
// can never smuggle whitespace, path text or mixed-case aliases.
bool is_valid_vocabulary_id(std::string_view id) {
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

std::string id_charset_message(std::string_view what, std::string_view value) {
    return std::string(what) + " is not a valid lowercase dot-separated id: '" + std::string(value) +
           "'";
}

[[nodiscard]] JsonValue id_list_to_json(const std::vector<CapabilityId> &ids) {
    JsonValue::Array array;
    array.reserve(ids.size());
    for (const auto &id : ids) {
        array.emplace_back(id);
    }
    return JsonValue{std::move(array)};
}

[[nodiscard]] JsonValue string_list_to_json(const std::vector<std::string> &values) {
    JsonValue::Array array;
    array.reserve(values.size());
    for (const auto &value : values) {
        array.emplace_back(value);
    }
    return JsonValue{std::move(array)};
}

[[nodiscard]] JsonValue data_access_to_json(const ToolDataAccess &data_access) {
    JsonValue::Object object;
    object.emplace_back("reads", string_list_to_json(data_access.reads));
    object.emplace_back("writes", string_list_to_json(data_access.writes));
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue tool_spec_to_json(const ModuleToolSpec &tool) {
    JsonValue::Object object;
    object.emplace_back("name", tool.name);
    object.emplace_back("version", semantic_version_to_json(tool.version));
    object.emplace_back("description", tool.description);
    object.emplace_back("arguments_schema", tool.arguments_schema.root);
    object.emplace_back("result_schema", tool.result_schema.root);
    object.emplace_back("required_capabilities", id_list_to_json(tool.required_capabilities));
    std::string side_effect;
    switch (tool.side_effect) {
    case ActionRisk::R0ReadOnly:
        side_effect = "read_only";
        break;
    case ActionRisk::R1ReversibleLow:
        side_effect = "reversible_low";
        break;
    case ActionRisk::R2UserVisible:
        side_effect = "user_visible";
        break;
    case ActionRisk::R3Sensitive:
        side_effect = "sensitive";
        break;
    case ActionRisk::R4Critical:
        side_effect = "critical";
        break;
    }
    object.emplace_back("side_effect", side_effect);
    object.emplace_back("data_access", data_access_to_json(tool.data_access));
    return JsonValue{std::move(object)};
}

// Normalized manifest projection; the digest input. Building the digest from
// the validated structure (not raw input bytes) makes it independent of the
// producer's key order and whitespace.
[[nodiscard]] JsonValue tool_module_manifest_to_json(const ToolModuleManifest &manifest,
                                                     bool include_signature) {
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolModuleManifestSchema));
    root.emplace_back("schema_version", std::string(kToolModuleManifestSchemaVersion));
    root.emplace_back("module_id", manifest.module_id);
    root.emplace_back("version", semantic_version_to_json(manifest.version));

    JsonValue::Object origin;
    origin.emplace_back("isolation", std::string(tool_module_origin_name(manifest.origin)));
    if (!manifest.signer.empty()) {
        origin.emplace_back("signer", manifest.signer);
    }
    if (!manifest.signature_algorithm.empty()) {
        origin.emplace_back("signature_algorithm", manifest.signature_algorithm);
    }
    if (include_signature && !manifest.signature.empty()) {
        origin.emplace_back("signature", manifest.signature);
    }
    root.emplace_back("origin", JsonValue{std::move(origin)});
    root.emplace_back("min_mira_module_abi",
                      static_cast<std::int64_t>(manifest.min_mira_module_abi));
    root.emplace_back("required_capabilities", id_list_to_json(manifest.required_capabilities));
    root.emplace_back("infra_capabilities", id_list_to_json(manifest.infra_capabilities));

    JsonValue::Array tools;
    tools.reserve(manifest.tools.size());
    for (const auto &tool : manifest.tools) {
        tools.emplace_back(tool_spec_to_json(tool));
    }
    root.emplace_back("tools", JsonValue{std::move(tools)});
    root.emplace_back("conflicts_with", string_list_to_json(manifest.conflicts_with));

    JsonValue::Object resources;
    resources.emplace_back(
        "max_total_concurrent_invocations",
        static_cast<std::int64_t>(manifest.resources.max_total_concurrent_invocations));
    resources.emplace_back("max_total_result_bytes",
                           static_cast<std::int64_t>(manifest.resources.max_total_result_bytes));
    root.emplace_back("resources", JsonValue{std::move(resources)});
    return JsonValue{std::move(root)};
}

[[nodiscard]] Result<std::vector<CapabilityId>>
parse_capability_list(const JsonValue *field, std::string_view what,
                      const CapabilityCatalog &catalog, const ToolModuleLimits &limits) {
    std::vector<CapabilityId> ids;
    if (field == nullptr) {
        return ids;
    }
    const auto *array = field->as_array();
    if (array == nullptr) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 std::string(what) + " must be an array");
    }
    if (array->size() > limits.max_capabilities) {
        return make_module_error(ErrorCode::ResourceExhausted, std::string(what) +
                                                                     " exceeds the capability " +
                                                                     "list limit");
    }
    std::set<std::string_view> seen;
    for (const auto &entry : *array) {
        const auto *id = entry.as_string();
        if (id == nullptr) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string(what) + " entries must be strings");
        }
        if (!is_valid_vocabulary_id(*id)) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     id_charset_message(what, *id));
        }
        if (!catalog.contains(*id)) {
            return make_module_error(ErrorCode::UnsupportedCapability,
                                     std::string(what) + " references capability '" + *id +
                                         "' outside the governed catalog");
        }
        if (!seen.insert(*id).second) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string(what) + " contains duplicate '" + *id + "'");
        }
        ids.push_back(*id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

[[nodiscard]] Result<std::vector<std::string>>
parse_label_list(const JsonValue *field, std::string_view what, const ToolModuleLimits &limits,
                 bool require_module_id_charset = false) {
    std::vector<std::string> values;
    if (field == nullptr) {
        return values;
    }
    const auto *array = field->as_array();
    if (array == nullptr) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 std::string(what) + " must be an array");
    }
    if (array->size() > limits.max_data_access_entries) {
        return make_module_error(ErrorCode::ResourceExhausted,
                                 std::string(what) + " exceeds the list limit");
    }
    std::set<std::string_view> seen;
    for (const auto &entry : *array) {
        const auto *value = entry.as_string();
        if (value == nullptr || value->empty() || value->size() > limits.max_label_bytes) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string(what) + " entries must be non-empty strings " +
                                         "within the label limit");
        }
        if (require_module_id_charset && !is_valid_vocabulary_id(*value)) {
            return make_module_error(ErrorCode::InvalidArgument, id_charset_message(what, *value));
        }
        if (!seen.insert(*value).second) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string(what) + " contains duplicate '" + *value + "'");
        }
        values.push_back(*value);
    }
    std::sort(values.begin(), values.end());
    return values;
}

[[nodiscard]] Result<ModuleToolSpec> parse_tool_spec(const JsonValue &json,
                                                     const CapabilityCatalog &catalog,
                                                     const ToolModuleLimits &limits) {
    if (!json.is_object()) {
        return make_module_error(ErrorCode::InvalidArgument, "member tool must be an object");
    }
    ModuleToolSpec tool;
    const auto *name = json.find("name");
    if (name == nullptr || !name->is_string()) {
        return make_module_error(ErrorCode::InvalidArgument, "member tool requires a name");
    }
    tool.name = *name->as_string();
    if (!is_valid_vocabulary_id(tool.name)) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 id_charset_message("member tool name", tool.name));
    }
    if (tool.name.size() > limits.max_name_bytes) {
        return make_module_error(ErrorCode::ResourceExhausted,
                                 "member tool name exceeds the name limit");
    }

    const auto *version = json.find("version");
    if (version == nullptr) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "member tool '" + tool.name + "' requires a version");
    }
    auto parsed_version = semantic_version_from_json(*version);
    if (!parsed_version.has_value()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "member tool '" + tool.name + "' has an invalid version");
    }
    tool.version = parsed_version.value();

    const auto *description = json.find("description");
    if (description != nullptr) {
        const auto *text = description->as_string();
        if (text == nullptr || text->size() > limits.max_description_bytes) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "member tool '" + tool.name +
                                         "' description must be a string within the limit");
        }
        tool.description = *text;
    }

    for (const auto &[field, target] : {std::pair{"arguments_schema", &tool.arguments_schema},
                                        std::pair{"result_schema", &tool.result_schema}}) {
        const auto *schema = json.find(field);
        if (schema == nullptr || !schema->is_object()) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "member tool '" + tool.name + "' requires " + field);
        }
        target->root = *schema;
        auto gated = gate_schema_subset(*target);
        if (!gated.has_value()) {
            Error error = gated.error();
            error.domain = "mira.tool_module";
            error.safe_message = "member tool '" + tool.name + "' " + field +
                                 " is outside the shared schema subset: " + error.safe_message;
            return error;
        }
    }

    auto required = parse_capability_list(json.find("required_capabilities"),
                                          "member tool '" + tool.name + "' required_capabilities",
                                          catalog, limits);
    if (!required.has_value()) {
        return required.error();
    }
    tool.required_capabilities = std::move(required.value());

    const auto *side_effect = json.find("side_effect");
    if (side_effect == nullptr || !side_effect->is_string()) {
        return make_module_error(ErrorCode::InvalidArgument, "member tool '" + tool.name +
                                                                 "' requires an explicit " +
                                                                 "side_effect");
    }
    const std::string_view effect_name(*side_effect->as_string());
    if (effect_name == "read_only") {
        tool.side_effect = ActionRisk::R0ReadOnly;
    } else if (effect_name == "reversible_low") {
        tool.side_effect = ActionRisk::R1ReversibleLow;
    } else if (effect_name == "user_visible") {
        tool.side_effect = ActionRisk::R2UserVisible;
    } else if (effect_name == "sensitive") {
        tool.side_effect = ActionRisk::R3Sensitive;
    } else if (effect_name == "critical") {
        tool.side_effect = ActionRisk::R4Critical;
    } else {
        return make_module_error(ErrorCode::InvalidArgument, "member tool '" + tool.name +
                                                                 "' has an unknown side_effect '" +
                                                                 std::string(effect_name) + "'");
    }

    const auto *data_access = json.find("data_access");
    if (data_access != nullptr) {
        if (!data_access->is_object()) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "member tool '" + tool.name +
                                         "' data_access must be an object");
        }
        auto reads = parse_label_list(data_access->find("reads"),
                                      "member tool '" + tool.name + "' data_access.reads", limits);
        if (!reads.has_value()) {
            return reads.error();
        }
        tool.data_access.reads = std::move(reads.value());
        auto writes =
            parse_label_list(data_access->find("writes"),
                             "member tool '" + tool.name + "' data_access.writes", limits);
        if (!writes.has_value()) {
            return writes.error();
        }
        tool.data_access.writes = std::move(writes.value());
    }
    return tool;
}

[[nodiscard]] Result<std::uint32_t> parse_bounded_uint32(const JsonValue *field,
                                                         std::string_view what,
                                                         std::uint32_t maximum) {
    if (field == nullptr || !field->is_integer()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 std::string(what) + " must be an integer");
    }
    const auto value = field->as_integer();
    if (!value.has_value() || *value < 1 || *value > static_cast<std::int64_t>(maximum)) {
        return make_module_error(ErrorCode::InvalidArgument, std::string(what) +
                                                                 " must be within [1, " +
                                                                 std::to_string(maximum) + "]");
    }
    return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] Result<std::uint64_t> parse_bounded_uint64(const JsonValue *field,
                                                         std::string_view what,
                                                         std::uint64_t maximum) {
    if (field == nullptr || !field->is_integer()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 std::string(what) + " must be an integer");
    }
    const auto value = field->as_integer();
    if (!value.has_value() || *value < 1 || *value > static_cast<std::int64_t>(maximum)) {
        return make_module_error(ErrorCode::InvalidArgument, std::string(what) +
                                                                 " must be within [1, " +
                                                                 std::to_string(maximum) + "]");
    }
    return static_cast<std::uint64_t>(*value);
}

[[nodiscard]] JsonValue capability_catalog_to_json(const std::vector<CapabilityDescriptor> &entries) {
    JsonValue::Array array;
    array.reserve(entries.size());
    for (const auto &entry : entries) {
        JsonValue::Object object;
        object.emplace_back("id", entry.id);
        object.emplace_back("kind", std::string(entry.kind == CapabilityKind::Counted
                                                    ? "counted"
                                                    : "boolean"));
        object.emplace_back("summary", entry.summary);
        array.emplace_back(JsonValue{std::move(object)});
    }
    return JsonValue{std::move(array)};
}

} // namespace

// ---------------------------------------------------------------------------
// CapabilityCatalog
// ---------------------------------------------------------------------------

Result<CapabilityCatalog> CapabilityCatalog::make(std::vector<CapabilityDescriptor> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const CapabilityDescriptor &lhs, const CapabilityDescriptor &rhs) {
                  return lhs.id < rhs.id;
              });
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!is_valid_vocabulary_id(entries[index].id)) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     id_charset_message("capability id", entries[index].id));
        }
        if (index > 0 && entries[index].id == entries[index - 1].id) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "capability catalog contains duplicate '" + entries[index].id +
                                         "'");
        }
    }
    const Hash digest = canonical_json_digest(capability_catalog_to_json(entries));
    return CapabilityCatalog{std::move(entries), digest};
}

const CapabilityCatalog &CapabilityCatalog::core() {
    static const CapabilityCatalog catalog = [] {
        std::vector<CapabilityDescriptor> entries;
        entries.push_back(
            {"env.device.state", CapabilityKind::Boolean, "Environment reports device state."});
        entries.push_back({"env.epoch.invalidation", CapabilityKind::Boolean,
                           "Environment invalidates observation epochs on topology, permission "
                           "or session changes."});
        entries.push_back({"env.foreground.app", CapabilityKind::Boolean,
                           "Environment reports the foreground application."});
        entries.push_back(
            {"env.input.discrete", CapabilityKind::Boolean, "Environment accepts discrete input."});
        entries.push_back({"env.input.release", CapabilityKind::Boolean,
                           "Environment can release in-flight input and unblock platform waits."});
        entries.push_back({"env.observation.atomic", CapabilityKind::Boolean,
                           "Environment captures observation components atomically."});
        entries.push_back({"env.perception.sources", CapabilityKind::Counted,
                           "On-device perception sources available to tools."});
        entries.push_back({"env.screen.capture", CapabilityKind::Boolean,
                           "Environment can capture the screen."});
        entries.push_back(
            {"env.ui.tree", CapabilityKind::Boolean, "Environment exposes a structured UI tree."});
        entries.push_back({"host.bridge.rpc", CapabilityKind::Boolean,
                           "Host provides an RPC bridge for tool modules."});
        entries.push_back({"host.process.supervision", CapabilityKind::Boolean,
                           "Host supervises out-of-process tool modules."});
        entries.push_back({"tool.fs.root", CapabilityKind::Boolean,
                           "Tool requires a filesystem root granted by the host."});
        entries.push_back({"tool.net.egress", CapabilityKind::Boolean,
                           "Tool requires network egress granted by the host."});
        auto made = make(std::move(entries));
        if (!made.has_value()) {
            // The fixed vocabulary above is valid by construction; an empty
            // catalog keeps the invariant that core() never throws.
            return CapabilityCatalog{};
        }
        return std::move(made).value();
    }();
    return catalog;
}

const CapabilityDescriptor *CapabilityCatalog::find(std::string_view id) const {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const CapabilityDescriptor &entry, std::string_view key) { return entry.id < key; });
    if (found == entries_.end() || found->id != id) {
        return nullptr;
    }
    return &*found;
}

bool CapabilityCatalog::contains(std::string_view id) const { return find(id) != nullptr; }

std::vector<CapabilityId>
derive_environment_capabilities(const EnvironmentCapabilities &capabilities) {
    std::vector<CapabilityId> derived;
    derived.reserve(9);
    if (capabilities.screen_capture) {
        derived.push_back("env.screen.capture");
    }
    if (capabilities.ui_tree) {
        derived.push_back("env.ui.tree");
    }
    if (capabilities.foreground_app) {
        derived.push_back("env.foreground.app");
    }
    if (capabilities.device_state) {
        derived.push_back("env.device.state");
    }
    if (capabilities.perception_sources >= 1) {
        derived.push_back("env.perception.sources");
    }
    if (capabilities.atomic_observation) {
        derived.push_back("env.observation.atomic");
    }
    if (capabilities.discrete_input) {
        derived.push_back("env.input.discrete");
    }
    if (capabilities.input_release) {
        derived.push_back("env.input.release");
    }
    if (capabilities.epoch_invalidation) {
        derived.push_back("env.epoch.invalidation");
    }
    std::sort(derived.begin(), derived.end());
    return derived;
}

// ---------------------------------------------------------------------------
// Version, origin and status spellings
// ---------------------------------------------------------------------------

JsonValue semantic_version_to_json(const SemanticVersion &version) {
    JsonValue::Object object;
    object.emplace_back("major", static_cast<std::int64_t>(version.major));
    object.emplace_back("minor", static_cast<std::int64_t>(version.minor));
    object.emplace_back("patch", static_cast<std::int64_t>(version.patch));
    return JsonValue{std::move(object)};
}

Result<SemanticVersion> semantic_version_from_json(const JsonValue &value) {
    if (!value.is_object()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "semantic version must be an object");
    }
    SemanticVersion version{};
    for (const auto &[field, target] : {std::pair{"major", &version.major},
                                        std::pair{"minor", &version.minor},
                                        std::pair{"patch", &version.patch}}) {
        const auto *member = value.find(field);
        if (member == nullptr || !member->is_integer()) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string("semantic version requires an integer ") +
                                         field);
        }
        const auto number = member->as_integer();
        if (!number.has_value() || *number < 0 || *number > 65535) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string("semantic version ") + field +
                                         " is out of range");
        }
        *target = static_cast<std::uint16_t>(*number);
    }
    return version;
}

std::string_view tool_module_origin_name(ToolModuleOrigin origin) {
    switch (origin) {
    case ToolModuleOrigin::BuiltIn:
        return "built_in";
    case ToolModuleOrigin::HostProvided:
        return "host_provided";
    case ToolModuleOrigin::OutOfProcess:
        return "out_of_process";
    }
    return "built_in";
}

std::optional<ToolModuleOrigin> tool_module_origin_from_name(std::string_view name) {
    if (name == "built_in") {
        return ToolModuleOrigin::BuiltIn;
    }
    if (name == "host_provided") {
        return ToolModuleOrigin::HostProvided;
    }
    if (name == "out_of_process") {
        return ToolModuleOrigin::OutOfProcess;
    }
    return std::nullopt;
}

std::string_view module_status_name(ModuleStatus status) {
    switch (status) {
    case ModuleStatus::Available:
        return "available";
    case ModuleStatus::Unavailable:
        return "unavailable";
    case ModuleStatus::Conflict:
        return "conflict";
    case ModuleStatus::Revoked:
        return "revoked";
    }
    return "unavailable";
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------

Result<ToolModuleManifest>
parse_tool_module_manifest(const JsonValue &json, const CapabilityCatalog &catalog,
                           const ToolModuleLimits &limits) {
    if (!json.is_object()) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest must be a JSON object");
    }
    const auto canonical_size = canonical_json_string(json).size();
    if (canonical_size > limits.max_manifest_bytes) {
        return make_module_error(ErrorCode::ResourceExhausted,
                                 "manifest exceeds the canonical size limit");
    }

    const auto *schema = json.find("schema");
    if (schema == nullptr || !schema->is_string() ||
        schema->as_string()->empty() ||
        *schema->as_string() != kToolModuleManifestSchema) {
        return make_module_error(ErrorCode::UnsupportedVersion, "manifest schema is not '" +
                                                                    std::string(
                                                                        kToolModuleManifestSchema) +
                                                                    "'");
    }
    const auto *schema_version = json.find("schema_version");
    if (schema_version == nullptr || !schema_version->is_string() ||
        *schema_version->as_string() != kToolModuleManifestSchemaVersion) {
        return make_module_error(
            ErrorCode::UnsupportedVersion,
            "manifest schema_version must be '" + std::string(kToolModuleManifestSchemaVersion) +
                "'");
    }

    ToolModuleManifest manifest;
    const auto *module_id = json.find("module_id");
    if (module_id == nullptr || !module_id->is_string()) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest requires module_id");
    }
    manifest.module_id = *module_id->as_string();
    if (!is_valid_vocabulary_id(manifest.module_id) ||
        manifest.module_id.size() > limits.max_module_id_bytes) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 id_charset_message("module_id", manifest.module_id));
    }

    const auto *version = json.find("version");
    if (version == nullptr) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest requires a version");
    }
    auto parsed_version = semantic_version_from_json(*version);
    if (!parsed_version.has_value()) {
        return parsed_version.error();
    }
    manifest.version = parsed_version.value();

    const auto *origin = json.find("origin");
    if (origin == nullptr || !origin->is_object()) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest requires an origin object");
    }
    const auto *isolation = origin->find("isolation");
    if (isolation == nullptr || !isolation->is_string()) {
        return make_module_error(ErrorCode::InvalidArgument, "origin requires an isolation");
    }
    const auto parsed_origin = tool_module_origin_from_name(*isolation->as_string());
    if (!parsed_origin.has_value()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "origin has an unknown isolation '" +
                                     *isolation->as_string() + "'");
    }
    manifest.origin = *parsed_origin;
    const auto read_bounded_string = [origin](std::string_view field, std::string &target,
                                              std::size_t max_bytes) -> Result<void> {
        const auto *member = origin->find(field);
        if (member == nullptr) {
            return Result<void>{};
        }
        const auto *text = member->as_string();
        if (text == nullptr || text->size() > max_bytes) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "origin " + std::string(field) +
                                         " must be a string within the limit");
        }
        target = *text;
        return Result<void>{};
    };
    auto signer = read_bounded_string("signer", manifest.signer, limits.max_signer_bytes);
    if (!signer.has_value()) {
        return signer.error();
    }
    auto algorithm = read_bounded_string("signature_algorithm", manifest.signature_algorithm,
                                         limits.max_signature_bytes);
    if (!algorithm.has_value()) {
        return algorithm.error();
    }
    auto signature =
        read_bounded_string("signature", manifest.signature, limits.max_signature_bytes);
    if (!signature.has_value()) {
        return signature.error();
    }
    // Trust fields (tool module design §12): OutOfProcess packages must carry
    // signature material; HostProvided modules must name their allowlist
    // identity. Cryptographic verification itself is TM1.
    if (manifest.origin == ToolModuleOrigin::OutOfProcess &&
        (manifest.signer.empty() || manifest.signature_algorithm.empty() ||
         manifest.signature.empty())) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "out_of_process manifests require signer, signature_algorithm "
                                 "and signature");
    }
    if (manifest.origin == ToolModuleOrigin::HostProvided && manifest.signer.empty()) {
        return make_module_error(ErrorCode::InvalidArgument,
                                 "host_provided manifests require a signer identity");
    }

    const auto *abi = json.find("min_mira_module_abi");
    if (abi != nullptr) {
        if (!abi->is_integer()) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "min_mira_module_abi must be an integer");
        }
        const auto value = abi->as_integer();
        if (!value.has_value() || *value < 1) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "min_mira_module_abi must be positive");
        }
        if (*value > kCurrentToolModuleAbi) {
            return make_module_error(ErrorCode::UnsupportedVersion,
                                     "manifest requires module ABI " + std::to_string(*value) +
                                         " above the runtime ABI " +
                                         std::to_string(kCurrentToolModuleAbi));
        }
        manifest.min_mira_module_abi = static_cast<std::uint32_t>(*value);
    }

    auto required =
        parse_capability_list(json.find("required_capabilities"), "required_capabilities", catalog,
                              limits);
    if (!required.has_value()) {
        return required.error();
    }
    manifest.required_capabilities = std::move(required.value());
    auto infra = parse_capability_list(json.find("infra_capabilities"), "infra_capabilities",
                                       catalog, limits);
    if (!infra.has_value()) {
        return infra.error();
    }
    manifest.infra_capabilities = std::move(infra.value());

    const auto *tools = json.find("tools");
    if (tools == nullptr || !tools->is_array()) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest requires a tools array");
    }
    const auto &tool_array = *tools->as_array();
    if (tool_array.empty() || tool_array.size() > limits.max_tools) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest must declare 1.." +
                                                                 std::to_string(limits.max_tools) +
                                                                 " member tools");
    }
    std::set<std::string_view> member_names;
    for (const auto &entry : tool_array) {
        auto tool = parse_tool_spec(entry, catalog, limits);
        if (!tool.has_value()) {
            return tool.error();
        }
        if (!member_names.insert(tool.value().name).second) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     std::string("manifest declares duplicate member tool "
                                                 "name '") +
                                         tool.value().name + "'");
        }
        manifest.tools.push_back(std::move(tool.value()));
    }

    auto conflicts = parse_label_list(json.find("conflicts_with"), "conflicts_with", limits,
                                      /*require_module_id_charset=*/true);
    if (!conflicts.has_value()) {
        return conflicts.error();
    }
    for (const auto &conflict : conflicts.value()) {
        if (conflict == manifest.module_id) {
            return make_module_error(ErrorCode::InvalidArgument,
                                     "manifest cannot conflict with itself");
        }
        if (manifest.conflicts_with.size() >= limits.max_conflicts) {
            return make_module_error(ErrorCode::ResourceExhausted,
                                     "conflicts_with exceeds the limit");
        }
        manifest.conflicts_with.push_back(conflict);
    }

    const auto *resources = json.find("resources");
    if (resources == nullptr || !resources->is_object()) {
        return make_module_error(ErrorCode::InvalidArgument, "manifest requires resources");
    }
    auto concurrent = parse_bounded_uint32(resources->find("max_total_concurrent_invocations"),
                                           "resources.max_total_concurrent_invocations",
                                           limits.max_concurrent_invocations);
    if (!concurrent.has_value()) {
        return concurrent.error();
    }
    manifest.resources.max_total_concurrent_invocations = concurrent.value();
    auto result_bytes = parse_bounded_uint64(resources->find("max_total_result_bytes"),
                                             "resources.max_total_result_bytes",
                                             limits.max_result_bytes);
    if (!result_bytes.has_value()) {
        return result_bytes.error();
    }
    manifest.resources.max_total_result_bytes = result_bytes.value();

    return manifest;
}

Hash tool_module_manifest_digest(const ToolModuleManifest &manifest) {
    return canonical_json_digest(tool_module_manifest_to_json(manifest, /*include_signature=*/true));
}

Hash tool_module_unsigned_manifest_digest(const ToolModuleManifest &manifest) {
    return canonical_json_digest(
        tool_module_manifest_to_json(manifest, /*include_signature=*/false));
}

// ---------------------------------------------------------------------------
// Negotiation
// ---------------------------------------------------------------------------

ModuleSnapshot module_snapshot_from_manifest(const ToolModuleManifest &manifest) {
    ModuleSnapshot snapshot;
    snapshot.module_id = manifest.module_id;
    snapshot.version = manifest.version;
    snapshot.module_digest = tool_module_manifest_digest(manifest);
    snapshot.tools = manifest.tools;
    snapshot.conflicts_with = manifest.conflicts_with;

    std::set<std::string> required(manifest.required_capabilities.begin(),
                                   manifest.required_capabilities.end());
    required.insert(manifest.infra_capabilities.begin(), manifest.infra_capabilities.end());
    for (const auto &tool : manifest.tools) {
        required.insert(tool.required_capabilities.begin(), tool.required_capabilities.end());
    }
    snapshot.required.assign(required.begin(), required.end());
    return snapshot;
}

ModuleNegotiationResult negotiate_modules(std::span<const ModuleSnapshot> active,
                                          const EnvironmentCapabilities &environment,
                                          const CapabilityCatalog &catalog) {
    std::vector<ModuleSnapshot> snapshots(active.begin(), active.end());
    std::sort(snapshots.begin(), snapshots.end(),
              [](const ModuleSnapshot &lhs, const ModuleSnapshot &rhs) {
                  return lhs.module_id < rhs.module_id;
              });

    const std::vector<CapabilityId> provided = derive_environment_capabilities(environment);

    // Collect conflict partners: declared conflicts (either direction) and
    // cross-module wire-namespace collisions on member tool names.
    std::map<std::string, std::set<std::string>> conflict_partners;
    std::map<std::string, std::size_t> index_by_id;
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        index_by_id[snapshots[index].module_id] = index;
    }
    std::map<std::string, std::set<std::string>> name_owners;
    for (const auto &snapshot : snapshots) {
        for (const auto &declared : snapshot.conflicts_with) {
            const auto found = index_by_id.find(declared);
            if (found == index_by_id.end()) {
                continue;
            }
            conflict_partners[snapshot.module_id].insert(declared);
            conflict_partners[declared].insert(snapshot.module_id);
        }
        for (const auto &tool : snapshot.tools) {
            name_owners[tool.name].insert(snapshot.module_id);
        }
    }
    for (const auto &owners_entry : name_owners) {
        const std::set<std::string> &owners = owners_entry.second;
        if (owners.size() < 2) {
            continue;
        }
        for (const auto &owner : owners) {
            conflict_partners[owner].insert(owners.begin(), owners.end());
            conflict_partners[owner].erase(owner);
        }
    }

    ModuleNegotiationResult result;
    result.modules.reserve(snapshots.size());
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const auto &snapshot = snapshots[index];
        ModuleAvailability availability;
        availability.module_id = snapshot.module_id;
        availability.version = snapshot.version;
        availability.module_digest = snapshot.module_digest;

        // Duplicate module ids cannot come from a validated registry; if a
        // caller assembles them anyway, every copy conflicts with its twin
        // instead of silently merging.
        const bool duplicated =
            (index > 0 && snapshots[index - 1].module_id == snapshot.module_id) ||
            (index + 1 < snapshots.size() && snapshots[index + 1].module_id == snapshot.module_id);

        std::vector<CapabilityId> missing;
        if (!duplicated) {
            std::set<std::string> required(snapshot.required.begin(), snapshot.required.end());
            for (const auto &capability : required) {
                // Catalog-unknown requirements are missing by definition even
                // when the environment claims them: vocabulary outside the
                // governed catalog never becomes available. Fail closed
                // without inventing a third outcome.
                if (!catalog.contains(capability) ||
                    !std::binary_search(provided.begin(), provided.end(), capability)) {
                    missing.push_back(capability);
                }
            }
        }

        const auto &partners = conflict_partners[snapshot.module_id];
        if (duplicated) {
            availability.status = ModuleStatus::Conflict;
            availability.conflicting_with = snapshot.module_id;
        } else if (!partners.empty()) {
            availability.status = ModuleStatus::Conflict;
            availability.conflicting_with = *partners.begin();
        } else if (!missing.empty()) {
            availability.status = ModuleStatus::Unavailable;
            availability.missing = std::move(missing);
        } else {
            availability.status = ModuleStatus::Available;
        }
        result.modules.push_back(std::move(availability));
    }

    result.digest = canonical_json_digest(module_negotiation_to_json(result));
    return result;
}

JsonValue module_negotiation_to_json(const ModuleNegotiationResult &result) {
    JsonValue::Array modules;
    modules.reserve(result.modules.size());
    for (const auto &availability : result.modules) {
        JsonValue::Object object;
        object.emplace_back("module_id", availability.module_id);
        object.emplace_back("version", semantic_version_to_json(availability.version));
        object.emplace_back("module_digest", availability.module_digest.to_string());
        object.emplace_back("status", std::string(module_status_name(availability.status)));
        object.emplace_back("missing", id_list_to_json(availability.missing));
        object.emplace_back("conflicting_with", availability.conflicting_with);
        modules.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object root;
    root.emplace_back("modules", JsonValue{std::move(modules)});
    return JsonValue{std::move(root)};
}

} // namespace mira
