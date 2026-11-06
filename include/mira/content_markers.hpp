#pragma once

#include <string>
#include <vector>

namespace mira {

// Default substring vocabularies shared by Context and Memory (DEC-081).
// Each synchronous call returns an independently owned, mutable vector.
// No tasks, I/O or global mutable policy; allocation exceptions may propagate.
// Matching folds ASCII A-Z only. These defaults do not replace ACL or approval.
[[nodiscard]] std::vector<std::string> default_forbidden_content_markers();
[[nodiscard]] std::vector<std::string> default_injection_content_markers();

} // namespace mira
