// Shared case helpers for the M24 fork/merge suite, extracted verbatim from
// m24_fork_merge_test.cpp when the suite was split by responsibility across
// two translation units (architecture max-file-lines budget; assertions and
// case semantics unchanged). Pure, stateless helpers only.
#pragma once

#include "m24_fork_merge_support.hpp"

#include <mira/json.hpp>
#include <mira/memory_contracts.hpp>

#include <string>
#include <utility>
#include <vector>

namespace mira::m24_cases {

[[nodiscard]] inline WorkingContextForkSeed m24_fork_seed(std::uint64_t child_session_seed,
                                                          std::uint64_t child_task_seed,
                                                          std::uint64_t child_watermark) {
    WorkingContextForkSeed seed;
    seed.child_session = mira::testing::m24_session_from_seed(child_session_seed);
    seed.child_identity = mira::testing::m24_identity_from_seed(child_task_seed);
    seed.child_watermark = child_watermark;
    return seed;
}

[[nodiscard]] inline WorkingContextIdentity
m24_parent_identity(const WorkingContextSnapshot &snapshot) {
    WorkingContextIdentity identity;
    identity.task = snapshot.task_id;
    identity.task_epoch = snapshot.task_epoch;
    identity.environment_epoch = snapshot.environment_epoch;
    return identity;
}

// Cross-run comparable payload: created_at is the only field a pure fork may
// stamp per invocation (it never enters the digest), so normalization zeroes
// it before byte comparisons (m22 precedent).
[[nodiscard]] inline std::string normalized_json(const WorkingContextSnapshot &snapshot) {
    WorkingContextSnapshot copy = snapshot;
    copy.created_at = Timestamp{};
    return to_json_string(working_context_to_json(copy));
}

[[nodiscard]] inline std::string snapshot_json(const WorkingContextSnapshot &snapshot) {
    return to_json_string(working_context_to_json(snapshot));
}

[[nodiscard]] inline bool has_token(const Error &error, const char *token) {
    return error.safe_message.find(token) != std::string::npos;
}

[[nodiscard]] inline WorkingContextCommitState m24_live(const WorkingContextSnapshot &snapshot) {
    WorkingContextCommitState live;
    live.session = snapshot.session_id;
    live.task = snapshot.task_id;
    live.task_epoch = snapshot.task_epoch;
    live.environment_epoch = snapshot.environment_epoch;
    return live;
}

// The item contents inside one section, joined for cheap order comparisons.
[[nodiscard]] inline std::string section_contents(const std::vector<WorkingContextItem> &section) {
    std::string joined;
    for (const WorkingContextItem &item : section) {
        if (!joined.empty()) {
            joined += "|";
        }
        joined += item.content;
    }
    return joined;
}

[[nodiscard]] inline MemoryScope testing_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "env-42";
    scope.tenant_id = "tenant-9";
    return scope;
}

} // namespace mira::m24_cases
