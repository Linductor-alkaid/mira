#pragma once

#include <mira/model_provider.hpp>

#include <cstddef>
#include <vector>

namespace mira {

// Serves recorded canonical responses for offline replay. The provider holds
// no transport, no secrets and no artifact writes: replay cannot reach the
// network by construction, and the capability surface says so.
class ReplayModelProvider final : public IModelProvider {
  public:
    ReplayModelProvider(std::shared_ptr<const ModelProfile> profile,
                        std::vector<ModelResponse> script);

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &options) override;

    // Number of replayed responses consumed so far.
    [[nodiscard]] std::size_t consumed() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t script_size() const noexcept { return script_.size(); }
    // Digests of the requests served, in order.
    [[nodiscard]] const std::vector<Hash> &served_request_digests() const noexcept {
        return served_;
    }
    // Set when a recorded response referenced a raw artifact that is no
    // longer present: replay quality degraded, canonical data unaffected.
    [[nodiscard]] bool raw_payload_missing() const noexcept { return raw_missing_; }

    // Marks the recorded raw artifact as deleted (tombstone semantics).
    void note_raw_artifact_erased() { raw_erased_ = true; }

  private:
    std::shared_ptr<const ModelProfile> profile_;
    std::vector<ModelResponse> script_;
    std::size_t cursor_ = 0;
    std::vector<Hash> served_;
    bool raw_missing_ = false;
    bool raw_erased_ = false;
};

} // namespace mira
