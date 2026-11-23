#include <mira/world_fact.hpp>

#include "world_fact_codec.hpp"

#include <algorithm>
#include <utility>

namespace mira {
namespace {

constexpr std::size_t kIdWidth = 32;
constexpr auto kLatestInstant = std::chrono::sys_days{std::chrono::year{2200} / 1 / 1};

bool canonical_id(std::string_view text) {
    const auto id = Id128::parse(text);
    return id && !id->is_nil() && id->to_string() == text;
}

bool valid_scope(const MemoryScope &scope) {
    if (!scope.tenant_id || !canonical_id(*scope.tenant_id))
        return false;
    const std::string_view subject = scope.subject_id;
    if (scope.kind == MemoryScopeKind::WorldEnvironment)
        return canonical_id(subject);
    return scope.kind == MemoryScopeKind::WorldApplication && subject.size() == 2 * kIdWidth + 1 &&
           subject[kIdWidth] == '/' && canonical_id(subject.substr(0, kIdWidth)) &&
           canonical_id(subject.substr(kIdWidth + 1));
}

bool valid_property(const WorldProperty &property) {
    const auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto &name = property.property;
    return !property.entity.is_nil() && !name.empty() && name.size() <= 48 &&
           letter(name.front()) && std::ranges::all_of(name, [&](char c) {
               return letter(c) || (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
}

bool valid_instant(std::chrono::system_clock::time_point instant) {
    return instant.time_since_epoch().count() > 0 && instant < kLatestInstant;
}

MemoryKind kind_for(const MemoryScope &scope) {
    return scope.kind == MemoryScopeKind::WorldApplication ? MemoryKind::ApplicationFact
                                                           : MemoryKind::EnvironmentFact;
}

MemoryId observation_id(const MemoryScope &scope, const WorldProperty &property, EventId event) {
    const auto digest = digest_string("mira-world-observation-v1/" + *scope.tenant_id + "/" +
                                      scope.subject_id + "/" + property.entity.to_string() + "/" +
                                      property.property + "/" + event.to_string());
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return MemoryId{Id128{bytes}};
}

Error invalid_observation() {
    return make_memory_error(MemoryDomainCode::InvalidRecord,
                             "invalid structured world observation");
}

} // namespace

Result<MemoryScope> world_environment_scope(TenantId tenant, Id128 environment) {
    if (tenant.is_nil() || environment.is_nil())
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid world environment key");
    return MemoryScope{MemoryScopeKind::WorldEnvironment, environment.to_string(),
                       tenant.to_string()};
}

Result<MemoryScope> world_application_scope(TenantId tenant, Id128 environment, Id128 application) {
    auto scope = world_environment_scope(tenant, environment);
    if (!scope)
        return scope.error();
    if (application.is_nil())
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid world application key");
    scope.value().kind = MemoryScopeKind::WorldApplication;
    scope.value().subject_id += "/" + application.to_string();
    return scope;
}

Result<MemoryScope> world_application_model_scope(TenantId tenant, Id128 environment,
                                                  Id128 application,
                                                  const Sha256Digest &app_model) {
    if (application.is_nil() || app_model == Sha256Digest{})
        return make_memory_error(MemoryDomainCode::ScopeDenied,
                                 "invalid world application model key");
    const auto version = digest_string("mira-world-app-model-v1/" + application.to_string() + "/" +
                                       app_model.to_string());
    Id128::Bytes bytes{};
    std::copy_n(version.bytes.begin(), bytes.size(), bytes.begin());
    return world_application_scope(tenant, environment, Id128{bytes});
}

Result<MemoryRecord> make_world_observation(const MemoryScope &scope, WorldProperty property,
                                            std::string value, EventId source_event,
                                            ValidityInterval validity,
                                            std::chrono::system_clock::time_point recorded_at) {
    if (!valid_scope(scope) || !valid_property(property) || !valid_instant(validity.valid_from))
        return invalid_observation();
    MemoryRecord record;
    record.scope = scope;
    record.kind = kind_for(scope);
    record.id = observation_id(scope, property, source_event);
    record.statement = std::move(value);
    record.world_property = std::move(property);
    record.provenance = {source_event};
    record.verification = MemoryVerification::Observed;
    record.validity = validity;
    record.recorded_at = recorded_at;
    record.expires_at = memory_default_expiry(record.kind, validity.valid_from);
    if (const auto valid = record.validate(); !valid)
        return valid.error();
    return record;
}

Result<void> WorldFactQuery::validate() const {
    if (!valid_scope(scope))
        return make_memory_error(MemoryDomainCode::ScopeDenied, "invalid world fact scope");
    if (!valid_property(property) || (as_of_valid && !valid_instant(*as_of_valid)) ||
        (as_of_recorded && !valid_instant(*as_of_recorded)))
        return make_memory_error(MemoryDomainCode::InvalidRecord, "invalid world fact query");
    return {};
}

namespace world_fact_detail {

Result<void> validate_record(const MemoryRecord &record) {
    if (!valid_scope(record.scope) || !record.world_property ||
        !valid_property(*record.world_property) || record.kind != kind_for(record.scope) ||
        record.provenance.size() != 1 || record.provenance.front().is_nil() ||
        record.social_attribute || record.social_relationship || record.source_namespace ||
        record.supersedes || record.alternative_of || record.status != MemoryStatus::Active ||
        record.version != 1 || record.verification != MemoryVerification::Observed ||
        record.id !=
            observation_id(record.scope, *record.world_property, record.provenance.front()) ||
        !valid_instant(record.validity.valid_from) || !valid_instant(record.recorded_at) ||
        record.recorded_at < record.validity.valid_from || !record.validity.valid_until ||
        *record.validity.valid_until <= record.validity.valid_from || !record.expires_at ||
        record.expires_at != memory_default_expiry(record.kind, record.validity.valid_from) ||
        *record.validity.valid_until > *record.expires_at ||
        record.recorded_at >= *record.expires_at || record.statement.size() > 2048 ||
        record.statement.find('\0') != std::string::npos ||
        memory_normalize_claim_key(record.statement).empty() ||
        !parse_json(canonical_json_string(JsonValue{record.statement})))
        return invalid_observation();
    return {};
}

void encode(const MemoryRecord &record, JsonValue::Object &object) {
    if (record.world_property) {
        object.emplace_back("world_property",
                            JsonValue::Object{{"entity", record.world_property->entity.to_string()},
                                              {"property", record.world_property->property}});
    }
}

Result<void> decode(const JsonValue &json, MemoryRecord &record) {
    if (const auto *world = json.find("world_property")) {
        const auto *entity = world->find("entity");
        const auto *key = world->find("property");
        const auto id =
            entity && entity->is_string() ? Id128::parse(*entity->as_string()) : std::nullopt;
        if (!id || !key || !key->is_string())
            return invalid_observation();
        record.world_property = WorldProperty{*id, *key->as_string()};
    }
    if (is_world_scope(record.scope.kind) || record.world_property)
        return record.validate();
    return {};
}

} // namespace world_fact_detail
} // namespace mira
