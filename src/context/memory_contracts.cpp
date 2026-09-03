#include <mira/memory_contracts.hpp>

#include <cmath>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error memory_error(MemoryDomainCode code, std::string message) {
    return make_memory_error(code, std::move(message));
}

[[nodiscard]] std::string sensitivity_name(Sensitivity sensitivity) {
    switch (sensitivity) {
    case Sensitivity::Public:
        return "public";
    case Sensitivity::Internal:
        return "internal";
    case Sensitivity::Sensitive:
        return "sensitive";
    case Sensitivity::Secret:
        return "secret";
    }
    return "unknown";
}

[[nodiscard]] std::optional<Sensitivity> sensitivity_from(const JsonValue &json) {
    const auto *text = json.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    if (*text == "public") {
        return Sensitivity::Public;
    }
    if (*text == "internal") {
        return Sensitivity::Internal;
    }
    if (*text == "sensitive") {
        return Sensitivity::Sensitive;
    }
    if (*text == "secret") {
        return Sensitivity::Secret;
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t wall_nanos(std::chrono::system_clock::time_point stamp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point wall_from_nanos(std::int64_t nanos) {
    return std::chrono::system_clock::time_point{} + std::chrono::nanoseconds(nanos);
}

[[nodiscard]] JsonValue artifact_ref_to_json(const ArtifactRef &reference) {
    JsonValue::Object object;
    object.emplace_back("id", reference.id.to_string());
    object.emplace_back("digest", reference.digest.to_string());
    object.emplace_back("byte_size", static_cast<std::int64_t>(reference.byte_size));
    object.emplace_back("media_type", reference.media_type);
    object.emplace_back("sensitivity", sensitivity_name(reference.sensitivity));
    return JsonValue(std::move(object));
}

[[nodiscard]] Result<ArtifactRef> artifact_ref_from(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "artifact reference must be an object");
    }
    ArtifactRef reference;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "artifact reference requires an id string");
    }
    const auto parsed_id = ArtifactId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidRecord, "artifact reference id is malformed");
    }
    reference.id = *parsed_id;
    const auto *digest = json.find("digest");
    if (digest != nullptr && digest->is_string()) {
        auto parsed = digest_from_hex(*digest->as_string());
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "artifact reference digest is malformed");
        }
        reference.digest = *parsed;
    }
    const auto *byte_size = json.find("byte_size");
    if (byte_size != nullptr && byte_size->is_integer()) {
        reference.byte_size = static_cast<std::uint64_t>(byte_size->as_integer().value());
    }
    const auto *media_type = json.find("media_type");
    if (media_type != nullptr && media_type->is_string()) {
        reference.media_type = *media_type->as_string();
    }
    const auto *sensitivity = json.find("sensitivity");
    if (sensitivity != nullptr && sensitivity->is_string()) {
        const auto parsed = sensitivity_from(*sensitivity);
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "artifact reference has unknown sensitivity");
        }
        reference.sensitivity = *parsed;
    }
    return reference;
}

template <typename Enum>
[[nodiscard]] std::optional<Enum> enum_from_json(const JsonValue &json, const char *key,
                                                 std::optional<Enum> (*from)(std::string_view)) {
    const auto *text = json.find(key);
    if (text == nullptr || !text->is_string()) {
        return std::nullopt;
    }
    return from(*text->as_string());
}

} // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string memory_scope_kind_name(MemoryScopeKind kind) {
    switch (kind) {
    case MemoryScopeKind::Task:
        return "task";
    case MemoryScopeKind::Session:
        return "session";
    case MemoryScopeKind::Environment:
        return "environment";
    case MemoryScopeKind::Application:
        return "application";
    case MemoryScopeKind::User:
        return "user";
    case MemoryScopeKind::Agent:
        return "agent";
    case MemoryScopeKind::TaskSkill:
        return "task-skill";
    }
    return "unknown";
}

std::optional<MemoryScopeKind> memory_scope_kind_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryScopeKind> kKinds[] = {
        {"task", MemoryScopeKind::Task},
        {"session", MemoryScopeKind::Session},
        {"environment", MemoryScopeKind::Environment},
        {"application", MemoryScopeKind::Application},
        {"user", MemoryScopeKind::User},
        {"agent", MemoryScopeKind::Agent},
        {"task-skill", MemoryScopeKind::TaskSkill},
    };
    for (const auto &[kind_name, kind] : kKinds) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string memory_kind_name(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Preference:
        return "preference";
    case MemoryKind::EnvironmentFact:
        return "environment-fact";
    case MemoryKind::ApplicationFact:
        return "application-fact";
    case MemoryKind::Episode:
        return "episode";
    case MemoryKind::Procedure:
        return "procedure";
    case MemoryKind::RecoveryLesson:
        return "recovery-lesson";
    case MemoryKind::SkillHint:
        return "skill-hint";
    }
    return "unknown";
}

std::optional<MemoryKind> memory_kind_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryKind> kKinds[] = {
        {"preference", MemoryKind::Preference},
        {"environment-fact", MemoryKind::EnvironmentFact},
        {"application-fact", MemoryKind::ApplicationFact},
        {"episode", MemoryKind::Episode},
        {"procedure", MemoryKind::Procedure},
        {"recovery-lesson", MemoryKind::RecoveryLesson},
        {"skill-hint", MemoryKind::SkillHint},
    };
    for (const auto &[kind_name, kind] : kKinds) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string memory_verification_name(MemoryVerification verification) {
    switch (verification) {
    case MemoryVerification::Unverified:
        return "unverified";
    case MemoryVerification::Observed:
        return "observed";
    case MemoryVerification::Verified:
        return "verified";
    case MemoryVerification::HumanConfirmed:
        return "human-confirmed";
    }
    return "unknown";
}

std::optional<MemoryVerification> memory_verification_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryVerification> kLevels[] = {
        {"unverified", MemoryVerification::Unverified},
        {"observed", MemoryVerification::Observed},
        {"verified", MemoryVerification::Verified},
        {"human-confirmed", MemoryVerification::HumanConfirmed},
    };
    for (const auto &[level_name, level] : kLevels) {
        if (level_name == name) {
            return level;
        }
    }
    return std::nullopt;
}

std::string memory_status_name(MemoryStatus status) {
    switch (status) {
    case MemoryStatus::Active:
        return "active";
    case MemoryStatus::Superseded:
        return "superseded";
    case MemoryStatus::Tombstoned:
        return "tombstoned";
    }
    return "unknown";
}

std::optional<MemoryStatus> memory_status_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryStatus> kStatuses[] = {
        {"active", MemoryStatus::Active},
        {"superseded", MemoryStatus::Superseded},
        {"tombstoned", MemoryStatus::Tombstoned},
    };
    for (const auto &[status_name, status] : kStatuses) {
        if (status_name == name) {
            return status;
        }
    }
    return std::nullopt;
}

std::string memory_mutation_type_name(MemoryMutationType type) {
    switch (type) {
    case MemoryMutationType::Add:
        return "add";
    case MemoryMutationType::Update:
        return "update";
    case MemoryMutationType::Supersede:
        return "supersede";
    case MemoryMutationType::Tombstone:
        return "tombstone";
    case MemoryMutationType::Noop:
        return "noop";
    }
    return "unknown";
}

std::optional<MemoryMutationType> memory_mutation_type_from(std::string_view name) {
    static const std::pair<std::string_view, MemoryMutationType> kTypes[] = {
        {"add", MemoryMutationType::Add},
        {"update", MemoryMutationType::Update},
        {"supersede", MemoryMutationType::Supersede},
        {"tombstone", MemoryMutationType::Tombstone},
        {"noop", MemoryMutationType::Noop},
    };
    for (const auto &[type_name, type] : kTypes) {
        if (type_name == name) {
            return type;
        }
    }
    return std::nullopt;
}

std::string mutation_reason_name(MutationReasonCode reason) {
    switch (reason) {
    case MutationReasonCode::VerifiedEvent:
        return "verified-event";
    case MutationReasonCode::HumanCorrection:
        return "human-correction";
    case MutationReasonCode::Consolidation:
        return "consolidation";
    case MutationReasonCode::RetentionExpiry:
        return "retention-expiry";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void> MemoryRecord::validate() const {
    const auto schema = validate_schema_version(schema_version, memory_schema_current());
    if (!schema) {
        return schema.error();
    }
    if (id.is_nil()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record id must be non-nil");
    }
    if (statement.empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record requires a statement");
    }
    if (!(confidence >= 0.0F && confidence <= 1.0F) || std::isnan(confidence)) {
        return memory_error(MemoryDomainCode::InvalidRecord, "confidence must be within [0,1]");
    }
    // Secrets never enter long-term memory (design Context/Memory §13.2).
    if (sensitivity == Sensitivity::Secret) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "secret content must not enter memory");
    }
    // Imported records carry their namespace and stay below HumanConfirmed;
    // home-grown HumanConfirmed records always trace to events.
    if (source_namespace.has_value() && verification == MemoryVerification::HumanConfirmed) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "imported memory cannot claim human confirmation");
    }
    if (!source_namespace.has_value() && verification == MemoryVerification::HumanConfirmed &&
        provenance.empty()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "human-confirmed memory requires event provenance");
    }
    if (provenance.empty() && confidence > 0.3F) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "untraceable memory must keep low confidence");
    }
    if (validity.valid_until.has_value() && *validity.valid_until < validity.valid_from) {
        return memory_error(MemoryDomainCode::InvalidRecord, "validity interval is inverted");
    }
    if (expires_at.has_value() && *expires_at < recorded_at) {
        return memory_error(MemoryDomainCode::InvalidRecord, "expiry precedes recording time");
    }
    if (status == MemoryStatus::Superseded && !supersedes.has_value()) {
        return memory_error(MemoryDomainCode::InvalidRecord,
                            "superseded records must reference their replacement");
    }
    return Result<void>{};
}

Result<void> MemoryMutation::validate() const {
    const auto proposal = proposed.validate();
    if (!proposal) {
        return proposal.error();
    }
    if (id.is_nil()) {
        return memory_error(MemoryDomainCode::InvalidMutation, "mutation id must be non-nil");
    }
    if (proposed.scope != scope) {
        return memory_error(MemoryDomainCode::InvalidMutation,
                            "mutation scope must match the proposed record scope");
    }
    switch (type) {
    case MemoryMutationType::Add:
        if (target.has_value()) {
            return memory_error(MemoryDomainCode::InvalidMutation, "add must not target a record");
        }
        break;
    case MemoryMutationType::Update:
    case MemoryMutationType::Supersede:
    case MemoryMutationType::Tombstone:
        if (!target.has_value() || target->is_nil()) {
            return memory_error(MemoryDomainCode::InvalidMutation,
                                "mutation type requires a target record");
        }
        if (!expected_version.has_value()) {
            return memory_error(MemoryDomainCode::InvalidMutation,
                                "mutation type requires an expected version");
        }
        break;
    case MemoryMutationType::Noop:
        return Result<void>{};
    }
    if (type == MemoryMutationType::Supersede && !proposed.supersedes.has_value()) {
        return memory_error(MemoryDomainCode::InvalidMutation,
                            "supersede proposals must name the replaced record");
    }
    if (evidence.empty() && reason != MutationReasonCode::RetentionExpiry) {
        return memory_error(MemoryDomainCode::InvalidMutation, "mutations require event evidence");
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// Error domain
// ---------------------------------------------------------------------------

std::string memory_domain_code_name(MemoryDomainCode code) {
    switch (code) {
    case MemoryDomainCode::InvalidRecord:
        return "InvalidRecord";
    case MemoryDomainCode::InvalidMutation:
        return "InvalidMutation";
    case MemoryDomainCode::ScopeDenied:
        return "ScopeDenied";
    case MemoryDomainCode::VersionConflict:
        return "VersionConflict";
    case MemoryDomainCode::SchemaUnsupported:
        return "SchemaUnsupported";
    }
    return "Unknown";
}

Error make_memory_error(MemoryDomainCode code, std::string safe_message, bool retryable,
                        std::optional<OperationId> operation) {
    Error error;
    error.domain = "mira.memory";
    error.domain_code = static_cast<std::int32_t>(code);
    error.retryable = retryable;
    error.safe_message = std::move(safe_message);
    error.operation_id = std::move(operation);
    switch (code) {
    case MemoryDomainCode::InvalidRecord:
    case MemoryDomainCode::InvalidMutation:
    case MemoryDomainCode::SchemaUnsupported:
        error.code = ErrorCode::InvalidArgument;
        break;
    case MemoryDomainCode::ScopeDenied:
        error.code = ErrorCode::PermissionDenied;
        break;
    case MemoryDomainCode::VersionConflict:
        error.code = ErrorCode::InvalidState;
        break;
    }
    return error;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

JsonValue memory_scope_to_json(const MemoryScope &scope) {
    JsonValue::Object object;
    object.emplace_back("kind", memory_scope_kind_name(scope.kind));
    object.emplace_back("subject_id", scope.subject_id);
    if (scope.tenant_id.has_value()) {
        object.emplace_back("tenant_id", *scope.tenant_id);
    }
    return JsonValue(std::move(object));
}

Result<MemoryScope> memory_scope_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory scope must be an object");
    }
    MemoryScope scope;
    const auto *kind = json.find("kind");
    if (kind != nullptr && kind->is_string()) {
        const auto parsed = memory_scope_kind_from(*kind->as_string());
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord, "memory scope kind is unknown");
        }
        scope.kind = *parsed;
    }
    const auto *subject = json.find("subject_id");
    if (subject != nullptr && subject->is_string()) {
        scope.subject_id = *subject->as_string();
    }
    const auto *tenant = json.find("tenant_id");
    if (tenant != nullptr && tenant->is_string()) {
        scope.tenant_id = *tenant->as_string();
    }
    return scope;
}

JsonValue memory_record_to_json(const MemoryRecord &record) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(memory_schema_current().major)},
                          {"minor", static_cast<std::int64_t>(memory_schema_current().minor)}});
    object.emplace_back("id", record.id.to_string());
    object.emplace_back("scope", memory_scope_to_json(record.scope));
    object.emplace_back("kind", memory_kind_name(record.kind));
    object.emplace_back("statement", record.statement);
    if (record.evidence.has_value()) {
        object.emplace_back("evidence", artifact_ref_to_json(*record.evidence));
    }
    JsonValue::Object validity;
    validity.emplace_back("valid_from", wall_nanos(record.validity.valid_from));
    if (record.validity.valid_until.has_value()) {
        validity.emplace_back("valid_until", wall_nanos(*record.validity.valid_until));
    }
    object.emplace_back("validity", JsonValue(std::move(validity)));
    object.emplace_back("recorded_at", wall_nanos(record.recorded_at));
    JsonValue::Array provenance;
    for (const auto &event : record.provenance) {
        provenance.emplace_back(event.to_string());
    }
    object.emplace_back("provenance", JsonValue(std::move(provenance)));
    object.emplace_back("verification", memory_verification_name(record.verification));
    object.emplace_back("confidence", static_cast<double>(record.confidence));
    object.emplace_back("sensitivity", sensitivity_name(record.sensitivity));
    object.emplace_back("status", memory_status_name(record.status));
    if (record.supersedes.has_value()) {
        object.emplace_back("supersedes", record.supersedes->to_string());
    }
    if (record.expires_at.has_value()) {
        object.emplace_back("expires_at", wall_nanos(*record.expires_at));
    }
    if (record.source_namespace.has_value()) {
        object.emplace_back("source_namespace", *record.source_namespace);
    }
    return JsonValue(std::move(object));
}

Result<MemoryRecord> memory_record_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::SchemaUnsupported, "memory record must be an object");
    }
    MemoryRecord record;
    const auto *schema = json.find("schema_version");
    if (schema != nullptr && schema->is_object()) {
        const auto *major = schema->find("major");
        const auto *minor = schema->find("minor");
        if (major != nullptr && major->is_integer() && minor != nullptr && minor->is_integer()) {
            record.schema_version =
                SchemaVersion{static_cast<std::uint16_t>(major->as_integer().value()),
                              static_cast<std::uint16_t>(minor->as_integer().value())};
        }
        const auto supported =
            validate_schema_version(record.schema_version, memory_schema_current());
        if (!supported) {
            return supported.error();
        }
    }
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record requires an id");
    }
    const auto parsed_id = MemoryId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidRecord, "memory record id is malformed");
    }
    record.id = *parsed_id;
    const auto *scope = json.find("scope");
    if (scope != nullptr) {
        auto parsed = memory_scope_from_json(*scope);
        if (!parsed) {
            return parsed.error();
        }
        record.scope = std::move(parsed).value();
    }
    if (auto kind = enum_from_json<MemoryKind>(json, "kind", memory_kind_from)) {
        record.kind = *kind;
    }
    const auto *statement = json.find("statement");
    if (statement != nullptr && statement->is_string()) {
        record.statement = *statement->as_string();
    }
    const auto *evidence = json.find("evidence");
    if (evidence != nullptr && !evidence->is_null()) {
        auto parsed = artifact_ref_from(*evidence);
        if (!parsed) {
            return parsed.error();
        }
        record.evidence = std::move(parsed).value();
    }
    const auto *validity = json.find("validity");
    if (validity != nullptr && validity->is_object()) {
        const auto *from = validity->find("valid_from");
        if (from != nullptr && from->is_integer()) {
            record.validity.valid_from = wall_from_nanos(from->as_integer().value());
        }
        const auto *until = validity->find("valid_until");
        if (until != nullptr && until->is_integer()) {
            record.validity.valid_until = wall_from_nanos(until->as_integer().value());
        }
    }
    const auto *recorded = json.find("recorded_at");
    if (recorded != nullptr && recorded->is_integer()) {
        record.recorded_at = wall_from_nanos(recorded->as_integer().value());
    }
    const auto *provenance = json.find("provenance");
    if (provenance != nullptr && provenance->is_array()) {
        for (const auto &event : *provenance->as_array()) {
            if (auto parsed = EventId::parse(event.as_string() ? *event.as_string() : ""); parsed) {
                record.provenance.emplace_back(*parsed);
            }
        }
    }
    if (auto verification =
            enum_from_json<MemoryVerification>(json, "verification", memory_verification_from)) {
        record.verification = *verification;
    }
    const auto *confidence = json.find("confidence");
    if (confidence != nullptr && confidence->is_number()) {
        const double value = confidence->as_number().value();
        if (value >= 0.0 && value <= 1.0) {
            record.confidence = static_cast<float>(value);
        }
    }
    const auto *sensitivity = json.find("sensitivity");
    if (sensitivity != nullptr && sensitivity->is_string()) {
        const auto parsed = sensitivity_from(*sensitivity);
        if (!parsed) {
            return memory_error(MemoryDomainCode::InvalidRecord,
                                "memory record has unknown sensitivity");
        }
        record.sensitivity = *parsed;
    }
    if (auto status = enum_from_json<MemoryStatus>(json, "status", memory_status_from)) {
        record.status = *status;
    }
    const auto *supersedes = json.find("supersedes");
    if (supersedes != nullptr && supersedes->is_string()) {
        if (auto parsed = MemoryId::parse(*supersedes->as_string()); parsed) {
            record.supersedes = *parsed;
        }
    }
    const auto *expires = json.find("expires_at");
    if (expires != nullptr && expires->is_integer()) {
        record.expires_at = wall_from_nanos(expires->as_integer().value());
    }
    const auto *namespace_field = json.find("source_namespace");
    if (namespace_field != nullptr && namespace_field->is_string()) {
        record.source_namespace = *namespace_field->as_string();
    }
    return record;
}

JsonValue memory_mutation_to_json(const MemoryMutation &mutation) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(memory_schema_current().major)},
                          {"minor", static_cast<std::int64_t>(memory_schema_current().minor)}});
    object.emplace_back("id", mutation.id.to_string());
    object.emplace_back("type", memory_mutation_type_name(mutation.type));
    object.emplace_back("scope", memory_scope_to_json(mutation.scope));
    if (mutation.target.has_value()) {
        object.emplace_back("target", mutation.target->to_string());
    }
    if (mutation.expected_version.has_value()) {
        object.emplace_back("expected_version",
                            static_cast<std::int64_t>(*mutation.expected_version));
    }
    object.emplace_back("proposed", memory_record_to_json(mutation.proposed));
    JsonValue::Array evidence;
    for (const auto &event : mutation.evidence) {
        evidence.emplace_back(event.to_string());
    }
    object.emplace_back("evidence", JsonValue(std::move(evidence)));
    object.emplace_back("reason", mutation_reason_name(mutation.reason));
    return JsonValue(std::move(object));
}

Result<MemoryMutation> memory_mutation_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return memory_error(MemoryDomainCode::SchemaUnsupported,
                            "memory mutation must be an object");
    }
    MemoryMutation mutation;
    const auto *id = json.find("id");
    if (id == nullptr || !id->is_string()) {
        return memory_error(MemoryDomainCode::InvalidMutation, "memory mutation requires an id");
    }
    const auto parsed_id = MutationId::parse(*id->as_string());
    if (!parsed_id) {
        return memory_error(MemoryDomainCode::InvalidMutation, "memory mutation id is malformed");
    }
    mutation.id = *parsed_id;
    if (auto type = enum_from_json<MemoryMutationType>(json, "type", memory_mutation_type_from)) {
        mutation.type = *type;
    }
    const auto *scope = json.find("scope");
    if (scope != nullptr) {
        auto parsed = memory_scope_from_json(*scope);
        if (!parsed) {
            return parsed.error();
        }
        mutation.scope = std::move(parsed).value();
    }
    const auto *target = json.find("target");
    if (target != nullptr && target->is_string()) {
        if (auto parsed = MemoryId::parse(*target->as_string()); parsed) {
            mutation.target = *parsed;
        }
    }
    const auto *version = json.find("expected_version");
    if (version != nullptr && version->is_integer()) {
        mutation.expected_version = static_cast<std::uint64_t>(version->as_integer().value());
    }
    const auto *proposed = json.find("proposed");
    if (proposed != nullptr) {
        auto parsed = memory_record_from_json(*proposed);
        if (!parsed) {
            return parsed.error();
        }
        mutation.proposed = std::move(parsed).value();
    }
    const auto *evidence = json.find("evidence");
    if (evidence != nullptr && evidence->is_array()) {
        for (const auto &event : *evidence->as_array()) {
            if (auto parsed = EventId::parse(event.as_string() ? *event.as_string() : ""); parsed) {
                mutation.evidence.emplace_back(*parsed);
            }
        }
    }
    const auto *reason = json.find("reason");
    if (reason != nullptr && reason->is_string()) {
        const std::string_view name = *reason->as_string();
        if (name == "verified-event") {
            mutation.reason = MutationReasonCode::VerifiedEvent;
        } else if (name == "human-correction") {
            mutation.reason = MutationReasonCode::HumanCorrection;
        } else if (name == "consolidation") {
            mutation.reason = MutationReasonCode::Consolidation;
        } else if (name == "retention-expiry") {
            mutation.reason = MutationReasonCode::RetentionExpiry;
        }
    }
    return mutation;
}

} // namespace mira
