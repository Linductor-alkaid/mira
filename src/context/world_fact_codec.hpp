#pragma once

#include <mira/memory_contracts.hpp>

namespace mira::world_fact_detail {

[[nodiscard]] Result<void> validate_record(const MemoryRecord &record);
void encode(const MemoryRecord &record, JsonValue::Object &object);
[[nodiscard]] Result<void> decode(const JsonValue &json, MemoryRecord &record);

} // namespace mira::world_fact_detail
