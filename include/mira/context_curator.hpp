#pragma once

#include <mira/context_consolidation.hpp>
#include <mira/context_contracts.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_working_context.hpp>
#include <mira/core_contracts.hpp>
#include <mira/json.hpp>
#include <mira/model_provider.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Context Curator Stage W2: the model-mediated Working Context maintainer
// (DEC-035, design §4.2/§6/§8; M21)
//
// `IContextCurator` turns `previous snapshot + new committed checkpoint +
// recent events` into the next `WorkingContextSnapshot` candidate — the
// task/session-oriented view of what the model needs to know to continue the
// task now. The candidate enters the store only through the frozen
// `commit_working_context` pipeline (design §5.2): the curator proposes, the
// deterministic commit validation disposes, and a same-watermark digest
// conflict fails closed instead of silently overwriting (the backstop for
// non-deterministic model output).
//
// Every model statement is untrusted input (RULE-09): the reference
// implementation below reuses the M19 model boundary verbatim —
// StrictJsonSchema output contract, a numbered three-block transcript
// (previous / checkpoint / events), citation-to-provenance binding where an
// out-of-range citation drops the statement instead of being repaired,
// marker filters, confidence floors, bounded output, deadline and
// cooperative cancellation, and fail-closed parsing. The model is injected
// through `IModelProvider`; any available source model works, including the
// model that produced the original context (DEC-036). Core ships the
// adapter, never a model.
//
// Retain / supersede / conflict-retain are instruction-contract vocabulary,
// not runtime-enforced editorial policy: the model expresses them through
// citations, the runtime validates mechanics (provenance, bounds, markers)
// and commits the whole snapshot. One mechanical guard is frozen: with a
// non-empty previous snapshot, a candidate whose bound items cite no
// previous-block entry is rejected outright ("degenerate merge") — superseding
// without citing what was superseded, or wiping a rich snapshot with fresh
// content, never reaches the store (M21 plan §4.1).
// ---------------------------------------------------------------------------

// Bounds, filters and call shaping for one curation run. Defaults are
// documented values, not a frozen contract (mirrors `ConsolidationOptions`).
struct ContextCurationOptions final {
    // Output bounds (RULE-08), aligned with `WorkingContextMergeOptions`.
    std::size_t max_items_per_section = 64;
    std::size_t max_item_chars = 512;
    std::size_t max_source_events = 256;
    // Input bound for the recent-events block; entries are pre-bounded by
    // conversation segmentation, the count is capped here.
    std::size_t max_recent_events = 128;
    // Model statements (and the run-level confidence) below this floor are
    // dropped / fail the run.
    double min_confidence = 0.0;
    // Soft deadline for the whole curation (including the model call);
    // exceeding it fails the run — callers keep the previous snapshot
    // (design §9), never a partial one.
    std::chrono::milliseconds deadline{10'000};
    std::uint64_t max_output_tokens = 2'048;
    // Content markers that must never enter the snapshot (RULE-09).
    std::vector<std::string> forbidden_markers = {
        "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};
    // Instruction-shaped markers for untrusted model text.
    std::vector<std::string> injection_markers = {
        "ignore previous", "disregard previous", "you are now",
        "system:",         "new instructions:",  "override policy"};
    // Cooperative cancellation probe owned by the operation supervisor; the
    // adapter maps it into the provider OperationContext.
    std::function<bool()> cancellation_requested;

    [[nodiscard]] bool cancelled() const noexcept {
        return cancellation_requested != nullptr && cancellation_requested();
    }

    [[nodiscard]] Result<void> validate() const;
};

// JSON schema of the curator output contract (mode `StrictJsonSchema`);
// exposed so hosts and tests can pin exactly what the curator demands: one
// object with a run-level `confidence` and the eight snapshot sections, each
// item carrying `content`, `sources` (transcript numbers) and `confidence`.
[[nodiscard]] JsonSchema working_context_curation_output_schema();

class IContextCurator {
  public:
    virtual ~IContextCurator() = default;
    // Produces the next snapshot candidate from the previous committed
    // snapshot (null on a fresh identity chain, e.g. after an epoch bump),
    // the newly committed checkpoint driving this refresh, and the recent
    // conversation tail (entries must not run past the checkpoint's
    // watermark — the candidate watermark equals the checkpoint's, so
    // varying inputs at a fixed watermark is a conflict by construction and
    // fails closed at commit). Failures mean "no new projection": callers
    // keep the previous snapshot and close requests as before (design §9) —
    // no partial candidate is ever returned.
    [[nodiscard]] virtual Result<WorkingContextSnapshot>
    curate(const WorkingContextSnapshot *previous, const ConversationCheckpoint &checkpoint,
           std::span<const ConversationSegmentEntry> recent_events,
           const ContextCurationOptions &options) = 0;
};

// Reference curator backed by an injected `IModelProvider` (DEC-035 §6,
// DEC-036 supply): renders the three-block numbered transcript, requests a
// schema-constrained JSON curation, then re-validates everything the model
// returned — statement bounds, marker filters, citation-to-provenance
// binding across the previous/checkpoint/events blocks, per-section caps and
// the confidence floor — dropping invalid statements instead of admitting
// them. The provider call runs synchronously on the caller's
// Executor-managed operation; deadline and cancellation travel through
// `options`.
class ProviderContextCurator final : public IContextCurator {
  public:
    explicit ProviderContextCurator(IModelProvider &provider);
    ~ProviderContextCurator() override;

    ProviderContextCurator(const ProviderContextCurator &) = delete;
    ProviderContextCurator &operator=(const ProviderContextCurator &) = delete;

    [[nodiscard]] Result<WorkingContextSnapshot>
    curate(const WorkingContextSnapshot *previous, const ConversationCheckpoint &checkpoint,
           std::span<const ConversationSegmentEntry> recent_events,
           const ContextCurationOptions &options) override;

  private:
    IModelProvider &provider_;
};

} // namespace mira
