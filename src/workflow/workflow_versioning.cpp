#include <mira/workflow_versioning.hpp>

#include <algorithm>

namespace mira {
namespace {

constexpr std::size_t kMaxActorBytes = 256;
constexpr std::size_t kMaxReasonBytes = 2048;

[[nodiscard]] Error versioning_error(ErrorCode code, std::string_view detail) {
    Error error;
    error.code = code;
    error.domain = "mira.workflow";
    error.safe_message = "workflow versioning: ";
    error.safe_message += detail;
    return error;
}

} // namespace

std::string workflow_validation_result_name(WorkflowValidationResult result) {
    switch (result) {
    case WorkflowValidationResult::NotValidated:
        return "not_validated";
    case WorkflowValidationResult::DryRunPassed:
        return "dry_run_passed";
    case WorkflowValidationResult::Validated:
        return "validated";
    case WorkflowValidationResult::Rejected:
        return "rejected";
    }
    return "unknown";
}

Result<WorkflowValidationResult> parse_workflow_validation_result(std::string_view name) {
    for (auto result : {WorkflowValidationResult::NotValidated,
                        WorkflowValidationResult::DryRunPassed,
                        WorkflowValidationResult::Validated,
                        WorkflowValidationResult::Rejected}) {
        if (workflow_validation_result_name(result) == name) {
            return result;
        }
    }
    return versioning_error(ErrorCode::InvalidArgument, "unknown validation result name");
}

bool workflow_version_is_runnable(const WorkflowVersionRecord &record) {
    return record.validation == WorkflowValidationResult::DryRunPassed ||
           record.validation == WorkflowValidationResult::Validated;
}

Result<void> append_workflow_version(WorkflowVersionHistory &history,
                                     const WorkflowVersionRecord &record) {
    if (history.workflow_id.is_nil()) {
        return versioning_error(ErrorCode::InvalidArgument, "history has a nil workflow id");
    }
    if (record.content_digest == Sha256Digest{}) {
        return versioning_error(ErrorCode::InvalidArgument, "content digest must not be nil");
    }
    if (record.actor.size() > kMaxActorBytes || record.reason.size() > kMaxReasonBytes) {
        return versioning_error(ErrorCode::InvalidArgument, "actor or reason exceeds limit");
    }
    if (record.validation != WorkflowValidationResult::NotValidated && !record.validation_evidence) {
        return versioning_error(ErrorCode::InvalidArgument,
                                "validated records must reference validation evidence");
    }
    if (record.validation == WorkflowValidationResult::NotValidated && record.validation_evidence) {
        return versioning_error(ErrorCode::InvalidArgument,
                                "not_validated records carry no evidence");
    }
    if (!history.records.empty()) {
        const WorkflowVersionRecord &head = history.records.back();
        if (record.version <= head.version) {
            return versioning_error(ErrorCode::InvalidArgument, "version must increase");
        }
        if (record.parent_digest != head.content_digest) {
            return versioning_error(ErrorCode::InvalidArgument,
                                    "parent digest must chain to the current head");
        }
    } else if (record.parent_digest != Sha256Digest{}) {
        return versioning_error(ErrorCode::InvalidArgument,
                                "first record must have a nil parent digest");
    }
    history.records.push_back(record);
    return Result<void>{};
}

Result<WorkflowVersionRecord> resolve_workflow_version(const WorkflowVersionHistory &history,
                                                       const Sha256Digest &ir_digest) {
    const auto record = std::find_if(history.records.begin(), history.records.end(),
                                     [&](const WorkflowVersionRecord &candidate) {
                                         return candidate.content_digest == ir_digest;
                                     });
    if (record == history.records.end()) {
        return versioning_error(ErrorCode::NotFound, "no version matches the digest");
    }
    return *record;
}

Result<WorkflowVersionRecord>
latest_runnable_workflow_version(const WorkflowVersionHistory &history) {
    for (auto record = history.records.rbegin(); record != history.records.rend(); ++record) {
        if (workflow_version_is_runnable(*record)) {
            return *record;
        }
    }
    return versioning_error(ErrorCode::NotFound, "history has no runnable version");
}

} // namespace mira
