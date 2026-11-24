#include <mira/memory_contracts.hpp>

#include <algorithm>

namespace mira {
namespace {

bool valid_identifier(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == ':' || c == '-';
    });
}

} // namespace

Result<void> RecoveryLessonKey::validate() const {
    if (workflow.is_nil() || ir_digest == Sha256Digest{} || (step && step->is_nil()) ||
        (step_kind && !valid_identifier(*step_kind)) || !valid_identifier(reason_code))
        return make_memory_error(MemoryDomainCode::InvalidRecord, "invalid recovery lesson key");
    return {};
}

} // namespace mira
