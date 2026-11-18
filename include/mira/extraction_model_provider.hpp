#pragma once

#include <mira/memory_agent.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_provider.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Model-backed IConsolidationModel: one provider-inferred proposal round over
// the recorded events (DEC-114). The deterministic extractor only reads two
// event shapes; this adapter lets a host-supplied model propose the remaining
// kinds — every proposal still flows through the consolidate() policy pipeline
// (model_assisted forced, HumanConfirmed stripped, injection gate, claim-key
// conflicts, approval), so the adapter owns shape and citation binding only.
// ---------------------------------------------------------------------------

// Bounds and failure semantics of one extraction surface.
struct ExtractionModelOptions final {
    // Output bound (RULE-08): the first bound candidates win.
    std::size_t max_candidates = 16;
    // Transcript bound: only the first N events are rendered for the model
    // (stable truncation); the deterministic extractor still sees everything.
    std::size_t max_transcript_events = 64;
    // Per-event payload characters rendered into the transcript.
    std::size_t max_payload_chars = 200;
    // Statement admission bound passed to the shared gate.
    std::size_t max_statement_chars = 512;
    std::chrono::milliseconds deadline{10'000};
    std::uint64_t max_output_tokens = 2'048;
    // True (default): any provider error, timeout, non-completed status or
    // malformed output yields an empty proposal set — extraction degrades to
    // the deterministic path and never fails the caller. False: the same
    // conditions throw (the Record job's exception-to-failed-Result path).
    bool degrade_on_error = true;

    [[nodiscard]] Result<void> validate() const;
};

class ProviderConsolidationModel final : public IConsolidationModel {
  public:
    // The provider must outlive the adapter (borrowed reference per call,
    // same division as ProviderSemanticConsolidator).
    explicit ProviderConsolidationModel(IModelProvider &provider,
                                        ExtractionModelOptions options = {});
    ~ProviderConsolidationModel() override;

    ProviderConsolidationModel(const ProviderConsolidationModel &) = delete;
    ProviderConsolidationModel &operator=(const ProviderConsolidationModel &) = delete;

    // One bounded proposal round over the events. Candidates carry kind,
    // statement, confidence and evidence bound to the cited event ids; every
    // candidate is model_assisted with Unverified verification (the pipeline
    // enforces this regardless of what the adapter sets).
    [[nodiscard]] std::vector<MemoryCandidate> propose(std::span<const EventEnvelope> events,
                                                       const MemoryScope &scope) override;

    [[nodiscard]] const ExtractionModelOptions &options() const noexcept { return options_; }

  private:
    IModelProvider &provider_;
    ExtractionModelOptions options_;
};

// ---------------------------------------------------------------------------
// One-call assembly of the extraction supply (DEC-113 shape): config in,
// validated profile + provider + adapter out, plus the MemoryAgentProcessor
// factory for the service-side Record/Compress supply.
// ---------------------------------------------------------------------------

// Host data for the extraction supply. Credentials travel as a SecretRef name
// only; the plaintext is resolved at the transport boundary.
struct ExtractionSupplyConfig final {
    // "https://api.example.com" — scheme plus host (plus optional port).
    std::string endpoint_origin;
    std::string api_prefix = "/v1";
    // Wire model name.
    std::string model_selector;
    std::string credential_secret_name;
    ProtocolDialect dialect = ProtocolDialect::OpenAIResponsesV1;
    std::chrono::milliseconds deadline{30'000};
    std::uint64_t max_output_tokens = 2'048;
    std::size_t max_candidates = 16;
    bool degrade_on_error = true;

    [[nodiscard]] Result<void> validate() const;
};

// The owned halves of the wired supply. The provider is shared because the
// agent processor constructs a fresh provider per invocation over the same
// transport; the consolidator adapter is exclusive. All members stay null on
// a failed wire (validation is strictly before construction).
struct ExtractionSupplyStack final {
    std::shared_ptr<const ModelProfile> profile;
    std::shared_ptr<IHttpTransport> transport;
    std::shared_ptr<IModelProvider> provider;
    std::unique_ptr<ProviderConsolidationModel> consolidation_model;

    // The consolidate()-path supply: hand to MemoryConsolidator's model hook.
    [[nodiscard]] IConsolidationModel &model() const;

    // The service-path supply: a make_model_memory_agent processor over the
    // same profile and transport (fresh provider per invocation). The
    // executor must outlive the returned processor's uses.
    [[nodiscard]] MemoryAgentProcessor agent_processor(kairo::Executor &executor) const;
};

// Builds and validates the profile and both supply surfaces. Throws nothing;
// every failure is a returned Error before any member is constructed.
[[nodiscard]] Result<ExtractionSupplyStack>
wire_extraction_supply(const ExtractionSupplyConfig &config,
                       std::shared_ptr<IHttpTransport> transport);

} // namespace mira
