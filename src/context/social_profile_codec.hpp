#pragma once

#include <mira/memory_contracts.hpp>

namespace mira::social_profile_detail {

void encode_assertion(const MemoryRecord &record, JsonValue::Object &object);
[[nodiscard]] Result<void> decode_assertion(const JsonValue &json, MemoryRecord &record);
[[nodiscard]] Result<void> validate_query(const MemoryQuery &query);

} // namespace mira::social_profile_detail
