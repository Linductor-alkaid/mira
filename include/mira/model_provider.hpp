#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_sse.hpp>
#include <mira/model_supervisor.hpp>
#include <mira/model_transport.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira {

struct ProviderInferOptions final {
    bool stream = false;
    // Writes the raw terminal payload to a protected artifact and attaches
    // the reference to the response (design LLM API §13.2/§15).
    bool capture_raw_response = false;
};

// One configured provider endpoint. Implementations own no lifecycle beyond
// the injected transport; they never advance task state.
class IModelProvider {
  public:
    virtual ~IModelProvider() = default;
    [[nodiscard]] virtual const ModelProfile &profile() const = 0;
    [[nodiscard]] virtual Result<ModelResponse> infer(const ModelRequest &request,
                                                      const OperationContext &context,
                                                      const ProviderInferOptions &options) = 0;
    // Diagnostics from the most recent call; defaults report an empty trace.
    [[nodiscard]] virtual const TransportTrace &last_trace() const;
    [[nodiscard]] virtual const SseStreamStats &last_sse_stats() const;
    [[nodiscard]] virtual std::optional<std::chrono::milliseconds>
    last_retry_after_hint() const;
};

// OpenAI-compatible endpoint over the two fixed M3 dialects. Each instance
// serves exactly one profile; dialect fallback inside one operation never
// happens.
class OpenAiCompatibleProvider final : public IModelProvider {
  public:
    OpenAiCompatibleProvider(std::shared_ptr<const ModelProfile> profile,
                             std::shared_ptr<IHttpTransport> transport,
                             std::shared_ptr<IArtifactSource> artifacts,
                             std::shared_ptr<IArtifactStore> protected_artifacts = nullptr);
    ~OpenAiCompatibleProvider() override;

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &options) override;

    // Diagnostics from the last call (test surface).
    [[nodiscard]] const TransportTrace &last_trace() const noexcept override {
        return last_trace_;
    }
    [[nodiscard]] const SseStreamStats &last_sse_stats() const noexcept override {
        return sse_stats_;
    }
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    last_retry_after_hint() const override;
    [[nodiscard]] std::optional<UnvalidatedModelPreview> take_last_preview();

  private:
    [[nodiscard]] Result<ArtifactRef> store_raw_response(const std::string &raw) const;

    std::shared_ptr<const ModelProfile> profile_;
    std::shared_ptr<IHttpTransport> transport_;
    std::shared_ptr<IArtifactSource> artifacts_;
    std::shared_ptr<IArtifactStore> protected_artifacts_;
    ResponsesV1Mapper responses_;
    ChatCompletionsV1Mapper chat_;
    TransportTrace last_trace_;
    std::vector<std::pair<std::string, std::string>> last_headers_;
    SseStreamStats sse_stats_;
    std::optional<UnvalidatedModelPreview> last_preview_;
};

// Classifies the request stage of one provider call for the retry table.
[[nodiscard]] RequestStage classify_provider_stage(const TransportTrace &trace,
                                                   const Error &failure);

} // namespace mira
