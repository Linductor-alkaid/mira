// M16 (DEC-032 Stage A): the long-session Layer 0 baseline harness. Drives
// the existing StandardContextManager over 100/500/1000-turn synthetic
// sessions (context_intelligence_design.md §11 content mix) with the full
// accumulated history re-presented at every sampled request, enforces the
// pre-frozen G1-G6 gates from docs/plans/m16-context-intelligence-stage-a.md
// §4 and prints a JSON baseline report (stdout, plus argv[1] when given).
// Any gate failure exits non-zero so CI treats the baseline as a regression
// gate.
//
// No model, no new contract, no async work: the benchmark is a deterministic
// single-threaded pure-function driver (fixed splitmix64 seed, counter-derived
// Id128 values, no clock, no threads). Token figures are
// ConservativeTokenCounter upper bounds, not provider counts; conclusions are
// limited to trends and relative structure (RULE-10).

#include <mira/context_manager.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (milestone §4; dataset_digest is taken over this)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t context_window_tokens = 48'000;
    std::uint64_t reserved_output_tokens = 8'192;
    std::uint64_t safety_margin_tokens = 1'024;
    std::uint64_t steady_floor_tokens = 19'392;     // 0.50 x input budget
    std::uint64_t checkpoint_bound_tokens = 32'966; // 0.85 x input budget (truncated)
    std::uint64_t warmup_turns = 32;
    std::uint64_t seed = 0x4D49'5241'0031ULL;       // "MIRA"-derived fixed seed
    std::array<std::uint64_t, 3> session_lengths{100, 500, 1000};
    double plateau_ratio_cap = 1.25;                // G3: S_max(1000)/S_max(100)
    std::uint64_t raw_growth_factor = 5;            // G3: R(1000)/R(100) floor
    std::uint64_t image_min_bytes = 150'000;
    std::uint64_t image_max_bytes = 400'000;
    std::uint32_t ui_tree_min_bytes = 2'000;
    std::uint32_t ui_tree_max_bytes = 6'000;
    std::uint32_t user_msg_min_bytes = 300;
    std::uint32_t user_msg_max_bytes = 700;
    std::uint32_t assistant_msg_min_bytes = 400;
    std::uint32_t assistant_msg_max_bytes = 900;
    std::uint32_t tool_result_min_bytes = 200;
    std::uint32_t tool_result_max_bytes = 600;
    std::uint32_t action_min_bytes = 120;
    std::uint32_t action_max_bytes = 260;
    std::uint64_t tool_pair_period = 5;   // ~60% of turns: 3 of every 5
    std::uint64_t workflow_period = 10;
    std::uint64_t recovery_period = 25;
    std::uint64_t constraint_period = 15;
    std::uint64_t memory_period = 20;
    std::uint64_t checkpoint_period = 50;
    std::uint64_t side_effect_lifetime_turns = 3;
};

[[nodiscard]] ContextLimits frozen_limits(const FrozenConfig &config) {
    ContextLimits limits;
    limits.context_window_tokens = config.context_window_tokens;
    limits.reserved_output_tokens = config.reserved_output_tokens;
    limits.safety_margin_tokens = config.safety_margin_tokens;
    limits.provider_overhead_tokens = 0;
    return limits;
}

// ---------------------------------------------------------------------------
// Deterministic primitives
// ---------------------------------------------------------------------------

class SplitMix64 final {
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    std::uint32_t below(std::uint32_t bound) noexcept {
        return static_cast<std::uint32_t>(next() % bound);
    }

  private:
    std::uint64_t state_;
};

[[nodiscard]] Id128 id128_from_counter(std::uint64_t counter) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        bytes[index] = static_cast<std::uint8_t>((counter >> (index * 8U)) & 0xFFU);
    }
    return Id128{bytes};
}

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        text.push_back(kHex[static_cast<unsigned char>(byte) >> 4U]);
        text.push_back(kHex[static_cast<unsigned char>(byte) & 0x0FU]);
    }
    return text;
}

[[nodiscard]] std::string synthetic_text(SplitMix64 &rng, std::uint32_t min_bytes,
                                         std::uint32_t max_bytes, const std::string &prefix) {
    static constexpr const char *kWords[] = {
        "settings",  "brightness", "scroll",   "list",     "item",      "confirm",
        "cancel",    "dialog",     "search",   "result",   "tap",       "input",
        "keyboard",  "rotation",   "screen",   "node",     "button",    "text",
        "toggle",    "panel",      "menu",     "back",     "workflow",  "step",
        "patch",     "recovery",   "lesson",   "episode",  "observation"};
    static constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
    const std::uint32_t target = min_bytes + rng.below(max_bytes - min_bytes + 1);
    std::string text = prefix;
    text.reserve(static_cast<std::size_t>(target) + 16U);
    while (text.size() < target) {
        text.append(kWords[rng.below(static_cast<std::uint32_t>(kWordCount))]);
        text.push_back(' ');
    }
    text.resize(target);
    return text;
}

[[nodiscard]] TextPart public_text(std::string text) {
    TextPart part;
    part.text = std::move(text);
    part.sensitivity = Sensitivity::Public;
    return part;
}

// ---------------------------------------------------------------------------
// Samples and run results
// ---------------------------------------------------------------------------

struct Sample final {
    std::uint64_t turn = 0;
    std::uint64_t estimated_tokens = 0;
    double utilization = 0.0;
    std::string watermark;
    bool checkpoint_recommended = false;
    std::uint64_t items = 0;
    std::uint64_t selected = 0;
    std::uint64_t replaced = 0;
    std::uint64_t compressed = 0;
    std::uint64_t dropped = 0;
    std::string selection_digest;
};

struct KindMix final {
    std::uint64_t selected = 0;
    std::uint64_t selected_by_reference = 0;
    std::uint64_t compressed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t selected_tokens = 0;
    std::uint64_t dropped_tokens = 0;
};

struct RunResult final {
    std::vector<Sample> samples;
    std::uint64_t final_raw_presented_tokens = 0;
    std::uint64_t final_items = 0;
    std::uint64_t final_constraints = 0;
    std::uint64_t final_min_set_tokens = 0;
    std::map<std::string, KindMix> final_mix;
    std::vector<std::string> failures; // per-sample audit failures (G1/G4/G6)
};

struct SeriesStats final {
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t max = 0;
    std::uint64_t count = 0;
};

[[nodiscard]] SeriesStats steady_stats(const std::vector<Sample> &samples,
                                       std::uint64_t warmup_turns) {
    std::vector<std::uint64_t> tokens;
    for (const auto &sample : samples) {
        if (sample.turn > warmup_turns) {
            tokens.push_back(sample.estimated_tokens);
        }
    }
    SeriesStats stats;
    stats.count = tokens.size();
    if (tokens.empty()) {
        return stats;
    }
    std::sort(tokens.begin(), tokens.end());
    const auto pick = [&tokens](double fraction) {
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(tokens.size() - 1));
        return tokens[std::min(index, tokens.size() - 1)];
    };
    stats.min = tokens.front();
    stats.p50 = pick(0.50);
    stats.p95 = pick(0.95);
    stats.max = tokens.back();
    return stats;
}

[[nodiscard]] std::uint64_t steady_max(const RunResult &run, std::uint64_t warmup_turns) {
    std::uint64_t peak = 0;
    for (const auto &sample : run.samples) {
        if (sample.turn > warmup_turns) {
            peak = std::max(peak, sample.estimated_tokens);
        }
    }
    return peak;
}

// ---------------------------------------------------------------------------
// Long-session driver
// ---------------------------------------------------------------------------

class SessionDriver final {
  public:
    SessionDriver(const FrozenConfig &config, std::uint64_t length,
                  const StandardContextManager &manager,
                  std::shared_ptr<const ConservativeTokenCounter> counter)
        : config_(config), length_(length), manager_(manager), counter_(std::move(counter)),
          rng_(config.seed) {}

    [[nodiscard]] RunResult run();

  private:
    [[nodiscard]] ContextItemId next_item_id() { return ContextItemId{id128_from_counter(++id_counter_)}; }
    [[nodiscard]] EventId next_event_id() { return EventId{id128_from_counter(++event_counter_)}; }
    [[nodiscard]] ArtifactId artifact_id(std::uint64_t counter) const {
        return ArtifactId{id128_from_counter(0xA000'0000ULL + counter)};
    }

    void push_static_frame();
    void begin_turn(std::uint64_t turn);
    void emit_turn_items(std::uint64_t turn);
    void measure(std::uint64_t turn, RunResult &result);
    [[nodiscard]] ContextItem make_history_item(ContextItemKind kind,
                                                ContextAuthority authority, std::string text,
                                                bool replaceable);
    [[nodiscard]] ContextItem make_observation(std::uint64_t turn);
    [[nodiscard]] std::vector<ContextItem>::iterator find_by_id(const ContextItemId &id);

    const FrozenConfig &config_;
    std::uint64_t length_;
    const StandardContextManager &manager_;
    std::shared_ptr<const ConservativeTokenCounter> counter_;
    SplitMix64 rng_;
    ContextLimits limits_ = frozen_limits(config_);
    TaskId task_id_{id128_from_counter(1)};
    SessionId session_id_{id128_from_counter(2)};
    ModelProfileId profile_id_{id128_from_counter(3)};
    std::uint64_t id_counter_ = 100; // strong ids for items; low counters are frame ids
    std::uint64_t event_counter_ = 0;
    std::uint64_t artifact_counter_ = 0;
    std::vector<ContextItem> presented_;
    std::vector<ExposedToolSpec> tools_;
    std::optional<ContextItemId> current_observation_id_;
    std::optional<ContextItemId> pending_result_id_;
    std::vector<std::pair<ContextItemId, std::uint64_t>> side_effects_;
    std::vector<ContextItemId> constraint_ids_;
};

std::vector<ContextItem>::iterator SessionDriver::find_by_id(const ContextItemId &id) {
    return std::find_if(presented_.begin(), presented_.end(),
                        [&id](const ContextItem &item) { return item.id == id; });
}

ContextItem SessionDriver::make_history_item(ContextItemKind kind, ContextAuthority authority,
                                             std::string text, bool replaceable) {
    ContextItem item;
    item.id = next_item_id();
    item.kind = kind;
    item.authority = authority;
    item.sequence = event_counter_;
    item.content.emplace_back(public_text(std::move(text)));
    item.provenance.push_back(next_event_id());
    item.replaceable_by_reference = replaceable;
    item.consumed = true;
    return item;
}

ContextItem SessionDriver::make_observation(std::uint64_t turn) {
    ContextItem item;
    item.id = next_item_id();
    item.kind = ContextItemKind::CurrentObservation;
    item.authority = ContextAuthority::VerifiedState;
    item.sequence = event_counter_;
    const std::uint64_t image_bytes =
        config_.image_min_bytes +
        static_cast<std::uint64_t>(
            rng_.below(static_cast<std::uint32_t>(config_.image_max_bytes - config_.image_min_bytes + 1)));
    ArtifactRef reference;
    reference.id = artifact_id(++artifact_counter_);
    reference.byte_size = image_bytes;
    reference.media_type = "image/png";
    ImagePart image;
    image.source = reference;
    image.media_type = reference.media_type;
    item.content.emplace_back(std::move(image));
    item.content.emplace_back(public_text(synthetic_text(
        rng_, config_.ui_tree_min_bytes, config_.ui_tree_max_bytes,
        "ui-tree turn=" + std::to_string(turn) + " ")));
    item.provenance.push_back(next_event_id());
    item.consumed = true;
    return item;
}

void SessionDriver::push_static_frame() {
    // System policy / goal / task limits form the P0/P1 frame; they are the
    // minimum executable set for the whole session.
    ContextItem policy;
    policy.id = next_item_id();
    policy.kind = ContextItemKind::SystemPolicy;
    policy.authority = ContextAuthority::SystemPolicy;
    policy.sequence = ++event_counter_;
    policy.content.emplace_back(public_text(
        "act only through verified tools; never reveal secrets; confirm "
        "destructive actions with the user"));
    policy.provenance.push_back(next_event_id());
    policy.consumed = true;
    presented_.push_back(std::move(policy));

    presented_.push_back(make_history_item(ContextItemKind::Goal,
                                           ContextAuthority::UserConstraint,
                                           "long-session device task: keep adjusting settings "
                                           "until the user goal is verified",
                                           false));

    presented_.push_back(make_history_item(
        ContextItemKind::TaskLimits, ContextAuthority::VerifiedState,
        "max_steps=unbounded; side_effect_budget=per-action; screenshot_every_turn=on",
        false));

    tools_.resize(4);
    const std::array<const char *, 4> names{"tap", "swipe", "type_text", "read_screen"};
    const std::array<const char *, 4> descriptions{
        "tap the screen at normalized coordinates",
        "swipe between two points with a duration",
        "type text through the platform input provider",
        "capture and describe the current screen"};
    for (std::size_t index = 0; index < tools_.size(); ++index) {
        auto &tool = tools_[index];
        tool.tool_id = ToolId{id128_from_counter(0xB000'0000ULL + index)};
        tool.wire_name = names[index];
        tool.description = descriptions[index];
        JsonValue::Object schema;
        schema.emplace_back("type", "object");
        schema.emplace_back("properties", JsonValue::Array{});
        tool.parameters_schema.root = JsonValue(std::move(schema));
    }
}

void SessionDriver::begin_turn(std::uint64_t turn) {
    // The previous turn's pending tool result has been followed by a model
    // decision; the host marks the pair consumed.
    if (pending_result_id_.has_value()) {
        const auto found = find_by_id(*pending_result_id_);
        if (found != presented_.end()) {
            found->consumed = true;
        }
        pending_result_id_.reset();
    }
    // The previous observation is no longer current: it becomes replaceable
    // history carrying its screenshot artifact by reference.
    if (current_observation_id_.has_value()) {
        const auto found = find_by_id(*current_observation_id_);
        if (found != presented_.end()) {
            found->kind = ContextItemKind::HistoricalPayload;
            found->replaceable_by_reference = true;
            if (const auto *image = std::get_if<ImagePart>(&found->content.front());
                image != nullptr) {
                found->payload = image->source;
            }
        }
        current_observation_id_.reset();
    }
    // Uncertain side effects resolve after their bounded lifetime.
    for (std::size_t index = 0; index < side_effects_.size();) {
        if (turn - side_effects_[index].second > config_.side_effect_lifetime_turns) {
            const auto found = find_by_id(side_effects_[index].first);
            if (found != presented_.end()) {
                presented_.erase(found);
            }
            side_effects_.erase(side_effects_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    // Only the newest checkpoint summary stays presented.
    if (turn % config_.checkpoint_period == 0) {
        presented_.erase(std::remove_if(presented_.begin(), presented_.end(),
                                        [](const ContextItem &item) {
                                            return item.kind == ContextItemKind::CheckpointSummary;
                                        }),
                         presented_.end());
        presented_.push_back(make_history_item(
            ContextItemKind::CheckpointSummary, ContextAuthority::VerifiedState,
            synthetic_text(rng_, 300, 600, "task-checkpoint turn=" + std::to_string(turn) + " "),
            true));
    }
}

void SessionDriver::emit_turn_items(std::uint64_t turn) {
    // User message and assistant reply: conversation history (P5).
    presented_.push_back(make_history_item(
        ContextItemKind::HistoricalPayload, ContextAuthority::UntrustedExternalData,
        synthetic_text(rng_, config_.user_msg_min_bytes, config_.user_msg_max_bytes,
                       "user turn=" + std::to_string(turn) + " "),
        false));
    presented_.push_back(make_history_item(
        ContextItemKind::HistoricalPayload, ContextAuthority::UntrustedExternalData,
        synthetic_text(rng_, config_.assistant_msg_min_bytes, config_.assistant_msg_max_bytes,
                       "assistant turn=" + std::to_string(turn) + " "),
        true));

    // Current observation (P2, minimum while current).
    ContextItem observation = make_observation(turn);
    current_observation_id_ = observation.id;
    presented_.push_back(std::move(observation));

    // Tool pair on ~60% of turns (3 of every 5); the current turn's result
    // is pending.
    if (turn % config_.tool_pair_period >= 2) {
        ContextItem call;
        call.id = next_item_id();
        call.kind = ContextItemKind::ToolCall;
        call.authority = ContextAuthority::VerifiedState;
        call.sequence = event_counter_;
        const std::string key = "call-" + std::to_string(turn);
        call.tool_call_key = key;
        call.content.emplace_back(public_text(
            synthetic_text(rng_, 120, 260, "tool-call turn=" + std::to_string(turn) + " ")));
        call.provenance.push_back(next_event_id());
        call.consumed = true;
        presented_.push_back(std::move(call));

        ContextItem result;
        result.id = next_item_id();
        result.kind = ContextItemKind::ToolResult;
        result.authority = ContextAuthority::UntrustedExternalData;
        result.sequence = event_counter_;
        result.tool_call_key = key;
        result.content.emplace_back(public_text(synthetic_text(
            rng_, config_.tool_result_min_bytes, config_.tool_result_max_bytes,
            "tool-result turn=" + std::to_string(turn) + " ")));
        result.provenance.push_back(next_event_id());
        result.consumed = false;
        result.replaceable_by_reference = true;
        ArtifactRef payload;
        payload.id = artifact_id(++artifact_counter_);
        payload.byte_size = 4'096 + rng_.below(4'096);
        payload.media_type = "application/json";
        result.payload = payload;
        pending_result_id_ = result.id;
        presented_.push_back(std::move(result));
    }

    // Progress: one recent action per turn (P3, compressible).
    presented_.push_back(make_history_item(
        ContextItemKind::RecentAction, ContextAuthority::VerifiedState,
        synthetic_text(rng_, config_.action_min_bytes, config_.action_max_bytes,
                       "action turn=" + std::to_string(turn) + " "),
        true));

    // Workflow run events every 10 turns.
    if (turn % config_.workflow_period == 0) {
        const std::uint32_t events = 1 + rng_.below(2);
        for (std::uint32_t index = 0; index < events; ++index) {
            presented_.push_back(make_history_item(
                ContextItemKind::HistoricalPayload, ContextAuthority::VerifiedState,
                synthetic_text(rng_, 200, 500,
                               "workflow-event turn=" + std::to_string(turn) + " "),
                true));
        }
    }

    // Recovery events every 25 turns: history entry, error and an uncertain
    // side effect with a bounded lifetime.
    if (turn % config_.recovery_period == 0) {
        presented_.push_back(make_history_item(
            ContextItemKind::HistoricalPayload, ContextAuthority::VerifiedState,
            synthetic_text(rng_, 200, 500, "recovery-event turn=" + std::to_string(turn) + " "),
            true));
        presented_.push_back(make_history_item(
            ContextItemKind::RecentError, ContextAuthority::VerifiedState,
            synthetic_text(rng_, 120, 300, "recent-error turn=" + std::to_string(turn) + " "),
            true));
        ContextItem side_effect;
        side_effect.id = next_item_id();
        side_effect.kind = ContextItemKind::UncertainSideEffect;
        side_effect.authority = ContextAuthority::VerifiedState;
        side_effect.sequence = event_counter_;
        side_effect.content.emplace_back(public_text(
            "uncertain input dispatch pending verification turn=" + std::to_string(turn)));
        side_effect.provenance.push_back(next_event_id());
        side_effect.consumed = true;
        side_effects_.emplace_back(side_effect.id, turn);
        presented_.push_back(std::move(side_effect));
    }

    // User corrections accumulate for the whole session (P1, never dropped).
    if (turn % config_.constraint_period == 0) {
        ContextItem constraint;
        constraint.id = next_item_id();
        constraint.kind = ContextItemKind::UserConstraint;
        constraint.authority = ContextAuthority::UserConstraint;
        constraint.sequence = event_counter_;
        constraint.content.emplace_back(public_text(
            synthetic_text(rng_, 80, 200, "constraint turn=" + std::to_string(turn) + " ")));
        constraint.provenance.push_back(next_event_id());
        constraint.consumed = true;
        constraint_ids_.push_back(constraint.id);
        presented_.push_back(std::move(constraint));
    }

    // Retrieved memory every 20 turns (P4).
    if (turn % config_.memory_period == 0) {
        ContextItem memory;
        memory.id = next_item_id();
        memory.kind = ContextItemKind::RetrievedMemory;
        memory.authority = ContextAuthority::RetrievedMemory;
        memory.sequence = event_counter_;
        memory.content.emplace_back(public_text(
            synthetic_text(rng_, 200, 400, "memory turn=" + std::to_string(turn) + " ")));
        memory.provenance.push_back(next_event_id());
        memory.consumed = true;
        presented_.push_back(std::move(memory));
    }
}

void SessionDriver::measure(std::uint64_t turn, RunResult &result) {
    ContextRequest request;
    request.task_id = task_id_;
    request.session_id = session_id_;
    request.profile_id = profile_id_;
    request.task_epoch = 3;
    request.environment_epoch = 2;
    request.through_event_sequence = event_counter_;
    request.limits = limits_;
    request.items = presented_;
    request.tools = tools_;

    const auto prepared = manager_.prepare(request);
    if (!prepared) {
        result.failures.push_back("turn " + std::to_string(turn) + ": prepare failed: " +
                                  prepared.error().safe_message);
        return;
    }
    const auto &value = prepared.value();

    Sample sample;
    sample.turn = turn;
    sample.estimated_tokens = value.budget.estimated_tokens;
    sample.utilization = value.budget.utilization;
    sample.watermark = context_watermark_name(value.budget.watermark);
    sample.checkpoint_recommended = value.budget.checkpoint_recommended;
    sample.items = presented_.size();
    sample.selected = value.budget.selected_items;
    sample.replaced = value.budget.replaced_items;
    sample.compressed = value.budget.compressed_items;
    sample.dropped = value.budget.dropped_items;
    sample.selection_digest = digest_hex(value.selection_digest);
    result.samples.push_back(sample);

    // G1/G6 audit checks for this sample.
    std::set<ContextItemId> audit_ids;
    std::set<ContextItemId> presented_ids;
    for (const auto &item : presented_) {
        presented_ids.insert(item.id);
    }
    if (value.item_audit.size() != presented_.size()) {
        result.failures.push_back("turn " + std::to_string(turn) + ": audit size mismatch");
    }
    for (const auto &audit : value.item_audit) {
        audit_ids.insert(audit.id);
        if (audit.disposition == ContextItemDisposition::RejectedMinimumSet) {
            result.failures.push_back("turn " + std::to_string(turn) +
                                      ": minimum set rejected for " + audit.id.to_string());
        }
    }
    if (audit_ids.size() != value.item_audit.size() || audit_ids != presented_ids) {
        result.failures.push_back("turn " + std::to_string(turn) +
                                  ": audit ids do not match presented items");
    }
    if (value.budget.utilization > 1.0 + 1e-9) {
        result.failures.push_back("turn " + std::to_string(turn) + ": utilization above 1.0");
    }
    if (value.budget.watermark == ContextWatermark::Hard) {
        result.failures.push_back("turn " + std::to_string(turn) + ": hard watermark reached");
    }
    std::set<ContextItemId> selected_ids;
    std::map<ContextItemId, const ContextItemAudit *> audit_by_id;
    for (const auto &audit : value.item_audit) {
        audit_by_id.emplace(audit.id, &audit);
        if (audit.disposition == ContextItemDisposition::Selected ||
            audit.disposition == ContextItemDisposition::SelectedByReference ||
            audit.disposition == ContextItemDisposition::Compressed) {
            selected_ids.insert(audit.id);
        }
    }
    // G4: every user constraint stays in the minimum set.
    for (const auto &constraint : constraint_ids_) {
        const auto found = audit_by_id.find(constraint);
        if (found == audit_by_id.end() || found->second->disposition != ContextItemDisposition::Selected ||
            found->second->reason != "minimum_set") {
            result.failures.push_back("turn " + std::to_string(turn) +
                                      ": user constraint not retained: " + constraint.to_string());
        }
    }
    // G6: the pending tool result of the current turn stays selected.
    if (pending_result_id_.has_value()) {
        const auto found = audit_by_id.find(*pending_result_id_);
        if (found == audit_by_id.end() ||
            found->second->disposition != ContextItemDisposition::Selected) {
            result.failures.push_back("turn " + std::to_string(turn) +
                                      ": pending tool result not selected");
        }
    }

    if (turn == length_) {
        // Final-request baseline metrics.
        result.final_items = presented_.size();
        result.final_constraints = constraint_ids_.size();
        for (const auto &audit : value.item_audit) {
            auto &mix = result.final_mix[context_item_kind_name(audit.kind)];
            const bool is_selected = audit.disposition == ContextItemDisposition::Selected;
            const bool by_reference =
                audit.disposition == ContextItemDisposition::SelectedByReference;
            const bool compressed = audit.disposition == ContextItemDisposition::Compressed;
            if (is_selected) {
                ++mix.selected;
                mix.selected_tokens += audit.estimated_tokens;
                if (audit.reason == "minimum_set") {
                    result.final_min_set_tokens += audit.estimated_tokens;
                }
            } else if (by_reference) {
                ++mix.selected_by_reference;
                mix.selected_tokens += audit.estimated_tokens;
            } else if (compressed) {
                ++mix.compressed;
                mix.selected_tokens += audit.estimated_tokens;
            } else {
                ++mix.dropped;
                mix.dropped_tokens += audit.estimated_tokens;
            }
        }
        std::uint64_t raw = 16; // per-request assembly base
        for (const auto &item : presented_) {
            const auto estimate = counter_->estimate_item(item, profile_id_);
            raw += estimate ? estimate.value().upper_bound : 0;
            raw += 2 * (1 + item.content.size());
        }
        result.final_raw_presented_tokens = raw;
    }
}

RunResult SessionDriver::run() {
    RunResult result;
    push_static_frame();
    for (std::uint64_t turn = 1; turn <= length_; ++turn) {
        begin_turn(turn);
        emit_turn_items(turn);
        if (turn <= 100 || turn % 10 == 0) {
            measure(turn, result);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Gates
// ---------------------------------------------------------------------------

struct GateReport final {
    bool g1_request_closure = true;
    bool g2_steady_band = true;
    bool g3_bound_independent_of_n = true;
    bool g4_constraint_retention = true;
    bool g5_determinism = true;
    bool g6_audit_complete = true;
    std::vector<std::string> failures;
};

[[nodiscard]] GateReport evaluate_gates(const FrozenConfig &config,
                                        const std::map<std::uint64_t, RunResult> &runs,
                                        const RunResult &repeat_run) {
    GateReport gates;
    const auto fail = [&gates](std::string message) { gates.failures.push_back(std::move(message)); };

    for (const auto &[length, run] : runs) {
        if (!run.failures.empty()) {
            gates.g1_request_closure = false;
            gates.g4_constraint_retention = false;
            gates.g6_audit_complete = false;
            for (const auto &message : run.failures) {
                fail("N=" + std::to_string(length) + ": " + message);
            }
        }
        if (run.samples.empty()) {
            fail("N=" + std::to_string(length) + ": no samples");
            gates.g1_request_closure = false;
            continue;
        }
        const auto last = run.samples.back();
        if (last.turn != length) {
            fail("N=" + std::to_string(length) + ": final sample missing");
        }
        for (const auto &sample : run.samples) {
            if (sample.turn > config.warmup_turns &&
                (sample.estimated_tokens < config.steady_floor_tokens ||
                 sample.estimated_tokens > config.checkpoint_bound_tokens)) {
                gates.g2_steady_band = false;
                fail("N=" + std::to_string(length) + " turn " + std::to_string(sample.turn) +
                     ": steady tokens " + std::to_string(sample.estimated_tokens) +
                     " outside [" + std::to_string(config.steady_floor_tokens) + ", " +
                     std::to_string(config.checkpoint_bound_tokens) + "]");
            }
        }
    }

    const auto &run100 = runs.at(100);
    const auto &run500 = runs.at(500);
    const auto &run1000 = runs.at(1000);
    const auto max100 = steady_max(run100, config.warmup_turns);
    const auto max1000 = steady_max(run1000, config.warmup_turns);
    if (max100 == 0) {
        gates.g3_bound_independent_of_n = false;
        fail("N=100: no steady samples");
    } else if (static_cast<double>(max1000) >
               config.plateau_ratio_cap * static_cast<double>(max100)) {
        gates.g3_bound_independent_of_n = false;
        fail("plateau drift: S_max(1000)=" + std::to_string(max1000) + " vs S_max(100)=" +
             std::to_string(max100));
    }
    if (run1000.final_raw_presented_tokens <
        config.raw_growth_factor * run100.final_raw_presented_tokens) {
        gates.g3_bound_independent_of_n = false;
        fail("raw history did not grow linearly: R(1000)=" +
             std::to_string(run1000.final_raw_presented_tokens) + " R(100)=" +
             std::to_string(run100.final_raw_presented_tokens));
    }
    if (run500.final_raw_presented_tokens <= run100.final_raw_presented_tokens ||
        run1000.final_raw_presented_tokens <= run500.final_raw_presented_tokens) {
        gates.g3_bound_independent_of_n = false;
        fail("raw history not monotonically growing with N");
    }

    if (repeat_run.samples.size() != run100.samples.size()) {
        gates.g5_determinism = false;
        fail("determinism: sample count differs on repeat");
    } else {
        for (std::size_t index = 0; index < run100.samples.size(); ++index) {
            const auto &left = run100.samples[index];
            const auto &right = repeat_run.samples[index];
            if (left.turn != right.turn || left.estimated_tokens != right.estimated_tokens ||
                left.selected != right.selected || left.dropped != right.dropped ||
                left.replaced != right.replaced || left.compressed != right.compressed ||
                left.selection_digest != right.selection_digest) {
                gates.g5_determinism = false;
                fail("determinism: sample " + std::to_string(index) + " differs on repeat");
                break;
            }
        }
    }
    return gates;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue stats_json(const SeriesStats &stats) {
    JsonValue::Object object;
    object.emplace_back("count", static_cast<std::int64_t>(stats.count));
    object.emplace_back("min", static_cast<std::int64_t>(stats.min));
    object.emplace_back("p50", static_cast<std::int64_t>(stats.p50));
    object.emplace_back("p95", static_cast<std::int64_t>(stats.p95));
    object.emplace_back("max", static_cast<std::int64_t>(stats.max));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue series_json(const std::vector<Sample> &samples,
                                    std::uint64_t warmup_turns) {
    JsonValue::Array series;
    for (const auto &sample : samples) {
        if (sample.turn <= warmup_turns || sample.turn % 20 != 0) {
            continue; // compact the stored series; stats use every sample
        }
        JsonValue::Object point;
        point.emplace_back("turn", static_cast<std::int64_t>(sample.turn));
        point.emplace_back("tokens", static_cast<std::int64_t>(sample.estimated_tokens));
        point.emplace_back("utilization", sample.utilization);
        point.emplace_back("watermark", sample.watermark);
        series.emplace_back(JsonValue(std::move(point)));
    }
    return JsonValue(std::move(series));
}

[[nodiscard]] JsonValue run_json(const RunResult &run, std::uint64_t warmup_turns) {
    JsonValue::Object object;
    object.emplace_back("samples", static_cast<std::int64_t>(run.samples.size()));
    object.emplace_back("steady_tokens", stats_json(steady_stats(run.samples, warmup_turns)));
    object.emplace_back("series_sampled", series_json(run.samples, warmup_turns));
    object.emplace_back("final_items", static_cast<std::int64_t>(run.final_items));
    object.emplace_back("final_raw_presented_tokens",
                        static_cast<std::int64_t>(run.final_raw_presented_tokens));
    object.emplace_back("final_min_set_tokens",
                        static_cast<std::int64_t>(run.final_min_set_tokens));
    object.emplace_back("final_constraints", static_cast<std::int64_t>(run.final_constraints));
    const auto checkpoint_recommended = std::count_if(
        run.samples.begin(), run.samples.end(), [](const Sample &sample) {
            return sample.checkpoint_recommended;
        });
    object.emplace_back("checkpoint_recommended_samples",
                        static_cast<std::int64_t>(checkpoint_recommended));
    JsonValue::Object mix;
    for (const auto &[kind, entry] : run.final_mix) {
        JsonValue::Object entry_json;
        entry_json.emplace_back("selected", static_cast<std::int64_t>(entry.selected));
        entry_json.emplace_back("selected_by_reference",
                                static_cast<std::int64_t>(entry.selected_by_reference));
        entry_json.emplace_back("compressed", static_cast<std::int64_t>(entry.compressed));
        entry_json.emplace_back("dropped", static_cast<std::int64_t>(entry.dropped));
        entry_json.emplace_back("selected_tokens", static_cast<std::int64_t>(entry.selected_tokens));
        entry_json.emplace_back("dropped_tokens", static_cast<std::int64_t>(entry.dropped_tokens));
        mix.emplace_back(kind, JsonValue(std::move(entry_json)));
    }
    object.emplace_back("final_disposition_by_kind", JsonValue(std::move(mix)));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue config_json(const FrozenConfig &config) {
    JsonValue::Object object;
    object.emplace_back("schema", "mira.m16.long-session-baseline.v1");
    object.emplace_back("context_window_tokens",
                        static_cast<std::int64_t>(config.context_window_tokens));
    object.emplace_back("reserved_output_tokens",
                        static_cast<std::int64_t>(config.reserved_output_tokens));
    object.emplace_back("safety_margin_tokens",
                        static_cast<std::int64_t>(config.safety_margin_tokens));
    object.emplace_back("steady_floor_tokens", static_cast<std::int64_t>(config.steady_floor_tokens));
    object.emplace_back("checkpoint_bound_tokens",
                        static_cast<std::int64_t>(config.checkpoint_bound_tokens));
    object.emplace_back("warmup_turns", static_cast<std::int64_t>(config.warmup_turns));
    object.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    JsonValue::Array lengths;
    for (const auto length : config.session_lengths) {
        lengths.emplace_back(static_cast<std::int64_t>(length));
    }
    object.emplace_back("session_lengths", JsonValue(std::move(lengths)));
    object.emplace_back("plateau_ratio_cap", config.plateau_ratio_cap);
    object.emplace_back("raw_growth_factor", static_cast<std::int64_t>(config.raw_growth_factor));
    object.emplace_back("image_min_bytes", static_cast<std::int64_t>(config.image_min_bytes));
    object.emplace_back("image_max_bytes", static_cast<std::int64_t>(config.image_max_bytes));
    object.emplace_back("ui_tree_min_bytes", static_cast<std::int64_t>(config.ui_tree_min_bytes));
    object.emplace_back("ui_tree_max_bytes", static_cast<std::int64_t>(config.ui_tree_max_bytes));
    object.emplace_back("user_msg_min_bytes", static_cast<std::int64_t>(config.user_msg_min_bytes));
    object.emplace_back("user_msg_max_bytes", static_cast<std::int64_t>(config.user_msg_max_bytes));
    object.emplace_back("assistant_msg_min_bytes",
                        static_cast<std::int64_t>(config.assistant_msg_min_bytes));
    object.emplace_back("assistant_msg_max_bytes",
                        static_cast<std::int64_t>(config.assistant_msg_max_bytes));
    object.emplace_back("tool_result_min_bytes",
                        static_cast<std::int64_t>(config.tool_result_min_bytes));
    object.emplace_back("tool_result_max_bytes",
                        static_cast<std::int64_t>(config.tool_result_max_bytes));
    object.emplace_back("action_min_bytes", static_cast<std::int64_t>(config.action_min_bytes));
    object.emplace_back("action_max_bytes", static_cast<std::int64_t>(config.action_max_bytes));
    object.emplace_back("tool_pair_period", static_cast<std::int64_t>(config.tool_pair_period));
    object.emplace_back("workflow_period", static_cast<std::int64_t>(config.workflow_period));
    object.emplace_back("recovery_period", static_cast<std::int64_t>(config.recovery_period));
    object.emplace_back("constraint_period", static_cast<std::int64_t>(config.constraint_period));
    object.emplace_back("memory_period", static_cast<std::int64_t>(config.memory_period));
    object.emplace_back("checkpoint_period", static_cast<std::int64_t>(config.checkpoint_period));
    object.emplace_back("side_effect_lifetime_turns",
                        static_cast<std::int64_t>(config.side_effect_lifetime_turns));
    object.emplace_back("token_counter", "ConservativeTokenCounter(default)");
    object.emplace_back("token_semantics", "conservative upper bounds, not provider counts");
    return JsonValue(std::move(object));
}

} // namespace

int main(int argc, char **argv) {
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (output_path.empty()) {
            output_path = argument;
        }
    }

    const FrozenConfig config;
    const ContextLimits limits = frozen_limits(config);

    // Fail fast when the frozen derivations drift from the contract math.
    if (limits.input_budget_tokens() != 38'784 ||
        limits.watermark_tokens(0.85) != config.checkpoint_bound_tokens ||
        limits.watermark_tokens(0.50) != config.steady_floor_tokens) {
        std::cerr << "frozen budget derivation mismatch\n";
        return 1;
    }

    auto counter = std::make_shared<ConservativeTokenCounter>();
    const StandardContextManager manager(counter);

    std::map<std::uint64_t, RunResult> runs;
    for (const auto length : config.session_lengths) {
        SessionDriver driver(config, length, manager, counter);
        runs.emplace(length, driver.run());
    }
    // G5: repeat the N=100 session and compare the full series.
    SessionDriver repeat(config, 100, manager, counter);
    const auto repeat_run = repeat.run();

    const auto gates = evaluate_gates(config, runs, repeat_run);

    JsonValue::Object report;
    report.emplace_back("schema", "mira.m16.long-session-baseline.v1");
    const auto config_object = config_json(config);
    report.emplace_back("dataset_digest", digest_hex(canonical_json_digest(config_object)));
    report.emplace_back("config", config_object);
    JsonValue::Object runs_json;
    for (const auto &[length, run] : runs) {
        runs_json.emplace_back(std::to_string(length), run_json(run, config.warmup_turns));
    }
    report.emplace_back("runs", JsonValue(std::move(runs_json)));
    JsonValue::Object gates_json;
    gates_json.emplace_back("g1_request_closure", gates.g1_request_closure);
    gates_json.emplace_back("g2_steady_band", gates.g2_steady_band);
    gates_json.emplace_back("g3_bound_independent_of_n", gates.g3_bound_independent_of_n);
    gates_json.emplace_back("g4_constraint_retention", gates.g4_constraint_retention);
    gates_json.emplace_back("g5_determinism", gates.g5_determinism);
    gates_json.emplace_back("g6_audit_complete", gates.g6_audit_complete);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : gates.failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "synthetic workload only; miracle-derived replay pending MNT-202609-27 evidence; "
        "token figures are ConservativeTokenCounter upper bounds, not provider counts");

    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    const bool all_gates = gates.g1_request_closure && gates.g2_steady_band &&
                           gates.g3_bound_independent_of_n && gates.g4_constraint_retention &&
                           gates.g5_determinism && gates.g6_audit_complete;
    if (!all_gates) {
        for (const auto &message : gates.failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return all_gates ? 0 : 1;
}
