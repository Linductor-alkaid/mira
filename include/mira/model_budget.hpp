#pragma once

#include <mira/model_contracts.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mira {

// One versioned price entry. Prices are micro currency units per million
// tokens; an absent entry means the cost is unknown, never zero.
struct PriceEntry final {
    std::string model;
    std::string currency = "USD";
    std::uint64_t input_micros_per_mtok = 0;
    std::uint64_t output_micros_per_mtok = 0;
    std::uint64_t cached_input_micros_per_mtok = 0;
    std::uint64_t reasoning_micros_per_mtok = 0;
    std::uint64_t version = 1;
    std::chrono::system_clock::time_point effective_from{};
    std::string source;
};

class PriceTable final {
  public:
    void add(PriceEntry entry);
    [[nodiscard]] std::optional<PriceEntry> lookup(const std::string &model,
                                                   const std::string &currency,
                                                   std::chrono::system_clock::time_point at) const;
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  private:
    std::vector<PriceEntry> entries_;
    std::uint64_t version_ = 1;
};

// Conservative token estimate used for admission; Mira ships no tokenizer in
// M3, so estimates are upper bounds (bytes / 3) rather than exact counts.
[[nodiscard]] std::uint64_t estimate_input_tokens(const ModelRequest &request);

struct BudgetEstimate final {
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint32_t requests = 1;
    std::uint64_t cost_micros = 0;
    std::string currency = "USD";
    UsageQuality quality = UsageQuality::Estimated;
};

struct BudgetReservation final {
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint32_t requests = 0;
    std::uint64_t cost_micros = 0;
};

enum class ReconciliationQuality : std::uint8_t {
    Reconciled,      // Provider usage priced against the table.
    MissingUsage,    // Reservation held; flagged for audit.
    PartialUsage,    // Subset of counters; remainder stays reserved.
    UnknownPrice,    // Usage known but the price table has no entry.
};

struct BudgetSettlement final {
    ReconciliationQuality quality = ReconciliationQuality::Reconciled;
    std::uint64_t reconciled_cost_micros = 0;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    // True when the reservation could not be fully released.
    bool reserved_remainder = false;
    std::string note;
};

// Per-task budget ledger. Reservations are upper bounds taken before
// dispatch; reported usage reconciles them afterwards. Missing usage keeps
// the reservation and flags an audit instead of inventing a zero cost.
class BudgetLedger final {
  public:
    explicit BudgetLedger(PriceTable prices);

    // Reserves against the task budget; fails with ResourceExhausted when the
    // estimate exceeds what remains.
    [[nodiscard]] Result<BudgetReservation> reserve(const TaskId &task, const ModelBudget &budget,
                                                    const BudgetEstimate &estimate);
    [[nodiscard]] Result<BudgetSettlement> reconcile(const TaskId &task,
                                                     const ModelBudget &budget,
                                                     const ModelUsage &reported_usage,
                                                     const std::string &model);
    // Releases a reservation for a call that never produced a response.
    // Ambiguous completions keep their reservation (billing may have
    // happened); the caller decides based on the failure class.
    [[nodiscard]] Result<void> release(const TaskId &task, const BudgetReservation &reservation);
    [[nodiscard]] const PriceTable &prices() const noexcept { return prices_; }
    [[nodiscard]] BudgetReservation reserved(const TaskId &task) const;
    [[nodiscard]] std::uint64_t spent_micros(const TaskId &task) const;
    // Releases reservations for tasks that will issue no further requests.
    void close(const TaskId &task);

  private:
    struct Account final {
        BudgetReservation reserved;
        std::uint64_t spent_micros = 0;
        bool closed = false;
    };

    PriceTable prices_;
    std::map<TaskId, Account> accounts_;
};

} // namespace mira
