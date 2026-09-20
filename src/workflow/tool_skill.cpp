#include <mira/tool_skill.hpp>

#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>
#include <mira/tool_module.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace mira {
namespace {

// Deterministic domain codes for the mira.skill domain: 1 = invalid
// descriptor/surface input, 2 = lifecycle or capacity rejection, 3 = source
// binding/consistency failure. safe_message carries bounded identifiers only.
[[nodiscard]] Error make_skill_error(std::int32_t domain_code, ErrorCode code,
                                     std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.skill";
    error.domain_code = domain_code;
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] Error invalid_skill(std::string message) {
    return make_skill_error(1, ErrorCode::InvalidArgument, std::move(message));
}
[[nodiscard]] Error lifecycle_rejected(std::string message) {
    return make_skill_error(2, ErrorCode::InvalidState, std::move(message));
}
[[nodiscard]] Error binding_rejected(std::string message) {
    return make_skill_error(3, ErrorCode::InvalidArgument, std::move(message));
}

// Governed vocabulary charset shared with the module and reference layers
// (capability ids, module ids, member names, reference wire names).
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

[[nodiscard]] JsonValue digest_to_json(const Hash &digest) { return JsonValue{digest.to_string()}; }

[[nodiscard]] JsonValue version_to_json_value(const SemanticVersion &version) {
    return semantic_version_to_json(version);
}

// ---------------------------------------------------------------------------
// Surface derivation (design §17.1)
// ---------------------------------------------------------------------------

[[nodiscard]] Result<JsonValue> derive_parameter_property(const WorkflowParameterSpec &spec,
                                                          const SkillLimits &limits) {
    if (spec.summary.size() > limits.max_description_bytes) {
        return invalid_skill("parameter '" + spec.name + "' summary exceeds the description bound");
    }
    JsonValue::Object property;
    property.emplace_back("type", std::string(workflow_parameter_type_name(spec.type)));
    if (!spec.summary.empty()) {
        property.emplace_back("description", spec.summary);
    }
    if (spec.minimum.has_value()) {
        property.emplace_back("minimum", JsonValue{*spec.minimum});
    }
    if (spec.maximum.has_value()) {
        property.emplace_back("maximum", JsonValue{*spec.maximum});
    }
    if (spec.min_length.has_value()) {
        property.emplace_back("minLength", JsonValue{static_cast<std::int64_t>(*spec.min_length)});
    }
    if (spec.max_length.has_value()) {
        property.emplace_back("maxLength", JsonValue{static_cast<std::int64_t>(*spec.max_length)});
    }
    if (spec.pattern.has_value()) {
        property.emplace_back("pattern", *spec.pattern);
    }
    if (!spec.enum_values.empty()) {
        JsonValue::Array enum_values;
        enum_values.reserve(spec.enum_values.size());
        for (const auto &value : spec.enum_values) {
            enum_values.emplace_back(value);
        }
        property.emplace_back("enum", JsonValue{std::move(enum_values)});
    }
    return JsonValue{std::move(property)};
}

// Sink failures never propagate to the control plane: they are counted and
// visible in stats (the same isolation the module registry applies).
bool append_skill_event(IEventStore *sink, RuntimeId runtime_id, SessionId session_id,
                        EventClass classification, JsonValue payload) {
    if (sink == nullptr || runtime_id.is_nil() || session_id.is_nil()) {
        return false;
    }
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = runtime_id;
    request.session_id = session_id;
    request.payload.type = std::string(kSkillPublicationEventSchema);
    request.payload.data = to_json_string(payload);
    request.payload.classification = classification;
    return sink->append(request).has_value();
}

[[nodiscard]] std::vector<SkillPublicationRecord>::iterator
find_record(std::vector<SkillPublicationRecord> &records, std::string_view name) {
    return std::find_if(records.begin(), records.end(), [&](const SkillPublicationRecord &record) {
        return record.descriptor.name == name;
    });
}

[[nodiscard]] std::vector<SkillPublicationRecord>::const_iterator
find_record(const std::vector<SkillPublicationRecord> &records, std::string_view name) {
    return std::find_if(records.begin(), records.end(), [&](const SkillPublicationRecord &record) {
        return record.descriptor.name == name;
    });
}

} // namespace

// ---------------------------------------------------------------------------
// Descriptor and surface (design §17.1)
// ---------------------------------------------------------------------------

Hash skill_descriptor_digest(const SkillDescriptor &descriptor) {
    JsonValue::Object surface;
    surface.emplace_back("description", descriptor.surface.description);
    surface.emplace_back("parameters_schema", descriptor.surface.parameters_schema.root);
    surface.emplace_back("has_side_effects", descriptor.surface.has_side_effects);
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kSkillDescriptorSchema));
    root.emplace_back("name", descriptor.name);
    root.emplace_back("version", version_to_json_value(descriptor.version));
    root.emplace_back("source_workflow_id", descriptor.source_workflow_id.to_string());
    root.emplace_back("source_ir_digest", digest_to_json(descriptor.source_ir_digest));
    root.emplace_back("surface", JsonValue{std::move(surface)});
    return canonical_json_digest(JsonValue{std::move(root)});
}

Result<SkillSurface> derive_skill_surface(const WorkflowDefinition &definition,
                                          const WorkflowToolRefManifest &refs,
                                          std::span<const ExposedToolSpec> view,
                                          const SkillLimits &limits) {
    if (!validate_workflow_definition(definition).has_value()) {
        return binding_rejected("definition failed structural validation");
    }
    auto binding = verify_workflow_tool_refs(refs, definition.workflow_id,
                                             workflow_definition_digest(definition));
    if (!binding.has_value()) {
        return binding.error();
    }
    if (definition.summary.empty() || definition.summary.size() > limits.max_description_bytes) {
        return invalid_skill("definition summary is empty or exceeds the description bound");
    }
    if (definition.parameters.size() > limits.max_parameters) {
        return invalid_skill("definition exceeds the skill parameter budget");
    }
    std::map<std::string, const ExposedToolSpec *> index;
    for (const auto &tool : view) {
        const auto [position, inserted] = index.emplace(tool.wire_name, &tool);
        if (!inserted) {
            return binding_rejected("exposure view contains duplicate wire name '" +
                                    tool.wire_name + "'");
        }
    }

    SkillSurface surface;
    surface.description = definition.summary;
    JsonValue::Object properties;
    JsonValue::Array required;
    for (const auto &spec : definition.parameters) {
        auto property = derive_parameter_property(spec, limits);
        if (!property.has_value()) {
            return property.error();
        }
        properties.emplace_back(spec.name, std::move(property.value()));
        if (spec.required) {
            required.emplace_back(spec.name);
        }
    }
    JsonValue::Object schema;
    schema.emplace_back("type", JsonValue{std::string("object")});
    schema.emplace_back("properties", JsonValue{std::move(properties)});
    if (!required.empty()) {
        schema.emplace_back("required", JsonValue{std::move(required)});
    }
    schema.emplace_back("additionalProperties", JsonValue{false});
    surface.parameters_schema = JsonSchema{JsonValue{std::move(schema)}};
    if (!gate_schema_subset(surface.parameters_schema).has_value()) {
        return invalid_skill("derived parameters schema is outside the supported subset");
    }

    for (const auto &entry : refs.entries) {
        const auto found = index.find(entry.wire_name);
        if (found == index.end()) {
            return binding_rejected("referenced tool '" + entry.wire_name +
                                    "' is absent from the exposure view");
        }
        if (found->second->has_side_effects) {
            surface.has_side_effects = true;
        }
    }
    return surface;
}

Result<SkillDescriptor> make_skill_descriptor(const WorkflowDefinition &definition,
                                              const WorkflowToolRefManifest &refs,
                                              std::span<const ExposedToolSpec> view,
                                              std::string_view name, SemanticVersion version,
                                              const SkillLimits &limits) {
    if (name.empty() || name.size() > limits.max_name_bytes || !is_valid_vocabulary_id(name)) {
        return invalid_skill("skill name is empty, over the length limit or outside the "
                             "vocabulary charset");
    }
    if (is_known_hosted_tool_name(name)) {
        return invalid_skill("skill name collides with a hosted provider tool name");
    }
    auto surface = derive_skill_surface(definition, refs, view, limits);
    if (!surface.has_value()) {
        return surface.error();
    }
    SkillDescriptor descriptor;
    descriptor.name = std::string(name);
    descriptor.version = version;
    descriptor.source_workflow_id = definition.workflow_id;
    descriptor.source_ir_digest = workflow_definition_digest(definition);
    descriptor.surface = std::move(surface.value());
    descriptor.digest = skill_descriptor_digest(descriptor);
    return descriptor;
}

JsonValue skill_descriptor_to_json(const SkillDescriptor &descriptor) {
    JsonValue::Object surface;
    surface.emplace_back("description", descriptor.surface.description);
    surface.emplace_back("parameters_schema", descriptor.surface.parameters_schema.root);
    surface.emplace_back("has_side_effects", descriptor.surface.has_side_effects);
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kSkillDescriptorSchema));
    root.emplace_back("name", descriptor.name);
    root.emplace_back("version", version_to_json_value(descriptor.version));
    root.emplace_back("source_workflow_id", descriptor.source_workflow_id.to_string());
    root.emplace_back("source_ir_digest", digest_to_json(descriptor.source_ir_digest));
    root.emplace_back("surface", JsonValue{std::move(surface)});
    root.emplace_back("digest", digest_to_json(descriptor.digest));
    return JsonValue{std::move(root)};
}

Result<SkillDescriptor> skill_descriptor_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return invalid_skill("skill descriptor must be an object");
    }
    for (const auto &member : *json.as_object()) {
        const bool known = member.first == "schema" || member.first == "name" ||
                           member.first == "version" || member.first == "source_workflow_id" ||
                           member.first == "source_ir_digest" || member.first == "surface" ||
                           member.first == "digest";
        if (!known) {
            return invalid_skill("unknown skill descriptor field '" + member.first + "'");
        }
    }
    const auto *schema = json.find("schema");
    if (schema == nullptr || !schema->is_string() ||
        *schema->as_string() != kSkillDescriptorSchema) {
        return invalid_skill("skill descriptor requires schema '" +
                             std::string(kSkillDescriptorSchema) + "'");
    }
    const auto *name = json.find("name");
    if (name == nullptr || !name->is_string() || name->as_string()->empty()) {
        return invalid_skill("skill descriptor requires a name");
    }
    const auto *version = json.find("version");
    if (version == nullptr) {
        return invalid_skill("skill descriptor requires a version");
    }
    auto parsed_version = semantic_version_from_json(*version);
    if (!parsed_version.has_value()) {
        return invalid_skill("skill descriptor version is not a semantic version");
    }
    const auto *workflow_id = json.find("source_workflow_id");
    if (workflow_id == nullptr || !workflow_id->is_string()) {
        return invalid_skill("skill descriptor requires a source workflow id");
    }
    const auto parsed_id = WorkflowId::parse(*workflow_id->as_string());
    if (!parsed_id.has_value() || parsed_id->value.is_nil()) {
        return invalid_skill("skill descriptor source workflow id is not a valid id");
    }
    const auto *ir_digest = json.find("source_ir_digest");
    if (ir_digest == nullptr || !ir_digest->is_string()) {
        return invalid_skill("skill descriptor requires a source ir digest");
    }
    const auto parsed_digest = digest_from_hex(*ir_digest->as_string());
    if (!parsed_digest.has_value()) {
        return invalid_skill("skill descriptor source ir digest is not a digest");
    }
    const auto *surface = json.find("surface");
    if (surface == nullptr || !surface->is_object()) {
        return invalid_skill("skill descriptor requires a surface object");
    }
    for (const auto &member : *surface->as_object()) {
        const bool known = member.first == "description" || member.first == "parameters_schema" ||
                           member.first == "has_side_effects";
        if (!known) {
            return invalid_skill("unknown skill surface field '" + member.first + "'");
        }
    }
    const auto *description = surface->find("description");
    const auto *parameters = surface->find("parameters_schema");
    const auto *side_effects = surface->find("has_side_effects");
    if (description == nullptr || !description->is_string() || parameters == nullptr ||
        !parameters->is_object() || side_effects == nullptr || !side_effects->is_boolean()) {
        return invalid_skill("skill surface requires description, parameters_schema and "
                             "has_side_effects");
    }
    SkillDescriptor descriptor;
    descriptor.name = *name->as_string();
    descriptor.version = parsed_version.value();
    descriptor.source_workflow_id = *parsed_id;
    descriptor.source_ir_digest = *parsed_digest;
    descriptor.surface.description = *description->as_string();
    descriptor.surface.parameters_schema = JsonSchema{*parameters};
    descriptor.surface.has_side_effects = *side_effects->as_boolean();
    descriptor.digest = skill_descriptor_digest(descriptor);
    const auto *digest = json.find("digest");
    if (digest == nullptr || !digest->is_string()) {
        return invalid_skill("skill descriptor requires a digest");
    }
    const auto parsed_descriptor_digest = digest_from_hex(*digest->as_string());
    if (!parsed_descriptor_digest.has_value() || *parsed_descriptor_digest != descriptor.digest) {
        return invalid_skill("skill descriptor digest does not match its content");
    }
    return descriptor;
}

// ---------------------------------------------------------------------------
// Publication lifecycle (design §17.2)
// ---------------------------------------------------------------------------

std::string_view skill_publication_status_name(SkillPublicationStatus status) {
    switch (status) {
    case SkillPublicationStatus::Published:
        return "published";
    case SkillPublicationStatus::Revoked:
        return "revoked";
    }
    return "revoked";
}

JsonValue skill_publication_event_to_json(const SkillPublicationEvent &event) {
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kSkillPublicationEventSchema));
    root.emplace_back("kind", event.kind);
    root.emplace_back("name", event.name);
    root.emplace_back("version", version_to_json_value(event.version));
    root.emplace_back("source_workflow_id", event.source_workflow_id.to_string());
    root.emplace_back("source_ir_digest", digest_to_json(event.source_ir_digest));
    root.emplace_back("descriptor_digest", digest_to_json(event.descriptor_digest));
    if (!event.reason.empty()) {
        root.emplace_back("reason", event.reason);
    }
    return JsonValue{std::move(root)};
}

SkillPublicationRegistry::SkillPublicationRegistry(SkillLimits limits, IEventStore *event_sink,
                                                   RuntimeId runtime_id, SessionId session_id)
    : limits_(limits), event_sink_(event_sink), runtime_id_(runtime_id), session_id_(session_id) {}

void SkillPublicationRegistry::emit(const SkillPublicationEvent &event, EventClass classification) {
    const bool ok = append_skill_event(event_sink_, runtime_id_, session_id_, classification,
                                       skill_publication_event_to_json(event));
    if (ok) {
        ++stats_.events_emitted;
    } else {
        ++stats_.event_sink_failures;
    }
}

Result<SkillDescriptor> SkillPublicationRegistry::check_publish_inputs(
    const SkillDescriptor &descriptor, const WorkflowDefinition &source,
    const WorkflowToolRefManifest &refs, std::span<const ExposedToolSpec> view,
    const WorkflowVersionHistory &history) {
    if (descriptor.name.empty() || descriptor.name.size() > limits_.max_name_bytes ||
        !is_valid_vocabulary_id(descriptor.name)) {
        return invalid_skill("skill name is empty, over the length limit or outside the "
                             "vocabulary charset");
    }
    if (is_known_hosted_tool_name(descriptor.name)) {
        return invalid_skill("skill name collides with a hosted provider tool name");
    }
    if (descriptor.digest != skill_descriptor_digest(descriptor)) {
        return invalid_skill("skill descriptor digest does not match its content");
    }
    if (!validate_workflow_definition(source).has_value()) {
        return binding_rejected("source definition failed structural validation");
    }
    if (!(history.workflow_id == descriptor.source_workflow_id) ||
        !(source.workflow_id == descriptor.source_workflow_id)) {
        return binding_rejected("source workflow id does not match the descriptor");
    }
    const Sha256Digest definition_digest = workflow_definition_digest(source);
    if (!(descriptor.source_ir_digest == definition_digest)) {
        return binding_rejected("pinned ir_digest does not match the source definition content");
    }
    auto record = resolve_workflow_version(history, descriptor.source_ir_digest);
    if (!record.has_value()) {
        return record.error();
    }
    if (!workflow_version_is_runnable(record.value())) {
        return binding_rejected("source version has not passed the publish gate "
                                "(DryRunPassed/Validated required)");
    }
    auto binding = verify_workflow_tool_refs(refs, source.workflow_id, definition_digest);
    if (!binding.has_value()) {
        return binding.error();
    }
    auto surface = derive_skill_surface(source, refs, view, limits_);
    if (!surface.has_value()) {
        return surface.error();
    }
    if (!(descriptor.surface == surface.value())) {
        return invalid_skill("descriptor surface does not match the derived surface");
    }
    return descriptor;
}

Result<SkillPublicationRecord> SkillPublicationRegistry::publish_skill(
    const SkillDescriptor &descriptor, const WorkflowDefinition &source,
    const WorkflowToolRefManifest &refs, std::span<const ExposedToolSpec> view,
    const WorkflowVersionHistory &history) {
    if (closed_) {
        ++stats_.rejected_closed;
        return lifecycle_rejected("registry is closed");
    }
    if (sealed_) {
        ++stats_.rejected_sealed;
        return lifecycle_rejected("publish window is sealed");
    }
    auto checked = check_publish_inputs(descriptor, source, refs, view, history);
    if (!checked.has_value()) {
        ++stats_.rejected_other;
        return checked.error();
    }
    auto existing = find_record(records_, descriptor.name);
    if (existing != records_.end()) {
        // Idempotency is keyed on name + descriptor (the frozen contract),
        // not on the whole record: republishing the current descriptor after
        // an upgrade is still a NoOp even though the superseded trail is
        // non-empty.
        if (existing->status == SkillPublicationStatus::Published &&
            existing->descriptor == descriptor) {
            ++stats_.idempotent_noops;
            return *existing;
        }
        ++stats_.rejected_other;
        if (existing->status == SkillPublicationStatus::Revoked) {
            return lifecycle_rejected("skill '" + descriptor.name +
                                      "' is revoked; republication requires a new "
                                      "registration window");
        }
        return make_skill_error(2, ErrorCode::AlreadyExists,
                                "skill name '" + descriptor.name + "' is already published");
    }
    if (records_.size() >= limits_.max_skills) {
        ++stats_.rejected_other;
        return make_skill_error(2, ErrorCode::ResourceExhausted,
                                "registry exceeds the skill capacity");
    }
    SkillPublicationRecord record;
    record.descriptor = descriptor;
    record.status = SkillPublicationStatus::Published;
    records_.insert(std::upper_bound(records_.begin(), records_.end(), descriptor.name,
                                     [](const std::string &name, const SkillPublicationRecord &r) {
                                         return name < r.descriptor.name;
                                     }),
                    record);
    ++stats_.published;
    emit(SkillPublicationEvent{"published",
                               descriptor.name,
                               descriptor.version,
                               descriptor.source_workflow_id,
                               descriptor.source_ir_digest,
                               descriptor.digest,
                               {}},
         EventClass::State);
    return record;
}

Result<SkillPublicationRecord> SkillPublicationRegistry::upgrade_skill(
    std::string_view name, const SkillDescriptor &next, const WorkflowDefinition &source,
    const WorkflowToolRefManifest &refs, std::span<const ExposedToolSpec> view,
    const WorkflowVersionHistory &history) {
    if (closed_) {
        ++stats_.rejected_closed;
        return lifecycle_rejected("registry is closed");
    }
    if (sealed_) {
        ++stats_.rejected_sealed;
        return lifecycle_rejected("publish window is sealed");
    }
    auto position = find_record(records_, name);
    if (position == records_.end()) {
        ++stats_.rejected_other;
        return make_skill_error(2, ErrorCode::NotFound,
                                "skill '" + std::string(name) + "' is not published");
    }
    if (position->status == SkillPublicationStatus::Revoked) {
        ++stats_.rejected_other;
        return lifecycle_rejected("skill '" + std::string(name) +
                                  "' is revoked; upgrades require a new registration window");
    }
    if (next.name != name) {
        ++stats_.rejected_other;
        return invalid_skill("upgrade descriptor name does not match the published skill");
    }
    if (next.source_ir_digest == position->descriptor.source_ir_digest) {
        ++stats_.idempotent_noops;
        return *position;
    }
    auto checked = check_publish_inputs(next, source, refs, view, history);
    if (!checked.has_value()) {
        ++stats_.rejected_other;
        return checked.error();
    }
    if (!(next.version > position->descriptor.version)) {
        ++stats_.rejected_other;
        return lifecycle_rejected("upgrade version must be strictly greater than the "
                                  "published version");
    }
    if (position->superseded_versions.size() >= limits_.max_superseded_versions) {
        ++stats_.rejected_other;
        return lifecycle_rejected("superseded version trail exceeds the limit");
    }
    SkillPublicationRecord updated = *position;
    updated.superseded_versions.push_back(position->descriptor.version);
    updated.descriptor = checked.value();
    *position = updated;
    ++stats_.upgrades;
    emit(SkillPublicationEvent{"upgraded",
                               next.name,
                               next.version,
                               next.source_workflow_id,
                               next.source_ir_digest,
                               next.digest,
                               {}},
         EventClass::State);
    return updated;
}

Result<SkillPublicationRecord> SkillPublicationRegistry::revoke_skill(std::string_view name,
                                                                      std::string_view reason) {
    if (closed_) {
        ++stats_.rejected_closed;
        return lifecycle_rejected("registry is closed");
    }
    auto position = find_record(records_, name);
    if (position == records_.end()) {
        ++stats_.rejected_other;
        return make_skill_error(2, ErrorCode::NotFound,
                                "skill '" + std::string(name) + "' is not published");
    }
    if (position->status == SkillPublicationStatus::Revoked) {
        ++stats_.idempotent_noops;
        return *position;
    }
    if (reason.empty() || reason.size() > limits_.max_reason_bytes) {
        ++stats_.rejected_other;
        return invalid_skill("revoke reason is empty or exceeds the bound");
    }
    position->status = SkillPublicationStatus::Revoked;
    ++stats_.revocations;
    emit(SkillPublicationEvent{"revoked", position->descriptor.name, position->descriptor.version,
                               position->descriptor.source_workflow_id,
                               position->descriptor.source_ir_digest, position->descriptor.digest,
                               std::string(reason)},
         EventClass::Critical);
    return *position;
}

void SkillPublicationRegistry::seal() { sealed_ = true; }
void SkillPublicationRegistry::close() {
    sealed_ = true;
    closed_ = true;
}

std::optional<SkillPublicationRecord> SkillPublicationRegistry::find(std::string_view name) const {
    auto position = find_record(records_, name);
    if (position == records_.end()) {
        return std::nullopt;
    }
    return *position;
}

std::vector<SkillPublicationRecord> SkillPublicationRegistry::publications() const {
    return records_;
}

// ---------------------------------------------------------------------------
// Procedure index projection (design §17.3)
// ---------------------------------------------------------------------------

namespace {

JsonValue procedure_statement_to_json(const SkillPublicationRecord &record) {
    JsonValue::Object root;
    root.emplace_back("schema", std::string(kSkillProcedureIndexSchema));
    root.emplace_back("name", record.descriptor.name);
    root.emplace_back("version", version_to_json_value(record.descriptor.version));
    root.emplace_back("source_workflow_id", record.descriptor.source_workflow_id.to_string());
    root.emplace_back("source_ir_digest", digest_to_json(record.descriptor.source_ir_digest));
    root.emplace_back("descriptor_digest", digest_to_json(record.descriptor.digest));
    root.emplace_back("has_side_effects", record.descriptor.surface.has_side_effects);
    root.emplace_back("status", std::string(skill_publication_status_name(record.status)));
    return JsonValue{std::move(root)};
}

} // namespace

Result<SkillProcedureIndex>
project_skill_procedure_index(std::span<const SkillPublicationRecord> publications,
                              const SkillLimits &limits) {
    if (publications.size() > limits.max_skills) {
        return make_skill_error(2, ErrorCode::ResourceExhausted,
                                "publications exceed the skill capacity");
    }
    SkillProcedureIndex index;
    index.entries.reserve(publications.size());
    for (const auto &record : publications) {
        SkillProcedureEntry entry;
        entry.name = record.descriptor.name;
        entry.version = record.descriptor.version;
        entry.source_workflow_id = record.descriptor.source_workflow_id;
        entry.source_ir_digest = record.descriptor.source_ir_digest;
        entry.descriptor_digest = record.descriptor.digest;
        entry.has_side_effects = record.descriptor.surface.has_side_effects;
        entry.status = record.status;
        entry.statement = canonical_json_string(procedure_statement_to_json(record));
        index.entries.push_back(std::move(entry));
    }
    std::sort(index.entries.begin(), index.entries.end(),
              [](const SkillProcedureEntry &left, const SkillProcedureEntry &right) {
                  return left.name < right.name;
              });
    JsonValue::Array statements;
    statements.reserve(index.entries.size());
    for (const auto &entry : index.entries) {
        auto parsed = parse_json(entry.statement);
        if (!parsed.has_value()) {
            return invalid_skill("procedure statement does not parse");
        }
        statements.emplace_back(std::move(parsed.value()));
    }
    index.digest = canonical_json_digest(JsonValue{std::move(statements)});
    return index;
}

Result<SkillProcedureEntry> skill_procedure_entry_from_statement(std::string_view statement) {
    auto parsed = parse_json(statement);
    if (!parsed.has_value() || !parsed.value().is_object()) {
        return invalid_skill("procedure statement does not parse into an object");
    }
    for (const auto &member : *parsed.value().as_object()) {
        const bool known = member.first == "schema" || member.first == "name" ||
                           member.first == "version" || member.first == "source_workflow_id" ||
                           member.first == "source_ir_digest" ||
                           member.first == "descriptor_digest" ||
                           member.first == "has_side_effects" || member.first == "status";
        if (!known) {
            return invalid_skill("unknown procedure statement field '" + member.first + "'");
        }
    }
    const auto *schema = parsed.value().find("schema");
    if (schema == nullptr || !schema->is_string() ||
        *schema->as_string() != kSkillProcedureIndexSchema) {
        return invalid_skill("procedure statement requires schema '" +
                             std::string(kSkillProcedureIndexSchema) + "'");
    }
    const auto *name = parsed.value().find("name");
    if (name == nullptr || !name->is_string() || name->as_string()->empty()) {
        return invalid_skill("procedure statement requires a name");
    }
    const auto *version = parsed.value().find("version");
    if (version == nullptr) {
        return invalid_skill("procedure statement requires a version");
    }
    auto parsed_version = semantic_version_from_json(*version);
    if (!parsed_version.has_value()) {
        return invalid_skill("procedure statement version is not a semantic version");
    }
    const auto *workflow_id = parsed.value().find("source_workflow_id");
    if (workflow_id == nullptr || !workflow_id->is_string()) {
        return invalid_skill("procedure statement requires a source workflow id");
    }
    const auto parsed_id = WorkflowId::parse(*workflow_id->as_string());
    if (!parsed_id.has_value() || parsed_id->value.is_nil()) {
        return invalid_skill("procedure statement source workflow id is not a valid id");
    }
    const auto *ir_digest = parsed.value().find("source_ir_digest");
    const auto *descriptor_digest = parsed.value().find("descriptor_digest");
    if (ir_digest == nullptr || !ir_digest->is_string() || descriptor_digest == nullptr ||
        !descriptor_digest->is_string()) {
        return invalid_skill("procedure statement requires digests");
    }
    const auto parsed_ir = digest_from_hex(*ir_digest->as_string());
    const auto parsed_descriptor = digest_from_hex(*descriptor_digest->as_string());
    if (!parsed_ir.has_value() || !parsed_descriptor.has_value()) {
        return invalid_skill("procedure statement digests are not digests");
    }
    const auto *side_effects = parsed.value().find("has_side_effects");
    const auto *status = parsed.value().find("status");
    if (side_effects == nullptr || !side_effects->is_boolean() || status == nullptr ||
        !status->is_string()) {
        return invalid_skill("procedure statement requires side effects and status");
    }
    auto parsed_status = SkillPublicationStatus::Revoked;
    if (*status->as_string() == skill_publication_status_name(SkillPublicationStatus::Published)) {
        parsed_status = SkillPublicationStatus::Published;
    } else if (*status->as_string() !=
               skill_publication_status_name(SkillPublicationStatus::Revoked)) {
        return invalid_skill("procedure statement status is outside the closed set");
    }
    SkillProcedureEntry entry;
    entry.name = *name->as_string();
    entry.version = parsed_version.value();
    entry.source_workflow_id = *parsed_id;
    entry.source_ir_digest = *parsed_ir;
    entry.descriptor_digest = *parsed_descriptor;
    entry.has_side_effects = *side_effects->as_boolean();
    entry.status = parsed_status;
    // Rebuildability requires the canonical form: a non-canonical statement
    // is rejected rather than silently normalized.
    const std::string canonical = canonical_json_string(parsed.value());
    if (canonical != statement) {
        return invalid_skill("procedure statement is not in canonical form");
    }
    entry.statement = canonical;
    return entry;
}

} // namespace mira
