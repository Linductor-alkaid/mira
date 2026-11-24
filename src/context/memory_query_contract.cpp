#include <mira/memory_contracts.hpp>
#include <mira/skill_environment.hpp>

#include "social_profile_codec.hpp"

#include <algorithm>
#include <cmath>

namespace mira {

Result<void> MemoryQuery::validate() const {
    if (scopes.empty()) {
        return make_memory_error(MemoryDomainCode::ScopeDenied,
                                 "memory queries must state an explicit scope allowlist");
    }
    for (const auto &scope : scopes) {
        if (scope.kind == MemoryScopeKind::SkillEnvironment && !skill_environment_key_of(scope))
            return make_memory_error(MemoryDomainCode::ScopeDenied,
                                     "invalid skill environment scope");
        if (is_world_scope(scope.kind)) {
            return make_memory_error(MemoryDomainCode::ScopeDenied,
                                     "world facts require exact lookup");
        }
        if (scope.subject_id.empty() && scope.kind != MemoryScopeKind::Environment &&
            scope.kind != MemoryScopeKind::Agent) {
            return make_memory_error(MemoryDomainCode::ScopeDenied,
                                     "scoped memory queries require a subject id");
        }
        if (scope.subject_id.size() > 256) {
            return make_memory_error(MemoryDomainCode::ScopeDenied, "scope subject id is too long");
        }
        if (scope.tenant_id.has_value() && scope.tenant_id->size() > 256) {
            return make_memory_error(MemoryDomainCode::ScopeDenied, "scope tenant id is too long");
        }
    }
    if (kinds.has_value() && kinds->empty()) {
        return make_memory_error(MemoryDomainCode::InvalidRecord, "kind filter must not be empty");
    }
    if (record_id && record_id->is_nil()) {
        return make_memory_error(MemoryDomainCode::InvalidRecord, "record filter must be non-nil");
    }
    if (recovery_lesson_key) {
        if (scopes.size() != 1 || as_of_recorded || as_of_valid || social_attribute_field ||
            social_relationship_predicate)
            return make_memory_error(MemoryDomainCode::InvalidRecord,
                                     "recovery lesson recall needs one current scope");
        if (auto valid = recovery_lesson_key->validate(); !valid)
            return valid.error();
    }
    if (auto social = social_profile_detail::validate_query(*this); !social)
        return social.error();
    if (labels.has_value()) {
        if (labels->empty()) {
            return make_memory_error(MemoryDomainCode::InvalidRecord,
                                     "label filter must not be empty");
        }
        if (labels->size() > 16) {
            return make_memory_error(MemoryDomainCode::InvalidRecord,
                                     "label filter must not exceed 16 entries");
        }
        for (const auto &label : *labels) {
            if (label.empty() || label.size() > 64) {
                return make_memory_error(MemoryDomainCode::InvalidRecord,
                                         "label filter entries must be within (0, 64] bytes");
            }
        }
    }
    if (max_results == 0 || max_results > 512) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "max_results must be within (0, 512]");
    }
    if (deadline.count() < 0) {
        return make_memory_error(MemoryDomainCode::InvalidRecord, "deadline must not be negative");
    }
    if (query_embedding.values.size() > 4'096) {
        return make_memory_error(MemoryDomainCode::InvalidRecord, "query embedding exceeds bounds");
    }
    // DEC-111: values always travel with their producer identity. Exactly one
    // of the two being present is a representable-invalid state, rejected here.
    if (query_embedding.values.empty() != query_embedding.embedder.is_nil()) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "query embedding requires matching values and embedder id");
    }
    if (!std::all_of(query_embedding.values.begin(), query_embedding.values.end(),
                     [](float component) { return std::isfinite(component); })) {
        return make_memory_error(MemoryDomainCode::InvalidRecord,
                                 "query embedding values must be finite");
    }
    return Result<void>{};
}

} // namespace mira
