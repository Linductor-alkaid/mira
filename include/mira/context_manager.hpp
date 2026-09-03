#pragma once

#include <mira/context_contracts.hpp>
#include <mira/core_contracts.hpp>
#include <mira/model_contracts.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mira {

// ---------------------------------------------------------------------------
// Token counting (M4-02)
// ---------------------------------------------------------------------------

// Optional exact backend (provider count endpoint or local tokenizer). A
// failing exact counter degrades the estimate; it must never produce zero.
class IExactTokenCounter {
  public:
    virtual ~IExactTokenCounter() = default;
    [[nodiscard]] virtual Result<std::uint64_t>
    exact_count(const ContextItem &item, const ModelProfileId &profile) const = 0;
};

class ITokenCounter {
  public:
    virtual ~ITokenCounter() = default;
    [[nodiscard]] virtual Result<TokenEstimate>
    estimate_item(const ContextItem &item, const ModelProfileId &profile) const = 0;
    [[nodiscard]] virtual Result<TokenEstimate>
    estimate_tool(const ExposedToolSpec &tool, const ModelProfileId &profile) const = 0;
    // Message-boundary and dialect framing overhead for one assembled request.
    [[nodiscard]] virtual std::uint64_t
    estimate_assembly_overhead(std::size_t item_count, std::size_t part_count) const = 0;
    // Upper bound for one image part; used for the image budget cap.
    [[nodiscard]] virtual std::uint64_t image_tokens_upper(const ImagePart &image,
                                                           const ModelProfileId &profile) const = 0;
};

struct ConservativeTokenConfig final {
    // Conservative UTF-8 density: 4 bytes per token overcounts on purpose so
    // the upper bound stays safe without shipping a tokenizer per profile.
    std::uint32_t bytes_per_token = 4;
    // Vision cost model: a fixed tile floor plus a byte-proportional term.
    std::uint64_t image_floor_tokens = 256;
    std::uint64_t image_bytes_per_token = 256;
    std::uint32_t per_part_overhead_tokens = 8;
    std::uint32_t per_item_overhead_tokens = 12;
    std::uint32_t per_request_overhead_tokens = 16;
    std::uint32_t tool_schema_overhead_tokens = 24;
};

// Conservative counter with per-profile calibration. Calibrations are keyed by
// profile and never shared across profiles: tokenizer drift on one model must
// not loosen the bound of another.
class ConservativeTokenCounter final : public ITokenCounter {
  public:
    explicit ConservativeTokenCounter(ConservativeTokenConfig config = ConservativeTokenConfig{});

    void attach_exact_counter(std::shared_ptr<const IExactTokenCounter> exact);

    // Reconciles one estimate against provider-reported usage; the margin only
    // grows relative to the observed ratio and never drops below 1.0.
    void record_usage(const ModelProfileId &profile, std::uint64_t estimated_upper,
                      std::uint64_t actual_tokens);

    [[nodiscard]] std::optional<double> margin_for(const ModelProfileId &profile) const;
    [[nodiscard]] std::uint32_t exact_count_degradations() const;

    Result<TokenEstimate> estimate_item(const ContextItem &item,
                                        const ModelProfileId &profile) const override;
    Result<TokenEstimate> estimate_tool(const ExposedToolSpec &tool,
                                        const ModelProfileId &profile) const override;
    std::uint64_t estimate_assembly_overhead(std::size_t item_count,
                                             std::size_t part_count) const override;
    std::uint64_t image_tokens_upper(const ImagePart &image,
                                     const ModelProfileId &profile) const override;

  private:
    [[nodiscard]] TokenEstimate finalize(std::uint64_t lower, std::uint64_t upper,
                                         const ModelProfileId &profile) const;

    ConservativeTokenConfig config_;
    std::shared_ptr<const IExactTokenCounter> exact_;
    mutable std::mutex mutex_;
    std::map<ModelProfileId, double, std::less<>> margins_;
    mutable std::uint32_t degradations_ = 0;
};

// ---------------------------------------------------------------------------
// Context manager (M4-03/M4-04)
// ---------------------------------------------------------------------------

class IContextManager {
  public:
    virtual ~IContextManager() = default;
    [[nodiscard]] virtual Result<PreparedModelContext>
    prepare(const ContextRequest &request) const = 0;
};

// Deterministic P0-P5 context assembly. Selection, replacement, compression
// and rejection are pure functions of the request: no clock, no randomness,
// no environment access. The same request always yields the same digest.
class StandardContextManager final : public IContextManager {
  public:
    explicit StandardContextManager(std::shared_ptr<const ITokenCounter> counter);

    Result<PreparedModelContext> prepare(const ContextRequest &request) const override;

  private:
    std::shared_ptr<const ITokenCounter> counter_;
};

} // namespace mira
