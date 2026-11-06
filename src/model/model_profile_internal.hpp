#pragma once

#include <mira/model_profile.hpp>

namespace mira {

// Shared configuration gate for routing, direct mapper calls and pre-upload
// provider admission. Keep diagnostics free of host-supplied keys and values.
[[nodiscard]] Result<void> validate_profile_extra_body(const ModelProfile &profile);

} // namespace mira
