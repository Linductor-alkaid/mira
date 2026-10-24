#pragma once

#include <mira/agent_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_gateway.hpp>
#include <mira/tool_executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// DEC-056: Finite preserves the original whole-run limits; Continuous opts
// into request-level budgets and bounded retained state, without an accumulated
// turn/tool cutoff. Zero finite limits keep their original meaning.
enum class ConversationLimitMode : std::uint8_t { Finite, Continuous };

struct ConversationLoopConfig final {
    // Whole-run bound on model turns; exhausting it settles as MaxTurns.
    std::uint32_t max_turns = 12;
    // Bounded retries per run (Finite) or consecutive failure episode
    // (Continuous). A valid tool round starts a fresh episode.
    std::uint32_t max_recoveries = 2;
    std::chrono::milliseconds model_call_deadline{30'000};
    // Whole-run budget for tool executions.
    std::uint32_t max_tool_executions = 32;
    // Bounded continuations for a tolerated max_tokens cutoff (status
    // Incomplete, reason MaxOutputTokens), independent of max_recoveries:
    // the partial protocol-allowed text is kept in the history and the model
    // is asked to continue from the cut; a reply cut off mid tool call is
    // discarded unexecuted. In Continuous, a valid tool round resets this
    // episode budget. After the budget is spent the run settles Failed
    // with an explicit output-limit summary instead of regenerating.
    std::uint32_t max_output_continuations = 2;
    // Per-turn generation bound.
    std::uint64_t max_output_tokens_per_turn = 1024;
    std::optional<ReasoningEffort> reasoning_effort{};
    std::optional<ThinkingMode> thinking{};
    InferOptions inference{};
    ConversationLimitMode limit_mode = ConversationLimitMode::Finite;
    // Continuous-only active bounds, never accumulated task limits. History
    // is checked after compaction, before the next model dispatch. Bytes mean
    // the serialized canonical input array, not RSS or artifact payloads.
    std::size_t max_history_items = 128;
    std::size_t max_history_bytes = 2 * 1024 * 1024;
    std::size_t max_retained_turns = 64;
    std::uint32_t max_tools_per_turn = 32;
};

enum class ConversationOutcome : std::uint8_t {
    Answered,  // The model produced a terminal text answer.
    Failed,    // Non-recoverable failure, refusal or filtering.
    Cancelled, // Cancellation or admission rejection.
    MaxTurns,  // Turn budget exhausted before a terminal answer.
};

[[nodiscard]] std::string conversation_outcome_name(ConversationOutcome outcome);

struct ConversationTurnRecord final {
    std::uint64_t turn = 0;
    ModelRequestId model_request;
    std::string summary; // "tool:<wire_name>[;...]" or "answer".
    std::string note;
};

struct ConversationLoopResult final {
    ConversationOutcome outcome = ConversationOutcome::Failed;
    // Complete in Finite mode; recent bounded window in Continuous mode.
    std::vector<ConversationTurnRecord> turns;
    std::uint64_t total_turns = 0;
    std::uint64_t turn_records_dropped = 0;
    std::size_t retained_history_items = 0;
    std::size_t retained_history_bytes = 0;
    // Terminal assistant text; empty unless the outcome is Answered.
    std::string answer;
    std::uint64_t recoveries = 0;
    std::string safe_summary;
    ModelUsage last_usage{};
    std::uint64_t tool_executions = 0;
    // Supplied-input seam totals across the run (DEC-053): items accepted into
    // requests, items dropped by validation or bounds, and supplier failures
    // degraded to diagnostics. All zero without a supplier.
    std::uint64_t supplied_input_items = 0;
    std::uint64_t supplied_input_dropped = 0;
    std::uint64_t supply_degradations = 0;
    // Requests that carried the DEC-088 input-reuse annotation: a host
    // identity with reuse_count >= 2 on an image-bearing supply. Zero without
    // an identity provider or without qualifying supplies.
    std::uint64_t supplied_input_reuse_annotations = 0;
    // Bounded max_tokens continuations spent by this run (DEC follow-up to
    // MIRA-20261009-002); zero without a tolerated cutoff.
    std::uint64_t output_continuations = 0;
    // Terminal truncation reason (MIRA-20261022-001 / DEC-065, amended by
    // DEC-066): the reason the provider reported for the incomplete output
    // that ended the run. It is set on the two terminal failures caused by an
    // incomplete model output - the continuation budget being exhausted
    // (MaxOutputTokens) and the recovery budget being exhausted after an
    // incomplete reply (ContextWindow/Other, or whichever reason the provider
    // reported). Every other terminal - Answered, MaxTurns, Cancelled and
    // Failed for any other cause (tool budget, refusal, admission) - leaves it
    // empty, and so does an incomplete output whose provider reported no
    // reason. The counter above reports every tolerated cutoff of the run,
    // including those the model recovered from; this field reports why the
    // run ended, so hosts classify the terminal state without reading the
    // safe_summary text.
    std::optional<IncompleteReason> terminal_incomplete_reason;
    // Requests whose tool-history replay came from the host compaction seam
    // (DEC-088 follow-up), and requests where the seam degraded to the raw
    // replay (Error, exception, empty or invalid result). Both zero without
    // a transformer or an empty history.
    std::uint64_t history_transforms = 0;
    std::uint64_t history_transform_degradations = 0;
};

// Declared per-request bounds (RULE-08) for the supplied-input seam. Items
// beyond a bound are dropped in supply order and counted on the event surface
// and in ConversationLoopResult — never silently.
struct ModelInputSupplyOptions final {
    // Max supplied items folded into one request.
    std::size_t max_items = 8;
    // Max summed ImagePart artifact bytes folded into one request; an item
    // whose images would push the total over the bound is dropped whole.
    std::uint64_t max_image_bytes = 8ULL * 1024ULL * 1024ULL;

    [[nodiscard]] Result<void> validate() const;
};

// Host-controlled per-request structured input supply (DEC-053, issue #85 /
// MIRA-20261009-001): lets a host tool hand images (e.g. a screenshot artifact)
// to the next model turn as canonical ImageParts. Resolved exactly once per
// assembled request, after tool rounds complete and before the model call; an
// empty vector is the normal no-injection state. An Error return is a supply
// failure that degrades to a visible diagnostic instead of blocking the loop.
// Identity gating (task / session / epoch) closes inside the callback, which
// receives the running spec. Called synchronously on the run() caller's worker:
// keep it bounded and free of blocking work, like WorkingContextSupplier.
using ModelInputSupplier =
    std::function<Result<std::vector<ModelInputItem>>(const AgentLoopSpec &)>;

// Host-asserted cross-turn identity of the supplied input (DEC-088, follow-up
// to the DEC-053 seam). The loop is constructed per run and never sees prior
// runs, so only the host can count how often identical content has been
// re-offered across its dialog turns; the loop only renders the annotation.
// content_digest documents the identity the count refers to (truncated on the
// event surface), reuse_count is the number of consecutive identical
// re-offers before this request (0 = first supply) and semantic_label is
// rendered verbatim in the note (a generic fallback applies when empty).
struct InputReuseMeta final {
    std::string content_digest;
    std::uint32_t reuse_count = 0;
    std::string semantic_label;
};

// Optional per-request identity resolution with the same call discipline as
// ModelInputSupplier: resolved exactly once per assembled request, on the
// run() caller's worker, bounded and free of blocking work. An empty optional
// supplies no identity; an exception skips the annotation for that request
// with a visible degradation event.
using ModelInputIdentityProvider =
    std::function<std::optional<InputReuseMeta>(const AgentLoopSpec &)>;

// Optional host compaction seam over the canonical tool-history replay
// (DEC-088 history-compaction follow-up to the DEC-053 seam; downstream
// ledger MIRA-20261014-006). The loop owns the round-trip history and
// replays it verbatim into every request, so a host that wants watermark
// compaction, reference replacement or summarization can never see it
// today. With a transformer wired, the loop hands the full canonical
// history to the callback once per assembled request (after tool rounds
// complete, before build_request, on the run() caller's worker — bounded
// and free of blocking work, like ModelInputSupplier) and replays the
// returned list instead. The callback receives the loop's own items and
// must return provider-valid conversation items: User/Assistant roles
// only, tool calls still paired with their results in execution order.
// Any failure shape degrades to the raw replay with a visible event —
// an Error return, an empty result, a structurally invalid list (System
// role or empty content) or an exception never block the loop. Without
// the transformer, request assembly stays unchanged byte for byte.
using HistoryTransformer =
    std::function<Result<std::vector<ModelInputItem>>(const std::vector<ModelInputItem> &history)>;

// The no-observation conversational loop (DEC-047): model -> tool proposal ->
// execute -> result -> model, driven over the same ModelGateway, admission and
// budget contracts as the device agent loop. There is no environment, no
// observation and no device verification: a plain text answer is the terminal
// state. Tool rounds ride the canonical ToolCallPart/ToolResultPart items, so
// providers see native function_call/function_call_output semantics instead of
// flattened JSON text.
//
// Like AgentLoop, each turn is a bounded work unit; cancellation, admission
// rejection and terminal answers stop the loop before any new tool executes.
// The loop owns no threads and no queues — hosts run it on an Executor worker
// exactly like AgentLoop::run().
class ConversationLoop final {
  public:
    ConversationLoop(ModelGateway &gateway,
                     ConversationLoopConfig config = ConversationLoopConfig{});

    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime, SessionId session);
    void set_tool_registry(std::shared_ptr<BuiltinToolRegistry> tools);

    // Attaches the optional supplied-input seam (DEC-053) and its per-request
    // bounds. Without a supplier, request assembly stays unchanged byte for
    // byte. With one, each assembled request resolves the callback exactly
    // once and folds the surviving content parts into that request's primary
    // user item under the loop's own envelope (User role, request.v1
    // provenance, Internal authority — host input is never elevated);
    // supply attribution rides the ModelInputSupplied event and the result
    // counters. The run's tool-history replay never copies supplied content,
    // so nothing accumulates across turns (hosts re-offer per turn if
    // wanted). Only TextPart/ImagePart/FilePart are accepted; items that fail
    // validation or bounds are dropped with a visible count. Supply failures
    // (Error or exception) degrade to one diagnostic and never block the
    // loop. Wire once before run(), like the other setters.
    void set_model_input_supplier(ModelInputSupplier supplier,
                                  ModelInputSupplyOptions options = ModelInputSupplyOptions{});

    // Attaches the optional DEC-088 input-reuse identity seam. Without one,
    // request assembly stays unchanged byte for byte. With one, an identity
    // whose reuse_count reaches 2 (the third consecutive identical supply) and
    // whose supply carries an image appends one Internal-authority system note
    // as the last part of the primary user envelope — never the system role,
    // never host-elevated. The count is trusted host state: the loop cannot
    // observe prior runs. Wire once before run(), like the other setters.
    void set_model_input_identity(ModelInputIdentityProvider identity);

    // Attaches the optional history compaction seam (DEC-088 follow-up).
    // Without one, the canonical tool-history replay stays unchanged byte
    // for byte. With one, each assembled request whose history is non-empty
    // hands that history to the callback once and replays the returned list
    // instead; the run result counts applied transforms and degradations,
    // and both surface as loop events. Degradation shapes (Error, exception,
    // empty or structurally invalid result) fall back to the raw replay —
    // the seam can rewrite or shrink the replay, never block the loop or
    // strip its history silently. Wire once before run(), like the other
    // setters.
    // Continuous mode commits valid transformed history for subsequent turns.
    // It additionally validates complete call/result groups and an unchanged
    // latest-round suffix, including signed thinking. On degradation the raw
    // history remains; exceeding its active capacity fails visibly.
    void set_history_transformer(HistoryTransformer transformer);

    [[nodiscard]] Result<ConversationLoopResult> run(const AgentLoopSpec &spec,
                                                     const OperationContext &context);

  private:
    struct SuppliedInput final {
        std::vector<ModelContentPart> parts;
        std::size_t offered = 0;
        std::size_t accepted = 0;
        std::size_t dropped = 0;
        std::map<std::string, std::int64_t> drop_reasons;
        std::uint64_t image_bytes = 0;
        bool degraded = false;
        // DEC-088: host identity resolved for this request (nullopt without a
        // provider, on an exception or with nothing accepted) and whether the
        // reuse note qualifies to render on it.
        std::optional<InputReuseMeta> reuse;
        bool annotate_reuse = false;
    };

    [[nodiscard]] Result<ModelRequest> build_request(const AgentLoopSpec &spec,
                                                     const std::vector<ModelInputItem> &history,
                                                     const std::string &feedback,
                                                     const SuppliedInput &supplied);
    // Resolves the seam once for the request being assembled (DEC-053):
    // validates and bounds the offered items, forces the untrusted envelope and
    // flattens the surviving parts. Never fails — supply errors and exceptions
    // degrade to a visible diagnostic and an empty part list.
    [[nodiscard]] SuppliedInput resolve_supplied_input(const AgentLoopSpec &spec);
    // Resolves the history compaction seam once for the request being
    // assembled (DEC-088 follow-up). Nullopt keeps the raw replay; every
    // failure shape — exception, Error, empty or structurally invalid
    // result — degrades to the raw replay with a visible event.
    [[nodiscard]] std::optional<std::vector<ModelInputItem>>
    apply_history_transform(const AgentLoopSpec &spec, const std::vector<ModelInputItem> &history,
                            ConversationLoopResult &result, std::size_t protected_items);
    void emit(const AgentLoopSpec &spec, std::string type, JsonValue summary,
              EventClass classification) const;

    ModelGateway &gateway_;
    ConversationLoopConfig config_;
    std::shared_ptr<IEventStore> events_;
    RuntimeId runtime_;
    SessionId session_;
    std::shared_ptr<BuiltinToolRegistry> tools_;
    ModelInputSupplier input_supplier_;
    ModelInputIdentityProvider identity_provider_;
    HistoryTransformer history_transformer_;
    ModelInputSupplyOptions supply_options_{};
};

} // namespace mira
