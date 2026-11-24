#include <mira/memory_contracts.hpp>

namespace mira {

Result<void> LegacyMemoryAdd::validate_for(const MemoryMutation &mutation) const {
    if (mutation.type != MemoryMutationType::Add || mutation_id.is_nil() || record_id.is_nil() ||
        mutation.id.is_nil() || mutation.proposed.id.is_nil() || mutation_id == mutation.id ||
        record_id == mutation.proposed.id)
        return make_memory_error(MemoryDomainCode::InvalidMutation, "invalid legacy add identity");
    return {};
}

Result<MemoryMutationResult>
IMemory::apply_add_with_legacy_identity(const MemoryMutation &mutation,
                                        const LegacyMemoryAdd &legacy) {
    if (auto valid = legacy.validate_for(mutation); !valid)
        return valid.error();
    auto previous = mutation;
    previous.id = legacy.mutation_id;
    previous.proposed.id = legacy.record_id;
    return apply(previous);
}

} // namespace mira
