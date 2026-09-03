#include <mira/context_manager.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace mira {

namespace {

[[nodiscard]] Error manager_error(ContextDomainCode code, std::string message) {
    Error error;
    error.domain = "mira.context";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = std::move(message);
    switch (code) {
    case ContextDomainCode::MinimumSetTooLarge:
        error.code = ErrorCode::ResourceExhausted;
        break;
    case ContextDomainCode::StaleBuild:
        error.code = ErrorCode::InvalidState;
        break;
    case ContextDomainCode::InvalidItem:
    case ContextDomainCode::InvalidLimits:
    case ContextDomainCode::ToolPairingBroken:
        error.code = ErrorCode::InvalidArgument;
        break;
    case ContextDomainCode::TokenCountUnavailable:
        error.code = ErrorCode::Unavailable;
        break;
    default:
        error.code = ErrorCode::Internal;
        break;
    }
    return error;
}

[[nodiscard]] std::uint64_t divide_up(std::uint64_t value, std::uint64_t unit) noexcept {
    return unit == 0 ? value : (value + unit - 1) / unit;
}

// Stable audit reason codes; these are contract surface, not free text.
constexpr const char *kReasonMinimumSet = "minimum_set";
constexpr const char *kReasonPriorityFit = "priority_fit";
constexpr const char *kReasonBudgetExhausted = "budget_exhausted";
constexpr const char *kReasonBudgetReplace = "budget_replace_by_reference";
constexpr const char *kReasonPairDropped = "tool_pair_dropped";
constexpr const char *kReasonWatermarkTrim = "watermark_trim";
constexpr const char *kReasonWatermarkDrop = "watermark_checkpoint_drop";
constexpr const char *kReasonWatermarkCompress = "watermark_checkpoint_compress";
constexpr const char *kReasonWatermarkHardMemory = "watermark_hard_drop_memory";
constexpr const char *kReasonWatermarkHardState = "watermark_hard_drop_state";
constexpr const char *kReasonImageCap = "image_budget_cap";
constexpr const char *kReasonToolFit = "tool_schema_fit";
constexpr const char *kReasonToolSchemaCap = "tool_schema_budget";
constexpr const char *kReasonMinimumRejected = "minimum_set_rejected";
constexpr const char *kReasonMinimumRouted = "minimum_set_routed_large_window";

[[nodiscard]] std::string reference_text(const ContextItem &item) {
    if (item.payload.has_value()) {
        return "[artifact-ref id=" + item.payload->id.to_string() +
               " digest=" + item.payload->digest.to_string() +
               " bytes=" + std::to_string(item.payload->byte_size) + "]";
    }
    if (!item.provenance.empty()) {
        return "[ref event=" + item.provenance.front().to_string() + "]";
    }
    return "[ref kind=" + context_item_kind_name(item.kind) +
           " sequence=" + std::to_string(item.sequence) + "]";
}

[[nodiscard]] std::string compressed_text(const ContextItem &item) {
    std::string provenance = "none";
    if (!item.provenance.empty()) {
        provenance = item.provenance.front().to_string();
    }
    return "[compressed kind=" + context_item_kind_name(item.kind) +
           " sequence=" + std::to_string(item.sequence) + " event=" + provenance + "]";
}

[[nodiscard]] TextPart marker_part(std::string text) {
    TextPart part;
    part.text = std::move(text);
    part.sensitivity = Sensitivity::Public;
    return part;
}

[[nodiscard]] ModelRole role_for(const ContextItem &item) noexcept {
    switch (item.kind) {
    case ContextItemKind::SystemPolicy:
        return ModelRole::System;
    case ContextItemKind::ToolCall:
        return ModelRole::Assistant;
    default:
        return ModelRole::User;
    }
}

// The ModelInputItem authority field carries a trust tier, mirroring the
// agent loop convention: trusted runtime assembly is Internal, external text
// stays on the lowest tier and never gains System/Developer roles.
[[nodiscard]] Sensitivity authority_tier(const ContextItem &item) noexcept {
    switch (item.authority) {
    case ContextAuthority::SystemPolicy:
    case ContextAuthority::UserConstraint:
    case ContextAuthority::VerifiedState:
        return Sensitivity::Internal;
    case ContextAuthority::RetrievedMemory:
    case ContextAuthority::UntrustedExternalData:
        return Sensitivity::Public;
    }
    return Sensitivity::Public;
}

[[nodiscard]] std::string provenance_source_for(const ContextItem &item) {
    return "mira.context." + context_authority_name(item.authority) + ".v1";
}

// Per-item build state. `tokens` always reflects the current representation:
// original content, reference marker or compressed marker.
struct ItemEval final {
    std::size_t index = 0;
    ContextItem item;
    std::uint64_t tokens = 0;
    std::uint64_t image_tokens = 0;
    TokenCountQuality quality = TokenCountQuality::ConservativeEstimate;
    bool minimum = false;
    bool selected = false;
    bool replaced = false;
    bool compressed = false;
    std::string reason;
};

[[nodiscard]] std::size_t part_count_of(const ContextItem &item) noexcept {
    return item.content.size();
}

[[nodiscard]] ContextPartition partition_index(ContextItemKind kind) noexcept {
    return partition_of(kind);
}

} // namespace

// ---------------------------------------------------------------------------
// ConservativeTokenCounter
// ---------------------------------------------------------------------------

ConservativeTokenCounter::ConservativeTokenCounter(ConservativeTokenConfig config)
    : config_(config) {}

void ConservativeTokenCounter::attach_exact_counter(
    std::shared_ptr<const IExactTokenCounter> exact) {
    std::lock_guard lock(mutex_);
    exact_ = std::move(exact);
}

void ConservativeTokenCounter::record_usage(const ModelProfileId &profile,
                                            std::uint64_t estimated_upper,
                                            std::uint64_t actual_tokens) {
    if (estimated_upper == 0) {
        return;
    }
    const double ratio = static_cast<double>(actual_tokens) / static_cast<double>(estimated_upper);
    std::lock_guard lock(mutex_);
    auto &margin = margins_[profile];
    const double previous = margin > 0.0 ? margin : 1.0;
    // Exponential reconciliation toward the observed ratio; margins never drop
    // below 1.0 so a previously safe bound can only grow safer.
    margin = std::max(1.0, 0.9 * previous + 0.1 * std::max(0.0, ratio));
}

std::optional<double> ConservativeTokenCounter::margin_for(const ModelProfileId &profile) const {
    std::lock_guard lock(mutex_);
    const auto found = margins_.find(profile);
    if (found == margins_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::uint32_t ConservativeTokenCounter::exact_count_degradations() const {
    std::lock_guard lock(mutex_);
    return degradations_;
}

TokenEstimate ConservativeTokenCounter::finalize(std::uint64_t lower, std::uint64_t upper,
                                                 const ModelProfileId &profile) const {
    double margin = 1.0;
    {
        std::lock_guard lock(mutex_);
        const auto found = margins_.find(profile);
        if (found != margins_.end()) {
            margin = found->second;
        }
    }
    TokenEstimate estimate;
    estimate.lower_bound = lower;
    estimate.upper_bound =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(upper) * margin));
    estimate.profile_id = profile;
    return estimate;
}

std::uint64_t ConservativeTokenCounter::image_tokens_upper(const ImagePart &image,
                                                           const ModelProfileId &) const {
    return config_.image_floor_tokens +
           divide_up(image.source.byte_size, config_.image_bytes_per_token);
}

Result<TokenEstimate> ConservativeTokenCounter::estimate_item(const ContextItem &item,
                                                              const ModelProfileId &profile) const {
    std::uint64_t text_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t parts = 0;
    std::uint64_t image_tokens = 0;
    for (const auto &part : item.content) {
        ++parts;
        if (const auto *text = std::get_if<TextPart>(&part)) {
            text_bytes += text->text.size();
        } else if (const auto *image = std::get_if<ImagePart>(&part)) {
            image_tokens += config_.image_floor_tokens +
                            divide_up(image->source.byte_size, config_.image_bytes_per_token);
        } else if (const auto *file = std::get_if<FilePart>(&part)) {
            file_bytes += file->source.byte_size;
        }
    }
    const std::uint64_t text_tokens = divide_up(text_bytes, config_.bytes_per_token);
    const std::uint64_t file_tokens =
        file_bytes > 0 ? 32 + divide_up(file_bytes, config_.bytes_per_token) : 0;
    const std::uint64_t overhead =
        parts * config_.per_part_overhead_tokens + config_.per_item_overhead_tokens;
    const std::uint64_t upper = text_tokens + image_tokens + file_tokens + overhead;
    const std::uint64_t lower = upper / 4 + 1;

    std::shared_ptr<const IExactTokenCounter> exact;
    {
        std::lock_guard lock(mutex_);
        exact = exact_;
    }
    if (exact != nullptr) {
        const auto counted = exact->exact_count(item, profile);
        if (counted) {
            TokenEstimate estimate;
            estimate.lower_bound = counted.value();
            estimate.upper_bound = counted.value();
            estimate.quality = TokenCountQuality::ExactProviderCount;
            estimate.profile_id = profile;
            return estimate;
        }
        {
            std::lock_guard lock(mutex_);
            ++degradations_;
        }
        TokenEstimate estimate = finalize(lower, upper, profile);
        estimate.quality = TokenCountQuality::DegradedEstimate;
        return estimate;
    }
    return finalize(lower, upper, profile);
}

Result<TokenEstimate> ConservativeTokenCounter::estimate_tool(const ExposedToolSpec &tool,
                                                              const ModelProfileId &profile) const {
    const std::string schema = to_json_string(tool.parameters_schema.root);
    const std::uint64_t bytes = tool.wire_name.size() + tool.description.size() + schema.size();
    const std::uint64_t upper =
        divide_up(bytes, config_.bytes_per_token) + config_.tool_schema_overhead_tokens;
    return finalize(upper / 4 + 1, upper, profile);
}

std::uint64_t ConservativeTokenCounter::estimate_assembly_overhead(std::size_t item_count,
                                                                   std::size_t part_count) const {
    return config_.per_request_overhead_tokens +
           2ULL * (static_cast<std::uint64_t>(item_count) + static_cast<std::uint64_t>(part_count));
}

// ---------------------------------------------------------------------------
// StandardContextManager
// ---------------------------------------------------------------------------

StandardContextManager::StandardContextManager(std::shared_ptr<const ITokenCounter> counter)
    : counter_(std::move(counter)) {}

namespace {

[[nodiscard]] std::uint64_t assembly_increment(const ItemEval &eval, const ITokenCounter &counter) {
    return counter.estimate_assembly_overhead(1, part_count_of(eval.item)) -
           counter.estimate_assembly_overhead(0, 0);
}

// Re-estimates an item whose content was replaced by a marker; on counter
// failure the caller's current token bound is kept (never zero).
[[nodiscard]] std::uint64_t marker_tokens(const ITokenCounter &counter, const ContextItem &item,
                                          const ModelProfileId &profile, std::string marker,
                                          std::uint64_t fallback) {
    ContextItem probe = item;
    probe.content.clear();
    probe.content.emplace_back(marker_part(std::move(marker)));
    auto estimate = counter.estimate_item(probe, profile);
    return estimate ? estimate.value().upper_bound : fallback;
}

} // namespace

Result<PreparedModelContext> StandardContextManager::prepare(const ContextRequest &request) const {
    if (counter_ == nullptr) {
        return manager_error(ContextDomainCode::TokenCountUnavailable, "token counter is required");
    }
    const auto valid = request.validate();
    if (!valid) {
        return valid.error();
    }

    // Phase 0: epoch consistency. Items produced under other epochs make the
    // whole build stale; silently mixing them is never acceptable.
    for (const auto &item : request.items) {
        if (item.task_epoch.has_value() && *item.task_epoch != request.task_epoch) {
            return manager_error(ContextDomainCode::StaleBuild, "item task epoch mismatch");
        }
        if (item.environment_epoch.has_value() &&
            *item.environment_epoch != request.environment_epoch) {
            return manager_error(ContextDomainCode::StaleBuild, "item environment epoch mismatch");
        }
    }

    // Phase 1: estimates, image budget and tool pairing validation.
    std::vector<ItemEval> evals;
    evals.reserve(request.items.size());
    std::map<std::string, std::vector<std::size_t>> calls;
    std::map<std::string, std::vector<std::size_t>> results;
    for (std::size_t index = 0; index < request.items.size(); ++index) {
        ItemEval eval;
        eval.index = index;
        eval.item = request.items[index];
        auto estimate = counter_->estimate_item(eval.item, request.profile_id);
        if (!estimate) {
            return estimate.error();
        }
        eval.tokens = estimate.value().upper_bound;
        eval.quality = estimate.value().quality;
        for (const auto &part : eval.item.content) {
            if (const auto *image = std::get_if<ImagePart>(&part); image != nullptr) {
                eval.image_tokens += counter_->image_tokens_upper(*image, request.profile_id);
            }
        }
        evals.push_back(std::move(eval));
        const auto &kind = request.items[index].kind;
        if (kind == ContextItemKind::ToolCall) {
            calls[*request.items[index].tool_call_key].push_back(index);
        } else if (kind == ContextItemKind::ToolResult) {
            results[*request.items[index].tool_call_key].push_back(index);
        }
    }
    for (const auto &[key, result_indices] : results) {
        if (calls.find(key) == calls.end()) {
            return manager_error(ContextDomainCode::ToolPairingBroken,
                                 "tool result has no matching tool call");
        }
    }
    for (auto &eval : evals) {
        // P0/P1, pinned items, the current observation, uncertain side
        // effects, pending tool results and the calls paired with them form
        // the minimum executable set and can never be trimmed away.
        const auto kind = eval.item.kind;
        const bool in_frame = partition_index(kind) == ContextPartition::P0Policy ||
                              partition_index(kind) == ContextPartition::P1TaskFrame;
        bool paired_pending = false;
        if (kind == ContextItemKind::ToolCall) {
            const auto found = results.find(*eval.item.tool_call_key);
            paired_pending = found != results.end() &&
                             std::any_of(found->second.begin(), found->second.end(),
                                         [&request](std::size_t result_index) {
                                             return !request.items[result_index].consumed;
                                         });
        }
        eval.minimum = in_frame || eval.item.pinned || eval.item.is_tool_result_pending() ||
                       kind == ContextItemKind::CurrentObservation ||
                       kind == ContextItemKind::UncertainSideEffect || paired_pending;
    }

    // Phase 2: minimum-set admission, optionally on the explicitly authorized
    // large-window profile. Routing is admission-driven only.
    const ContextLimits *limits = &request.limits;
    bool routed = false;
    std::uint64_t min_tokens = 0;
    for (const auto &eval : evals) {
        if (eval.minimum) {
            min_tokens += eval.tokens + assembly_increment(eval, *counter_);
        }
    }
    if (min_tokens > request.limits.input_budget_tokens() &&
        request.authorized_large_window_profile.has_value()) {
        const std::uint64_t large = request.authorized_large_window_limits.input_budget_tokens();
        if (min_tokens <= large) {
            limits = &request.authorized_large_window_limits;
            routed = true;
        }
    }
    const std::uint64_t budget = limits->input_budget_tokens();
    if (min_tokens > budget) {
        PreparedModelContext rejected;
        rejected.task_id = request.task_id;
        rejected.session_id = request.session_id;
        rejected.task_epoch = request.task_epoch;
        rejected.environment_epoch = request.environment_epoch;
        rejected.through_event_sequence = request.through_event_sequence;
        rejected.profile_id = request.profile_id;
        rejected.budget.input_budget_tokens = budget;
        rejected.budget.estimated_tokens = min_tokens;
        rejected.item_audit.reserve(evals.size());
        for (const auto &eval : evals) {
            rejected.item_audit.push_back(ContextItemAudit{
                eval.item.id, eval.item.kind, ContextItemDisposition::RejectedMinimumSet,
                kReasonMinimumRejected, eval.tokens});
        }
        (void)rejected;
        return manager_error(ContextDomainCode::MinimumSetTooLarge,
                             "minimum context set exceeds the input budget");
    }

    // Phase 3: tool schema selection within the tool cap and the remaining
    // budget after the minimum set.
    std::vector<std::pair<std::size_t, std::uint64_t>> tool_estimates;
    tool_estimates.reserve(request.tools.size());
    for (std::size_t index = 0; index < request.tools.size(); ++index) {
        auto estimate = counter_->estimate_tool(request.tools[index], request.profile_id);
        if (!estimate) {
            return estimate.error();
        }
        tool_estimates.emplace_back(index, estimate.value().upper_bound);
    }
    const std::uint64_t tool_room = budget > min_tokens ? budget - min_tokens : 0;
    const std::uint64_t tool_cap =
        std::min<std::uint64_t>(limits->max_tool_schema_tokens, tool_room);
    std::vector<bool> tool_selected(request.tools.size(), false);
    std::uint64_t tool_tokens = 0;
    for (const auto &[index, tokens] : tool_estimates) {
        if (tool_tokens + tokens <= tool_cap) {
            tool_selected[index] = true;
            tool_tokens += tokens;
        }
    }

    // Phase 4: greedy selection of non-minimum items in partition priority
    // order. P2/P3/P5 consider newest sequences first, P4 keeps caller rank.
    std::vector<std::size_t> selection_order;
    selection_order.reserve(evals.size());
    for (std::size_t pass = 0; pass < 6; ++pass) {
        std::vector<std::size_t> partition_items;
        for (std::size_t index = 0; index < evals.size(); ++index) {
            if (static_cast<std::uint8_t>(partition_index(evals[index].item.kind)) == pass) {
                partition_items.push_back(index);
            }
        }
        const bool descending = pass != 4;
        std::stable_sort(partition_items.begin(), partition_items.end(),
                         [&evals, descending](std::size_t lhs, std::size_t rhs) {
                             if (evals[lhs].item.sequence != evals[rhs].item.sequence) {
                                 const bool less =
                                     evals[lhs].item.sequence < evals[rhs].item.sequence;
                                 return descending ? !less : less;
                             }
                             return lhs < rhs;
                         });
        selection_order.insert(selection_order.end(), partition_items.begin(),
                               partition_items.end());
    }

    std::uint64_t running = min_tokens + tool_tokens;
    std::set<std::size_t> selected_groups;
    for (const auto index : selection_order) {
        auto &eval = evals[index];
        if (eval.minimum) {
            eval.selected = true;
            eval.reason = kReasonMinimumSet;
            continue;
        }
        const auto kind = eval.item.kind;
        if (kind == ContextItemKind::ToolCall) {
            // A consumed call/result pair is selected or dropped atomically.
            const auto key = *eval.item.tool_call_key;
            if (selected_groups.count(index) != 0) {
                continue;
            }
            std::vector<std::size_t> group{index};
            const auto found = results.find(key);
            if (found != results.end()) {
                for (const auto result_index : found->second) {
                    if (!evals[result_index].minimum) {
                        group.push_back(result_index);
                        selected_groups.insert(result_index);
                    }
                }
            }
            std::uint64_t group_tokens = 0;
            for (const auto member : group) {
                group_tokens += evals[member].tokens + assembly_increment(evals[member], *counter_);
            }
            if (running + group_tokens <= budget) {
                for (const auto member : group) {
                    evals[member].selected = true;
                    evals[member].reason = kReasonPriorityFit;
                    running += evals[member].tokens + assembly_increment(evals[member], *counter_);
                }
                continue;
            }
            // Try replacing replaceable results with references.
            std::uint64_t ref_tokens = 0;
            const bool replaceable =
                std::all_of(group.begin(), group.end(), [&](std::size_t member) {
                    return evals[member].item.kind != ContextItemKind::ToolResult ||
                           evals[member].item.replaceable_by_reference;
                });
            if (replaceable) {
                for (const auto member : group) {
                    if (evals[member].item.kind == ContextItemKind::ToolResult) {
                        ref_tokens +=
                            marker_tokens(*counter_, evals[member].item, request.profile_id,
                                          reference_text(evals[member].item),
                                          evals[member].tokens) +
                            assembly_increment(evals[member], *counter_);
                    } else {
                        ref_tokens +=
                            evals[member].tokens + assembly_increment(evals[member], *counter_);
                    }
                }
            }
            if (replaceable && running + ref_tokens <= budget) {
                for (const auto member : group) {
                    evals[member].selected = true;
                    if (evals[member].item.kind == ContextItemKind::ToolResult) {
                        evals[member].replaced = true;
                    }
                    evals[member].reason = kReasonBudgetReplace;
                    if (evals[member].item.kind == ContextItemKind::ToolResult) {
                        evals[member].tokens =
                            marker_tokens(*counter_, evals[member].item, request.profile_id,
                                          reference_text(evals[member].item), evals[member].tokens);
                    }
                    evals[member].image_tokens = 0;
                    running += evals[member].tokens + assembly_increment(evals[member], *counter_);
                }
                continue;
            }
            for (const auto member : group) {
                evals[member].selected = false;
                evals[member].reason = kReasonPairDropped;
            }
            continue;
        }
        if (kind == ContextItemKind::ToolResult) {
            // Results whose call was handled above are already decided.
            if (selected_groups.count(index) != 0) {
                continue;
            }
            const auto call_found = calls.find(*eval.item.tool_call_key);
            if (call_found != calls.end() && !evals[call_found->second.front()].minimum &&
                !evals[call_found->second.front()].selected) {
                eval.reason = kReasonPairDropped;
                continue;
            }
        }
        const std::uint64_t increment = eval.tokens + assembly_increment(eval, *counter_);
        if (running + increment <= budget) {
            eval.selected = true;
            eval.reason = kReasonPriorityFit;
            running += increment;
        } else {
            eval.selected = false;
            eval.reason = kReasonBudgetExhausted;
        }
    }

    // Image budget cap: drop the oldest non-minimum image-bearing items first.
    const auto image_over_cap = [&]() {
        std::uint64_t total = 0;
        for (const auto &eval : evals) {
            if (eval.selected) {
                total += eval.image_tokens;
            }
        }
        return total > limits->max_image_tokens;
    };
    while (image_over_cap()) {
        std::optional<std::size_t> victim;
        for (std::size_t index = 0; index < evals.size(); ++index) {
            const auto &eval = evals[index];
            if (eval.selected && !eval.minimum && eval.image_tokens > 0 &&
                (victim == std::nullopt || eval.item.sequence < evals[*victim].item.sequence)) {
                victim = index;
            }
        }
        if (victim == std::nullopt) {
            return manager_error(ContextDomainCode::MinimumSetTooLarge,
                                 "minimum image evidence exceeds the image budget");
        }
        auto &eval = evals[*victim];
        eval.selected = false;
        eval.reason = kReasonImageCap;
        running -= eval.tokens + assembly_increment(eval, *counter_);
    }

    // Phase 5: watermark passes over the selected set.
    const auto utilization = [&]() {
        return budget == 0 ? 0.0 : static_cast<double>(running) / static_cast<double>(budget);
    };
    const auto oldest_selected = [&](auto predicate) -> std::optional<std::size_t> {
        std::optional<std::size_t> victim;
        for (std::size_t index = 0; index < evals.size(); ++index) {
            const auto &eval = evals[index];
            if (!eval.selected || eval.minimum || !predicate(eval)) {
                continue;
            }
            if (victim == std::nullopt || eval.item.sequence < evals[*victim].item.sequence) {
                victim = index;
            }
        }
        return victim;
    };
    const auto replaceable_history = [](const ItemEval &eval) {
        const auto partition = partition_index(eval.item.kind);
        return (partition == ContextPartition::P5History ||
                partition == ContextPartition::P3Progress) &&
               eval.item.replaceable_by_reference && !eval.replaced && !eval.compressed;
    };

    while (utilization() >= limits->trim_watermark) {
        const auto victim = oldest_selected(replaceable_history);
        if (victim == std::nullopt) {
            break;
        }
        auto &eval = evals[*victim];
        const std::uint64_t replacement = marker_tokens(*counter_, eval.item, request.profile_id,
                                                        reference_text(eval.item), eval.tokens);
        // Replacing a payload only helps when the reference is smaller; tiny
        // items keep their inline content.
        if (replacement >= eval.tokens) {
            eval.item.replaceable_by_reference = false;
            continue;
        }
        running -= eval.tokens - replacement;
        eval.tokens = replacement;
        eval.replaced = true;
        eval.reason = kReasonWatermarkTrim;
        eval.image_tokens = 0;
    }

    bool checkpoint_recommended = false;
    if (utilization() >= limits->checkpoint_watermark) {
        checkpoint_recommended = true;
        while (utilization() >= limits->checkpoint_watermark) {
            const auto victim = oldest_selected([](const ItemEval &eval) {
                return partition_index(eval.item.kind) == ContextPartition::P5History;
            });
            if (victim == std::nullopt) {
                break;
            }
            auto &eval = evals[*victim];
            eval.selected = false;
            eval.reason = kReasonWatermarkDrop;
            running -= eval.tokens + assembly_increment(eval, *counter_);
        }
        while (utilization() >= limits->checkpoint_watermark) {
            const auto victim = oldest_selected([](const ItemEval &eval) {
                const auto partition = partition_index(eval.item.kind);
                return partition == ContextPartition::P3Progress && !eval.compressed;
            });
            if (victim == std::nullopt) {
                break;
            }
            auto &eval = evals[*victim];
            const std::uint64_t compressed =
                marker_tokens(*counter_, eval.item, request.profile_id,
                              compressed_text(eval.item), eval.tokens);
            running -= eval.tokens > compressed ? eval.tokens - compressed : 0;
            eval.tokens = compressed;
            eval.compressed = true;
            eval.reason = kReasonWatermarkCompress;
        }
    }

    if (utilization() >= limits->hard_watermark) {
        while (utilization() >= limits->hard_watermark) {
            const auto victim = oldest_selected([](const ItemEval &eval) {
                return partition_index(eval.item.kind) == ContextPartition::P4Memory;
            });
            if (victim == std::nullopt) {
                break;
            }
            auto &eval = evals[*victim];
            eval.selected = false;
            eval.reason = kReasonWatermarkHardMemory;
            running -= eval.tokens + assembly_increment(eval, *counter_);
        }
        while (utilization() >= limits->hard_watermark) {
            const auto victim = oldest_selected([](const ItemEval &eval) {
                return partition_index(eval.item.kind) == ContextPartition::P2State &&
                       eval.item.kind == ContextItemKind::VerificationResult;
            });
            if (victim == std::nullopt) {
                break;
            }
            auto &eval = evals[*victim];
            eval.selected = false;
            eval.reason = kReasonWatermarkHardState;
            running -= eval.tokens + assembly_increment(eval, *counter_);
        }
    }

    ContextWatermark watermark = ContextWatermark::Normal;
    const double final_utilization = utilization();
    if (final_utilization >= limits->hard_watermark) {
        watermark = ContextWatermark::Hard;
    } else if (final_utilization >= limits->checkpoint_watermark) {
        watermark = ContextWatermark::Checkpoint;
    } else if (final_utilization >= limits->trim_watermark) {
        watermark = ContextWatermark::Trim;
    }

    // Phase 6: assembly. Selected items keep partition order, sequence order.
    std::vector<std::size_t> assembly_order;
    for (std::size_t index = 0; index < evals.size(); ++index) {
        if (evals[index].selected) {
            assembly_order.push_back(index);
        }
    }
    std::stable_sort(assembly_order.begin(), assembly_order.end(),
                     [&](std::size_t lhs, std::size_t rhs) {
                         const auto lhs_partition =
                             static_cast<std::uint8_t>(partition_index(evals[lhs].item.kind));
                         const auto rhs_partition =
                             static_cast<std::uint8_t>(partition_index(evals[rhs].item.kind));
                         if (lhs_partition != rhs_partition) {
                             return lhs_partition < rhs_partition;
                         }
                         if (evals[lhs].item.sequence != evals[rhs].item.sequence) {
                             return evals[lhs].item.sequence < evals[rhs].item.sequence;
                         }
                         return lhs < rhs;
                     });

    PreparedModelContext prepared;
    prepared.task_id = request.task_id;
    prepared.session_id = request.session_id;
    prepared.task_epoch = request.task_epoch;
    prepared.environment_epoch = request.environment_epoch;
    prepared.through_event_sequence = request.through_event_sequence;
    prepared.profile_id = routed ? *request.authorized_large_window_profile : request.profile_id;

    prepared.input.reserve(assembly_order.size());
    for (const auto index : assembly_order) {
        const auto &eval = evals[index];
        ModelInputItem input_item;
        input_item.role = role_for(eval.item);
        input_item.authority = authority_tier(eval.item);
        input_item.provenance.source = provenance_source_for(eval.item);
        if (eval.replaced) {
            input_item.content.emplace_back(marker_part(reference_text(eval.item)));
        } else if (eval.compressed) {
            input_item.content.emplace_back(marker_part(compressed_text(eval.item)));
        } else {
            input_item.content = eval.item.content;
        }
        prepared.input.push_back(std::move(input_item));
    }

    for (std::size_t index = 0; index < request.tools.size(); ++index) {
        if (tool_selected[index]) {
            prepared.tools.push_back(request.tools[index]);
        }
    }

    std::uint64_t lower_sum = 0;
    TokenCountQuality quality = TokenCountQuality::ExactProviderCount;
    bool any_selected = false;
    for (const auto &eval : evals) {
        if (eval.selected) {
            any_selected = true;
            lower_sum += eval.tokens / 4;
            if (eval.quality == TokenCountQuality::DegradedEstimate) {
                quality = TokenCountQuality::DegradedEstimate;
            } else if (eval.quality == TokenCountQuality::ConservativeEstimate &&
                       quality == TokenCountQuality::ExactProviderCount) {
                quality = TokenCountQuality::ConservativeEstimate;
            }
        }
    }
    if (!any_selected) {
        quality = TokenCountQuality::ConservativeEstimate;
    }

    prepared.budget.input_budget_tokens = budget;
    prepared.budget.estimated_tokens = running;
    prepared.budget.quality = quality;
    prepared.budget.utilization = final_utilization;
    prepared.budget.watermark = watermark;
    prepared.budget.routed_to_large_window = routed;
    prepared.budget.routed_profile =
        routed ? request.authorized_large_window_profile : std::nullopt;
    prepared.budget.checkpoint_recommended = checkpoint_recommended;
    for (const auto &eval : evals) {
        if (eval.selected) {
            ++prepared.budget.selected_items;
            if (eval.replaced) {
                ++prepared.budget.replaced_items;
            }
            if (eval.compressed) {
                ++prepared.budget.compressed_items;
            }
        } else {
            ++prepared.budget.dropped_items;
        }
    }

    prepared.item_audit.reserve(evals.size());
    for (const auto &eval : evals) {
        ContextItemDisposition disposition = ContextItemDisposition::Dropped;
        if (eval.selected && eval.replaced) {
            disposition = ContextItemDisposition::SelectedByReference;
        } else if (eval.selected && eval.compressed) {
            disposition = ContextItemDisposition::Compressed;
        } else if (eval.selected) {
            disposition = ContextItemDisposition::Selected;
        }
        prepared.item_audit.push_back(ContextItemAudit{
            eval.item.id, eval.item.kind, disposition,
            routed && eval.minimum && eval.selected ? kReasonMinimumRouted : eval.reason,
            eval.tokens});
    }
    for (std::size_t index = 0; index < request.tools.size(); ++index) {
        prepared.tool_audit.push_back(
            ContextToolAudit{request.tools[index].tool_id, tool_selected[index],
                             tool_selected[index] ? kReasonToolFit : kReasonToolSchemaCap,
                             tool_estimates[index].second});
    }

    prepared.total_estimate.lower_bound = lower_sum;
    prepared.total_estimate.upper_bound = running;
    prepared.total_estimate.quality = quality;
    prepared.total_estimate.profile_id = prepared.profile_id;

    JsonValue::Object digest_object;
    digest_object.emplace_back("task_id", prepared.task_id.to_string());
    digest_object.emplace_back("session_id", prepared.session_id.to_string());
    digest_object.emplace_back("task_epoch", static_cast<std::int64_t>(prepared.task_epoch));
    digest_object.emplace_back("environment_epoch",
                               static_cast<std::int64_t>(prepared.environment_epoch));
    digest_object.emplace_back("through_event_sequence",
                               static_cast<std::int64_t>(prepared.through_event_sequence));
    digest_object.emplace_back("profile_id", prepared.profile_id.to_string());
    JsonValue::Array audit_entries;
    for (const auto &audit : prepared.item_audit) {
        audit_entries.emplace_back(
            JsonValue::Object{{"id", audit.id.to_string()},
                              {"disposition", context_item_disposition_name(audit.disposition)},
                              {"reason", audit.reason},
                              {"tokens", static_cast<std::int64_t>(audit.estimated_tokens)}});
    }
    digest_object.emplace_back("audit", JsonValue(std::move(audit_entries)));
    JsonValue::Array tool_entries;
    for (const auto &audit : prepared.tool_audit) {
        tool_entries.emplace_back(JsonValue::Object{
            {"tool_id", audit.tool_id.to_string()},
            {"selected", audit.selected},
        });
    }
    digest_object.emplace_back("tools", JsonValue(std::move(tool_entries)));
    digest_object.emplace_back("budget", prepared.budget.estimated_tokens);
    digest_object.emplace_back("input_budget", prepared.budget.input_budget_tokens);
    prepared.selection_digest = canonical_json_digest(JsonValue(std::move(digest_object)));

    return prepared;
}

} // namespace mira
