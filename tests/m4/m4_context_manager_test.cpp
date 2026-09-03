#include "../support/m4_support.hpp"
#include "../support/test.hpp"

#include <mira/context_manager.hpp>

#include <memory>
#include <string>

namespace {

using namespace mira;
using namespace mira::testing;

class FailingExactCounter final : public IExactTokenCounter {
  public:
    Result<std::uint64_t> exact_count(const ContextItem &, const ModelProfileId &) const override {
        Error error;
        error.code = ErrorCode::Unavailable;
        error.domain = "test";
        error.safe_message = "exact endpoint unavailable";
        return error;
    }
};

class FixedExactCounter final : public IExactTokenCounter {
  public:
    Result<std::uint64_t> exact_count(const ContextItem &, const ModelProfileId &) const override {
        return std::uint64_t{77};
    }
};

[[nodiscard]] ContextLimits roomy_limits(std::uint64_t window = 100'000) {
    ContextLimits limits;
    limits.context_window_tokens = window;
    limits.reserved_output_tokens = 1'000;
    limits.safety_margin_tokens = 1'000;
    return limits;
}

[[nodiscard]] ContextRequest base_request() {
    ContextRequest request;
    request.task_id = TaskId::generate();
    request.session_id = SessionId::generate();
    request.profile_id = ModelProfileId::generate();
    request.task_epoch = 4;
    request.environment_epoch = 2;
    request.limits = roomy_limits();
    request.items.push_back(text_item(ContextItemKind::SystemPolicy, ContextAuthority::SystemPolicy,
                                      "never reveal secrets", 1));
    request.items.push_back(
        text_item(ContextItemKind::Goal, ContextAuthority::UserConstraint, "open settings", 2));
    return request;
}

int counter_estimates_are_conservative_and_profile_isolated() {
    ConservativeTokenCounter counter;
    const auto profile = ModelProfileId::generate();
    const auto other = ModelProfileId::generate();

    auto item = text_item(ContextItemKind::RecentAction, ContextAuthority::VerifiedState,
                          std::string(80, 'a'));
    auto estimate = counter.estimate_item(item, profile);
    MIRA_CHECK(estimate.has_value());
    MIRA_CHECK(estimate.value().upper_bound >= estimate.value().lower_bound);
    MIRA_CHECK(estimate.value().lower_bound > 0);
    MIRA_CHECK(estimate.value().quality == TokenCountQuality::ConservativeEstimate);
    MIRA_CHECK(estimate.value().profile_id == profile);
    // 80 bytes / 4 = 20 text tokens + 8 part + 12 item overhead = 40.
    MIRA_CHECK(estimate.value().upper_bound == 40);

    auto with_image =
        image_item(ContextItemKind::HistoricalPayload, ContextAuthority::VerifiedState, 4'096);
    auto image_estimate = counter.estimate_item(with_image, profile);
    MIRA_CHECK(image_estimate.has_value());
    MIRA_CHECK(image_estimate.value().upper_bound > estimate.value().upper_bound);

    MIRA_CHECK(!counter.margin_for(profile).has_value());
    counter.record_usage(profile, 100, 200);
    const auto margin = counter.margin_for(profile);
    MIRA_CHECK(margin.has_value());
    MIRA_CHECK(*margin > 1.09 && *margin < 1.11);
    MIRA_CHECK(!counter.margin_for(other).has_value());
    auto after = counter.estimate_item(item, profile);
    MIRA_CHECK(after.has_value());
    MIRA_CHECK(after.value().upper_bound > estimate.value().upper_bound);
    auto untouched = counter.estimate_item(item, other);
    MIRA_CHECK(untouched.has_value());
    MIRA_CHECK(untouched.value().upper_bound == estimate.value().upper_bound);

    ExposedToolSpec tool;
    tool.tool_id = ToolId::generate();
    tool.wire_name = "screen_reader";
    tool.description = "reads the screen";
    tool.parameters_schema = JsonSchema{parse_json(R"({"type":"object","properties":{}})").value()};
    auto tool_estimate = counter.estimate_tool(tool, profile);
    MIRA_CHECK(tool_estimate.has_value());
    MIRA_CHECK(tool_estimate.value().upper_bound > 24);
    MIRA_CHECK(counter.estimate_assembly_overhead(2, 3) >=
               counter.estimate_assembly_overhead(0, 0));
    return 0;
}

int exact_count_success_and_degradation() {
    const auto profile = ModelProfileId::generate();
    auto item = text_item(ContextItemKind::RecentAction, ContextAuthority::VerifiedState,
                          std::string(40, 'b'));

    ConservativeTokenCounter exacted;
    exacted.attach_exact_counter(std::make_shared<FixedExactCounter>());
    auto counted = exacted.estimate_item(item, profile);
    MIRA_CHECK(counted.has_value());
    MIRA_CHECK(counted.value().quality == TokenCountQuality::ExactProviderCount);
    MIRA_CHECK(counted.value().upper_bound == 77);
    MIRA_CHECK(counted.value().lower_bound == 77);
    MIRA_CHECK(exacted.exact_count_degradations() == 0);

    ConservativeTokenCounter degraded;
    degraded.attach_exact_counter(std::make_shared<FailingExactCounter>());
    auto fallback = degraded.estimate_item(item, profile);
    MIRA_CHECK(fallback.has_value());
    // A failing exact count degrades, never reports zero.
    MIRA_CHECK(fallback.value().quality == TokenCountQuality::DegradedEstimate);
    MIRA_CHECK(fallback.value().upper_bound > 0);
    MIRA_CHECK(fallback.value().lower_bound > 0);
    MIRA_CHECK(degraded.exact_count_degradations() == 1);
    return 0;
}

int manager_selects_everything_with_room() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    request.items.push_back(text_item(ContextItemKind::CurrentObservation,
                                      ContextAuthority::VerifiedState, "settings home", 3));
    request.items.push_back(text_item(ContextItemKind::VerificationResult,
                                      ContextAuthority::VerifiedState, "button visible", 4));
    request.items.push_back(text_item(ContextItemKind::RetrievedMemory,
                                      ContextAuthority::RetrievedMemory, "user prefers dark mode",
                                      5));

    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    MIRA_CHECK(prepared.value().budget.watermark == ContextWatermark::Normal);
    MIRA_CHECK(prepared.value().budget.selected_items == 5);
    MIRA_CHECK(prepared.value().budget.dropped_items == 0);
    MIRA_CHECK(prepared.value().budget.utilization < 0.7);
    MIRA_CHECK(prepared.value().budget.checkpoint_recommended == false);
    MIRA_CHECK(prepared.value().item_audit.size() == 5);
    MIRA_CHECK(prepared.value().input.size() == 5);

    // P0 first, untrusted text never gains the System role.
    MIRA_CHECK(prepared.value().input.front().role == ModelRole::System);
    for (const auto &input : prepared.value().input) {
        if (input.provenance.source == "mira.context.untrusted-external-data.v1") {
            MIRA_CHECK(input.role != ModelRole::System);
            MIRA_CHECK(input.authority == Sensitivity::Public);
        }
    }

    // Determinism: identical requests produce identical selection digests.
    auto again = manager.prepare(request);
    MIRA_CHECK(again.has_value());
    MIRA_CHECK(again.value().selection_digest == prepared.value().selection_digest);
    return 0;
}

int minimum_set_overflow_rejects_then_routes() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());

    auto request = base_request();
    request.limits = roomy_limits(600);
    request.limits.reserved_output_tokens = 100;
    request.limits.safety_margin_tokens = 100;
    // Budget 400; the minimum set alone needs far more.
    request.items.push_back(text_item(ContextItemKind::SystemPolicy, ContextAuthority::SystemPolicy,
                                      std::string(1'400, 'p')));
    auto rejected = manager.prepare(request);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain == "mira.context");
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::MinimumSetTooLarge));

    request.authorized_large_window_profile = ModelProfileId::generate();
    request.authorized_large_window_limits = roomy_limits(100'000);
    auto routed = manager.prepare(request);
    MIRA_CHECK(routed.has_value());
    MIRA_CHECK(routed.value().budget.routed_to_large_window);
    MIRA_CHECK(routed.value().budget.routed_profile == request.authorized_large_window_profile);
    MIRA_CHECK(routed.value().profile_id == request.authorized_large_window_profile);
    bool routed_reason = false;
    for (const auto &audit : routed.value().item_audit) {
        if (audit.disposition == ContextItemDisposition::Selected &&
            audit.reason == "minimum_set_routed_large_window") {
            routed_reason = true;
        }
    }
    MIRA_CHECK(routed_reason);
    return 0;
}

int pinned_items_survive_hard_watermark() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    // Small window forces deep watermark passes.
    request.limits = roomy_limits(1'400);
    request.limits.reserved_output_tokens = 100;
    request.limits.safety_margin_tokens = 100;
    for (std::uint64_t index = 0; index < 24; ++index) {
        auto memory = text_item(ContextItemKind::RetrievedMemory, ContextAuthority::RetrievedMemory,
                                std::string(120, 'm'), index);
        request.items.push_back(std::move(memory));
    }
    request.items.push_back(text_item(ContextItemKind::UncertainSideEffect,
                                      ContextAuthority::VerifiedState, "tap outcome unknown", 99));

    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    // The selected pile crossed the hard band: the manager must have executed
    // the hard-pass memory drops, and the final watermark reflects the
    // mitigated utilization (below hard, at most checkpoint band).
    bool hard_memory_drop = false;
    for (const auto &audit : prepared.value().item_audit) {
        if (audit.reason == "watermark_hard_drop_memory") {
            hard_memory_drop = true;
        }
    }
    MIRA_CHECK(hard_memory_drop);
    MIRA_CHECK(prepared.value().budget.utilization < 1.0);
    if (prepared.value().budget.watermark == ContextWatermark::Hard) {
        MIRA_CHECK(prepared.value().budget.utilization >= 0.95);
    }
    for (const auto &audit : prepared.value().item_audit) {
        if (audit.kind == ContextItemKind::SystemPolicy || audit.kind == ContextItemKind::Goal ||
            audit.kind == ContextItemKind::UncertainSideEffect) {
            MIRA_CHECK(audit.disposition == ContextItemDisposition::Selected);
        }
    }
    bool memory_dropped = false;
    for (const auto &audit : prepared.value().item_audit) {
        if (audit.kind == ContextItemKind::RetrievedMemory &&
            audit.disposition == ContextItemDisposition::Dropped) {
            memory_dropped = true;
        }
    }
    MIRA_CHECK(memory_dropped);
    return 0;
}

int tool_pairs_stay_atomic_and_pending_results_survive() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());

    auto request = base_request();
    // Tight budget: the consumed pair cannot fit with inline payloads.
    request.limits = roomy_limits(900);
    request.limits.reserved_output_tokens = 100;
    request.limits.safety_margin_tokens = 100;

    auto pending_call = text_item(ContextItemKind::ToolCall, ContextAuthority::VerifiedState,
                                  R"({"tool":"read_screen"})", 10);
    pending_call.tool_call_key = "call-1";
    const ContextItemId pending_call_id = pending_call.id;
    auto pending_result = text_item(ContextItemKind::ToolResult, ContextAuthority::VerifiedState,
                                    std::string(400, 'r'), 11);
    pending_result.tool_call_key = "call-1";
    pending_result.consumed = false;
    const ContextItemId pending_result_id = pending_result.id;
    auto consumed_call = text_item(ContextItemKind::ToolCall, ContextAuthority::VerifiedState,
                                   R"({"tool":"list_apps"})", 12);
    consumed_call.tool_call_key = "call-2";
    const ContextItemId consumed_call_id = consumed_call.id;
    auto consumed_result = text_item(ContextItemKind::ToolResult, ContextAuthority::VerifiedState,
                                     std::string(400, 's'), 13);
    consumed_result.tool_call_key = "call-2";
    consumed_result.consumed = true;
    consumed_result.replaceable_by_reference = true;
    const ContextItemId consumed_result_id = consumed_result.id;
    request.items.push_back(std::move(pending_call));
    request.items.push_back(std::move(pending_result));
    request.items.push_back(std::move(consumed_call));
    request.items.push_back(std::move(consumed_result));

    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    const auto disposition_of = [&prepared](const ContextItemId &id) {
        for (const auto &audit : prepared.value().item_audit) {
            if (audit.id == id) {
                return audit.disposition;
            }
        }
        return ContextItemDisposition::RejectedMinimumSet;
    };

    // Unconsumed tool results are minimum set: both members survive.
    MIRA_CHECK(disposition_of(pending_call_id) == ContextItemDisposition::Selected);
    MIRA_CHECK(disposition_of(pending_result_id) == ContextItemDisposition::Selected);

    // The consumed pair is decided atomically: both selected (possibly with
    // the result replaced by a reference) or both dropped together.
    const auto call_disposition = disposition_of(consumed_call_id);
    const auto result_disposition = disposition_of(consumed_result_id);
    const bool both_kept = call_disposition == ContextItemDisposition::Selected &&
                           (result_disposition == ContextItemDisposition::Selected ||
                            result_disposition == ContextItemDisposition::SelectedByReference);
    const bool both_dropped = call_disposition == ContextItemDisposition::Dropped &&
                              result_disposition == ContextItemDisposition::Dropped;
    MIRA_CHECK(both_kept || both_dropped);
    if (both_dropped) {
        for (const auto &audit : prepared.value().item_audit) {
            if (audit.id == consumed_call_id || audit.id == consumed_result_id) {
                MIRA_CHECK(audit.reason == "tool_pair_dropped");
            }
        }
    }
    return 0;
}

int orphan_tool_result_breaks_pairing() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    auto orphan = text_item(ContextItemKind::ToolResult, ContextAuthority::VerifiedState,
                            "orphan result", 10);
    orphan.tool_call_key = "call-missing";
    request.items.push_back(std::move(orphan));
    auto prepared = manager.prepare(request);
    MIRA_CHECK(!prepared.has_value());
    MIRA_CHECK(prepared.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::ToolPairingBroken));
    return 0;
}

int stale_epochs_reject_the_build() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    auto stale = text_item(ContextItemKind::RecentAction, ContextAuthority::VerifiedState,
                           "older action", 3);
    stale.task_epoch = 3; // request boundary is epoch 4
    request.items.push_back(std::move(stale));
    auto prepared = manager.prepare(request);
    MIRA_CHECK(!prepared.has_value());
    MIRA_CHECK(prepared.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::StaleBuild));

    auto request_env = base_request();
    auto stale_env = text_item(ContextItemKind::RecentAction, ContextAuthority::VerifiedState,
                               "older action", 3);
    stale_env.environment_epoch = 1; // request boundary is epoch 2
    request_env.items.push_back(std::move(stale_env));
    auto rejected = manager.prepare(request_env);
    MIRA_CHECK(!rejected.has_value());
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(ContextDomainCode::StaleBuild));
    return 0;
}

int image_budget_cap_drops_history_first() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    request.limits.max_image_tokens = 600;
    // Each image costs 256 + 4096/256 = 272 upper-bound tokens; three images
    // total 816 and force the cap to shed history.
    request.items.push_back(image_item(ContextItemKind::HistoricalPayload,
                                       ContextAuthority::VerifiedState, 4'096, 6, "frame a"));
    request.items.push_back(image_item(ContextItemKind::HistoricalPayload,
                                       ContextAuthority::VerifiedState, 4'096, 7, "frame b"));
    request.items.push_back(image_item(ContextItemKind::HistoricalPayload,
                                       ContextAuthority::VerifiedState, 4'096, 8, "frame c"));
    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    std::size_t selected_images = 0;
    std::size_t capped_images = 0;
    for (const auto &audit : prepared.value().item_audit) {
        if (audit.kind != ContextItemKind::HistoricalPayload) {
            continue;
        }
        if (audit.disposition == ContextItemDisposition::Selected) {
            ++selected_images;
        } else if (audit.reason == "image_budget_cap") {
            ++capped_images;
        }
    }
    MIRA_CHECK(selected_images + capped_images == 3);
    MIRA_CHECK(capped_images >= 1);
    MIRA_CHECK(selected_images * 272 <= 600);
    return 0;
}

int tool_schema_budget_drops_beyond_cap() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    request.limits.max_tool_schema_tokens = 150;
    for (int index = 0; index < 4; ++index) {
        ExposedToolSpec tool;
        tool.tool_id = ToolId::generate();
        tool.wire_name = "tool_" + std::to_string(index);
        tool.description = std::string(120, 'd');
        tool.parameters_schema = JsonSchema{
            parse_json(R"({"type":"object","properties":{"q":{"type":"string"}}})").value()};
        request.tools.push_back(std::move(tool));
    }
    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    std::size_t selected = 0;
    std::size_t dropped = 0;
    for (const auto &audit : prepared.value().tool_audit) {
        if (audit.selected) {
            ++selected;
        } else {
            ++dropped;
            MIRA_CHECK(audit.reason == "tool_schema_budget");
        }
    }
    MIRA_CHECK(selected >= 1);
    MIRA_CHECK(dropped >= 1);
    MIRA_CHECK(prepared.value().tools.size() == selected);
    return 0;
}

int watermark_trim_replaces_payloads_by_reference() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    // Enough history to land in the trim band but not the checkpoint band.
    request.limits = roomy_limits(4'000);
    request.limits.reserved_output_tokens = 100;
    request.limits.safety_margin_tokens = 100;
    for (std::uint64_t index = 0; index < 24; ++index) {
        auto history = text_item(ContextItemKind::HistoricalPayload,
                                 ContextAuthority::VerifiedState, std::string(400, 'h'), index);
        history.replaceable_by_reference = true;
        ArtifactRef reference;
        reference.id = ArtifactId::generate();
        reference.byte_size = 128;
        history.payload = reference;
        request.items.push_back(std::move(history));
    }
    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    MIRA_CHECK(prepared.value().budget.replaced_items > 0);
    MIRA_CHECK(prepared.value().budget.utilization < 0.7);
    for (const auto &audit : prepared.value().item_audit) {
        if (audit.disposition == ContextItemDisposition::SelectedByReference) {
            MIRA_CHECK(audit.reason == "watermark_trim");
        }
    }
    // Every input item is still accounted for exactly once.
    MIRA_CHECK(prepared.value().item_audit.size() == request.items.size());
    return 0;
}

int audit_reasons_are_stable_codes() {
    StandardContextManager manager(std::make_shared<ConservativeTokenCounter>());
    auto request = base_request();
    auto prepared = manager.prepare(request);
    MIRA_CHECK(prepared.has_value());
    for (const auto &audit : prepared.value().item_audit) {
        MIRA_CHECK(!audit.reason.empty());
        MIRA_CHECK(audit.reason.find(' ') == std::string::npos);
    }
    return 0;
}

} // namespace

int main() {
    if (counter_estimates_are_conservative_and_profile_isolated() != 0) {
        return 1;
    }
    if (exact_count_success_and_degradation() != 0) {
        return 1;
    }
    if (manager_selects_everything_with_room() != 0) {
        return 1;
    }
    if (minimum_set_overflow_rejects_then_routes() != 0) {
        return 1;
    }
    if (pinned_items_survive_hard_watermark() != 0) {
        return 1;
    }
    if (tool_pairs_stay_atomic_and_pending_results_survive() != 0) {
        return 1;
    }
    if (orphan_tool_result_breaks_pairing() != 0) {
        return 1;
    }
    if (stale_epochs_reject_the_build() != 0) {
        return 1;
    }
    if (image_budget_cap_drops_history_first() != 0) {
        return 1;
    }
    if (tool_schema_budget_drops_beyond_cap() != 0) {
        return 1;
    }
    if (watermark_trim_replaces_payloads_by_reference() != 0) {
        return 1;
    }
    if (audit_reasons_are_stable_codes() != 0) {
        return 1;
    }
    return 0;
}
