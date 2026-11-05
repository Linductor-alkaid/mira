#include "support/test.hpp"
#include <mira/coordinates.hpp>
#include <mira/model_schema.hpp>
#include <mira/observation.hpp>
#include <mira/task_checkpoint.hpp>
#include <mira/task_recovery.hpp>
#include <mira/tool_module.hpp>
#include <mira/tool_module_registry.hpp>

namespace {
using namespace mira;

int diagnostic_names_are_host_contracts() {
    constexpr std::string_view triggers[] = {
        "Watermark", "Pause", "Takeover", "Shutdown", "ProjectionMissing", "PeriodicRebuild"};
    constexpr std::string_view actions[] = {"ResumeObserving", "ResumeVerifying", "AlreadyTerminal",
                                            "NoState"};
    for (std::size_t index = 0; index < std::size(triggers); ++index) {
        MIRA_CHECK(checkpoint_trigger_name(static_cast<CheckpointTrigger>(index)) ==
                   triggers[index]);
    }
    for (std::size_t index = 0; index < std::size(actions); ++index) {
        MIRA_CHECK(recovery_action_name(static_cast<RecoveryAction>(index)) == actions[index]);
    }
    MIRA_CHECK(checkpoint_trigger_name(static_cast<CheckpointTrigger>(255)) == "Unknown");
    MIRA_CHECK(recovery_action_name(static_cast<RecoveryAction>(255)) == "Unknown");
    return 0;
}

int public_vocabulary_keeps_its_values() {
    static_assert(static_cast<unsigned>(DecisionSource::StrictToolCall) == 1);
    static_assert(static_cast<unsigned>(ErrorCode::ConfirmationRequired) == 16);
    static_assert(static_cast<unsigned>(OperationState::CompletionReceived) == 4);
    static_assert(static_cast<unsigned>(ClockSyncQuality::Synced) == 0);
    static_assert(static_cast<unsigned>(CoordinateSpaceKind::FramePixels) == 5);
    static_assert(static_cast<unsigned>(CoordinateSpaceKind::Unknown) == 8);
    for (unsigned value = 0; value < 256; ++value) {
        const auto parsed = parse_coordinate_space_kind(static_cast<std::uint8_t>(value));
        MIRA_CHECK(parsed.has_value() == (value <= 8));
        if (parsed) {
            CoordinateSpace space;
            space.kind = parsed.value();
            MIRA_CHECK(static_cast<unsigned>(space.kind) == value);
        } else {
            MIRA_CHECK(parsed.error().code == ErrorCode::UnsupportedVersion);
        }
    }
    return 0;
}

template <typename T> int expect_module_error(const Result<T> &result, std::string_view message) {
    MIRA_CHECK(!result);
    MIRA_CHECK(result.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(result.error().domain == "mira.tool_module");
    MIRA_CHECK(result.error().domain_code == 0 && !result.error().retryable);
    MIRA_CHECK(result.error().safe_message == message);
    MIRA_CHECK(!result.error().operation_id && !result.error().diagnostic_artifact);
    return 0;
}

int module_error_reuse_preserves_both_entry_points() {
    CapabilityCatalog catalog;
    MIRA_CHECK(expect_module_error(parse_tool_module_manifest(JsonValue{}, catalog),
                                   "manifest must be a JSON object") == 0);
    ModuleTrustConfig trust;
    trust.trusted_out_of_process_signers = {""};
    MIRA_CHECK(expect_module_error(trust.validate(),
                                   "out-of-process signer entry is empty or oversized") == 0);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(diagnostic_names_are_host_contracts() == 0);
    MIRA_CHECK(public_vocabulary_keeps_its_values() == 0);
    MIRA_CHECK(module_error_reuse_preserves_both_entry_points() == 0);
    return 0;
}
