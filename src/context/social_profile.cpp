#include <mira/social_profile.hpp>

#include "social_profile_codec.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace mira {
namespace {

using Source = SocialAttributeSource;
using Verification = MemoryVerification;

struct SourcePolicy final {
    Source source;
    std::string_view name;
    Verification verification;
};

constexpr std::array kSources = {
    SourcePolicy{Source::SelfReported, "self-reported", Verification::Observed},
    SourcePolicy{Source::UserConfirmed, "user-confirmed", Verification::HumanConfirmed},
    SourcePolicy{Source::Inferred, "inferred", Verification::Unverified},
};

const SourcePolicy *source_policy(Source source) noexcept {
    const auto found = std::ranges::find(kSources, source, &SourcePolicy::source);
    return found != kSources.end() ? &*found : nullptr;
}

constexpr std::size_t kIdWidth = 32;

MemoryId assertion_id(MemoryKind kind, const MemoryScope &scope, std::string_view field,
                      const SourcePolicy &source, EventId event) {
    const auto digest = digest_string("mira-" + memory_kind_name(kind) + "-v1/" + *scope.tenant_id +
                                      "/" + scope.subject_id + "/" + std::string(field) + "/" +
                                      std::string(source.name) + "/" + event.to_string());
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return MemoryId{Id128{bytes}};
}

std::optional<Id128> canonical_id(std::string_view text) {
    const auto id = Id128::parse(text);
    return id && !id->is_nil() && id->to_string() == text ? id : std::nullopt;
}

MemoryRecord assertion_base(MemoryKind kind, MemoryScope scope, MemoryId id, Verification level,
                            std::string statement, EventId event, ValidityInterval validity,
                            std::chrono::system_clock::time_point recorded_at) {
    MemoryRecord record;
    record.scope = std::move(scope);
    record.id = id;
    record.kind = kind;
    record.statement = std::move(statement);
    record.validity = validity;
    record.recorded_at = recorded_at;
    record.provenance = {event};
    record.verification = level;
    record.sensitivity = Sensitivity::Sensitive;
    return record;
}

} // namespace

std::string_view social_attribute_source_name(Source source) noexcept {
    const auto *policy = source_policy(source);
    return policy != nullptr ? policy->name : std::string_view{};
}

std::optional<SocialAttributeSource> social_attribute_source_from(std::string_view name) noexcept {
    const auto found = std::ranges::find(kSources, name, &SourcePolicy::name);
    return found != kSources.end() ? std::optional{found->source} : std::nullopt;
}

bool is_social_attribute_field(std::string_view field) noexcept {
    const auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
    return !field.empty() && field.size() <= 48 && letter(field.front()) &&
           std::ranges::all_of(field, [&](char c) {
               return letter(c) || (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

bool is_social_object_scope(const MemoryScope &scope) {
    const std::string_view subject = scope.subject_id;
    return scope.kind == MemoryScopeKind::SocialObject && scope.tenant_id &&
           canonical_id(*scope.tenant_id) && subject.size() == 2 * kIdWidth + 1 &&
           subject[kIdWidth] == '/' && canonical_id(subject.substr(0, kIdWidth)) &&
           canonical_id(subject.substr(kIdWidth + 1));
}

Result<MemoryScope> social_object_scope(const SocialObjectKey &key) {
    if (key.environment.tenant.is_nil() || key.environment.environment.is_nil() ||
        key.object.is_nil())
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid social object key");
    return MemoryScope{MemoryScopeKind::SocialObject,
                       key.environment.environment.to_string() + "/" + key.object.to_string(),
                       key.environment.tenant.to_string()};
}

Result<MemoryScope> social_environment_scope(const SocialEnvironmentKey &key) {
    if (key.tenant.is_nil() || key.environment.is_nil())
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid social environment key");
    return MemoryScope{MemoryScopeKind::SocialEnvironment, key.environment.to_string(),
                       key.tenant.to_string()};
}

std::optional<MemoryScope> social_environment_scope_of(const MemoryScope &scope) {
    if (!is_social_scope(scope.kind) || !scope.tenant_id || !canonical_id(*scope.tenant_id))
        return std::nullopt;
    const std::string_view environment = std::string_view{scope.subject_id}.substr(0, kIdWidth);
    if (!canonical_id(environment) ||
        (scope.kind == MemoryScopeKind::SocialEnvironment && scope.subject_id.size() != kIdWidth))
        return std::nullopt;
    return MemoryScope{MemoryScopeKind::SocialEnvironment, std::string(environment),
                       scope.tenant_id};
}

Result<MemoryScope> social_relationship_scope(const SocialRelationshipKey &key) {
    auto scope = social_object_scope(key.from);
    if (!scope)
        return scope.error();
    if (key.from.environment != key.to.environment || key.to.object.is_nil() ||
        key.from.object == key.to.object)
        return make_memory_error(MemoryDomainCode::ScopeDenied,
                                 "invalid social relationship endpoints");
    scope.value().kind = MemoryScopeKind::SocialRelationship;
    scope.value().subject_id += "/" + key.to.object.to_string();
    return scope;
}

std::optional<SocialRelationshipKey> social_relationship_endpoints(const MemoryScope &scope) {
    const std::string_view subject = scope.subject_id;
    if (scope.kind != MemoryScopeKind::SocialRelationship || !scope.tenant_id ||
        subject.size() != 3 * kIdWidth + 2 || subject[kIdWidth] != '/' ||
        subject[2 * kIdWidth + 1] != '/')
        return std::nullopt;
    const auto tenant = canonical_id(*scope.tenant_id);
    const auto environment = canonical_id(subject.substr(0, kIdWidth));
    const auto from = canonical_id(subject.substr(kIdWidth + 1, kIdWidth));
    const auto to = canonical_id(subject.substr(2 * kIdWidth + 2));
    if (!tenant || !environment || !from || !to || from == to)
        return std::nullopt;
    const SocialEnvironmentKey key{TenantId{*tenant}, *environment};
    return SocialRelationshipKey{{key, *from}, {key, *to}};
}

Result<void> MemoryRecord::validate_social_assertion() const {
    std::string_view field;
    const SourcePolicy *policy = nullptr;
    if (kind == MemoryKind::SocialAttribute && social_attribute && !social_relationship &&
        is_social_object_scope(scope)) {
        field = social_attribute->field;
        policy = source_policy(social_attribute->source);
    } else if (kind == MemoryKind::SocialRelationship && social_relationship && !social_attribute &&
               social_relationship_endpoints(scope)) {
        field = social_relationship->predicate;
        policy = source_policy(social_relationship->source);
    }
    const bool has_referent = kind == MemoryKind::SocialRelationship && social_relationship &&
                              social_relationship->source == Source::Inferred &&
                              schema_version.minor >= 7 && provenance.size() == 2 &&
                              !provenance.back().is_nil() &&
                              provenance.back() != provenance.front();
    if (policy == nullptr || !is_social_attribute_field(field) ||
        verification != policy->verification || (provenance.size() != 1 && !has_referent) ||
        provenance.front().is_nil() ||
        id != assertion_id(kind, scope, field, *policy, provenance.front()) || source_namespace ||
        supersedes || alternative_of || status == MemoryStatus::Superseded ||
        sensitivity != Sensitivity::Sensitive || recorded_at.time_since_epoch().count() <= 0 ||
        validity.valid_from.time_since_epoch().count() <= 0 ||
        (validity.valid_until && *validity.valid_until <= validity.valid_from) ||
        statement.size() > 2048 || statement.find('\0') != std::string::npos ||
        memory_normalize_claim_key(statement).empty() ||
        !parse_json(canonical_json_string(JsonValue{statement}))) {
        return make_memory_error(MemoryDomainCode::InvalidRecord, "invalid social assertion");
    }
    return {};
}

Result<MemoryRecord> make_social_attribute(const SocialObjectKey &key, SocialAttribute attribute,
                                           std::string value, EventId source_event,
                                           ValidityInterval validity,
                                           std::chrono::system_clock::time_point recorded_at) {
    auto scope = social_object_scope(key);
    if (!scope)
        return scope.error();
    const auto *policy = source_policy(attribute.source);
    if (policy == nullptr || !is_social_attribute_field(attribute.field))
        return make_memory_error(MemoryDomainCode::InvalidRecord, "invalid social attribute key");
    const auto id = assertion_id(MemoryKind::SocialAttribute, scope.value(), attribute.field,
                                 *policy, source_event);
    auto result =
        assertion_base(MemoryKind::SocialAttribute, std::move(scope).value(), id,
                       policy->verification, std::move(value), source_event, validity, recorded_at);
    result.social_attribute = std::move(attribute);
    if (const auto valid = result.validate(); !valid)
        return valid.error();
    return result;
}

Result<MemoryRecord> make_social_relationship(const SocialRelationshipKey &key,
                                              SocialRelationship relationship,
                                              std::string statement, EventId source_event,
                                              ValidityInterval validity,
                                              std::chrono::system_clock::time_point recorded_at) {
    return make_social_relationship(key, std::move(relationship), std::move(statement),
                                    source_event, validity, recorded_at, std::nullopt);
}

Result<MemoryRecord> make_social_relationship(const SocialRelationshipKey &key,
                                              SocialRelationship relationship,
                                              std::string statement, EventId source_event,
                                              ValidityInterval validity,
                                              std::chrono::system_clock::time_point recorded_at,
                                              std::optional<EventId> referent_event) {
    auto scope = social_relationship_scope(key);
    if (!scope)
        return scope.error();
    const auto *policy = source_policy(relationship.source);
    if (!policy || !is_social_attribute_field(relationship.predicate))
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "invalid social relationship predicate");
    const auto id = assertion_id(MemoryKind::SocialRelationship, scope.value(),
                                 relationship.predicate, *policy, source_event);
    auto result = assertion_base(MemoryKind::SocialRelationship, std::move(scope).value(), id,
                                 policy->verification, std::move(statement), source_event, validity,
                                 recorded_at);
    result.social_relationship = std::move(relationship);
    if (referent_event)
        result.provenance.push_back(*referent_event);
    if (const auto valid = result.validate(); !valid)
        return valid.error();
    return result;
}

namespace social_profile_detail {

void encode_assertion(const MemoryRecord &record, JsonValue::Object &object) {
    if (record.social_attribute) {
        object.emplace_back("social_attribute",
                            JsonValue::Object{{"field", record.social_attribute->field},
                                              {"source", std::string(social_attribute_source_name(
                                                             record.social_attribute->source))}});
    }
    if (record.social_relationship) {
        object.emplace_back(
            "social_relationship",
            JsonValue::Object{{"predicate", record.social_relationship->predicate},
                              {"source", std::string(social_attribute_source_name(
                                             record.social_relationship->source))}});
    }
}

Result<void> decode_assertion(const JsonValue &json, MemoryRecord &record) {
    const auto decode = [](const JsonValue &value, const char *key) -> Result<SocialAttribute> {
        const auto *field = value.find(key);
        const auto *source_value = value.find("source");
        const auto source = source_value && source_value->is_string()
                                ? social_attribute_source_from(*source_value->as_string())
                                : std::nullopt;
        if (!field || !field->is_string() || !source)
            return make_memory_error(MemoryDomainCode::InvalidRecord,
                                     "invalid social assertion data");
        return SocialAttribute{*field->as_string(), *source};
    };
    if (const auto *attribute = json.find("social_attribute")) {
        auto parsed = decode(*attribute, "field");
        if (!parsed)
            return parsed.error();
        record.social_attribute = std::move(parsed).value();
    }
    if (const auto *relationship = json.find("social_relationship")) {
        auto parsed = decode(*relationship, "predicate");
        if (!parsed)
            return parsed.error();
        record.social_relationship =
            SocialRelationship{std::move(parsed.value().field), parsed.value().source};
    }
    if (is_social_assertion(record.kind) || is_social_scope(record.scope.kind) ||
        record.social_attribute || record.social_relationship)
        return record.validate();
    return {};
}

Result<void> validate_query(const MemoryQuery &query) {
    if (!query.social_attribute_field && !query.social_relationship_predicate) {
        if (std::ranges::any_of(query.scopes,
                                [](const auto &scope) { return is_social_scope(scope.kind); }))
            return make_memory_error(MemoryDomainCode::ScopeDenied,
                                     "social recall needs an exact field or predicate");
        return {};
    }
    const bool relationship = query.social_relationship_predicate.has_value();
    const auto &key =
        relationship ? *query.social_relationship_predicate : *query.social_attribute_field;
    const auto kind = relationship ? MemoryKind::SocialRelationship : MemoryKind::SocialAttribute;
    if ((relationship && query.social_attribute_field) || !is_social_attribute_field(key) ||
        query.scopes.size() != 1 || !query.text.empty() || !query.exact_terms.empty() ||
        query.claim_key || !query.query_embedding.values.empty() ||
        (query.kinds && (query.kinds->size() != 1 || query.kinds->front() != kind)))
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "invalid structured social query");
    const auto &scope = query.scopes.front();
    if (relationship ? !social_relationship_endpoints(scope) : !is_social_object_scope(scope))
        return make_memory_error(MemoryDomainCode::ScopeDenied,
                                 "social query requires an exact scope");
    return {};
}

} // namespace social_profile_detail

} // namespace mira
