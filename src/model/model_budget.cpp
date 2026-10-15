#include <mira/model_budget.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error budget_error(std::string message) {
    Error error;
    error.code = ErrorCode::ResourceExhausted;
    error.domain = "mira.model";
    error.domain_code = static_cast<std::int32_t>(ModelDomainCode::ModelResourceExhausted);
    error.safe_message = std::move(message);
    return error;
}

[[nodiscard]] std::uint64_t price_tokens(std::uint64_t tokens, std::uint64_t micros_per_mtok) {
    // Ceiling division so estimates never round a real cost down.
    if (tokens == 0) {
        return 0;
    }
    return (tokens * micros_per_mtok + 999'999ULL) / 1'000'000ULL;
}

} // namespace

void PriceTable::add(PriceEntry entry) {
    entries_.push_back(std::move(entry));
    ++version_;
}

std::optional<PriceEntry> PriceTable::lookup(const std::string &model, const std::string &currency,
                                             std::chrono::system_clock::time_point at) const {
    std::optional<PriceEntry> best;
    for (const auto &entry : entries_) {
        if (entry.model != model || entry.currency != currency || entry.effective_from > at) {
            continue;
        }
        if (!best.has_value() || entry.effective_from >= best->effective_from) {
            best = entry;
        }
    }
    return best;
}

std::uint64_t estimate_input_tokens(const ModelRequest &request) {
    std::uint64_t total = 0;
    for (const auto &item : request.input) {
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                // Conservative upper bound: bytes / 3 (UTF-8 multi-byte text
                // dominates; framing overhead is absorbed by the margin).
                total += static_cast<std::uint64_t>(text->text.size() / 3) + 1;
            } else if (const auto *image = std::get_if<ImagePart>(&part)) {
                // Provisional vision estimate until a provider-verified count
                // exists: 1600 tokens per image, consistent with low-detail
                // tile accounting.
                static_cast<void>(image);
                total += 1'600;
            } else {
                total += 64;
            }
        }
    }
    for (const auto &tool : request.tools) {
        total +=
            static_cast<std::uint64_t>(to_json_string(tool.parameters_schema.root).size() / 3) + 32;
    }
    return total;
}

BudgetLedger::BudgetLedger(PriceTable prices) : prices_(std::move(prices)) {}

Result<BudgetReservation> BudgetLedger::reserve(const TaskId &task, const ModelBudget &budget,
                                                const BudgetEstimate &estimate) {
    auto &account = accounts_[task];
    if (account.closed) {
        return budget_error("task budget is closed");
    }
    if (budget.scope != ModelBudgetScope::Task && budget.scope != ModelBudgetScope::Request) {
        return budget_error("invalid budget scope");
    }
    if (budget.scope == ModelBudgetScope::Request &&
        (budget.max_requests != 1 || budget.max_output_tokens == 0 || estimate.requests != 1)) {
        return budget_error("request budget requires one infer and finite output tokens");
    }
    const auto add = [](std::uint64_t left, std::uint64_t right) {
        return right > std::numeric_limits<std::uint64_t>::max() - left
                   ? std::numeric_limits<std::uint64_t>::max()
                   : left + right;
    };
    BudgetReservation next;
    next.input_tokens = add(account.reserved.input_tokens, estimate.input_tokens);
    next.output_tokens = add(account.reserved.output_tokens, estimate.output_tokens);
    next.requests = add(account.reserved.requests, estimate.requests);
    next.cost_micros = add(account.reserved.cost_micros, estimate.cost_micros);
    if (budget.scope == ModelBudgetScope::Task &&
        (estimate.input_tokens >
             std::numeric_limits<std::uint64_t>::max() - account.reserved.input_tokens ||
         estimate.output_tokens >
             std::numeric_limits<std::uint64_t>::max() - account.reserved.output_tokens ||
         estimate.requests >
             std::numeric_limits<std::uint64_t>::max() - account.reserved.requests ||
         estimate.cost_micros >
             std::numeric_limits<std::uint64_t>::max() - account.reserved.cost_micros)) {
        return budget_error("task reservation counters would overflow");
    }
    const bool per_request = budget.scope == ModelBudgetScope::Request;
    const auto checked_input = per_request ? estimate.input_tokens : next.input_tokens;
    const auto checked_output = per_request ? estimate.output_tokens : next.output_tokens;
    const auto checked_requests = per_request ? estimate.requests : next.requests;
    const auto checked_cost = per_request ? estimate.cost_micros : next.cost_micros;
    if (budget.max_input_tokens != 0 && checked_input > budget.max_input_tokens) {
        return budget_error("input token reservation exceeds the task budget");
    }
    if (budget.max_output_tokens != 0 && checked_output > budget.max_output_tokens) {
        return budget_error("output token reservation exceeds the task budget");
    }
    if (budget.max_requests != 0 && checked_requests > budget.max_requests) {
        return budget_error("request reservation exceeds the task budget");
    }
    if (budget.max_total_cost_micros != 0 && checked_cost > budget.max_total_cost_micros) {
        return budget_error("cost reservation exceeds the task budget");
    }
    account.reserved = next;
    return per_request ? BudgetReservation{estimate.input_tokens, estimate.output_tokens,
                                           estimate.requests, estimate.cost_micros}
                       : next;
}

Result<BudgetSettlement> BudgetLedger::reconcile(const TaskId &task, const ModelBudget &budget,
                                                 const ModelUsage &reported_usage,
                                                 const std::string &model) {
    auto found = accounts_.find(task);
    if (found == accounts_.end()) {
        return budget_error("task has no open budget account");
    }
    auto &account = found->second;
    const auto now = std::chrono::system_clock::now();
    const auto price =
        prices_.lookup(model, budget.currency.empty() ? std::string("USD") : budget.currency, now);

    BudgetSettlement settlement;
    settlement.input_tokens = reported_usage.input_tokens.value_or(0);
    settlement.output_tokens = reported_usage.output_tokens.value_or(0);

    if (reported_usage.quality == UsageQuality::Missing) {
        settlement.quality = ReconciliationQuality::MissingUsage;
        settlement.reserved_remainder = true;
        settlement.note = "provider reported no usage; reservation held for audit";
        return settlement;
    }

    if (!price.has_value()) {
        settlement.quality = ReconciliationQuality::UnknownPrice;
        settlement.reserved_remainder = true;
        settlement.note = "price table has no entry for the resolved model";
        return settlement;
    }

    std::uint64_t cost = price_tokens(settlement.input_tokens, price->input_micros_per_mtok) +
                         price_tokens(settlement.output_tokens, price->output_micros_per_mtok);
    if (reported_usage.cached_input_tokens.has_value()) {
        const auto cached = *reported_usage.cached_input_tokens;
        if (cached > settlement.input_tokens) {
            settlement.quality = ReconciliationQuality::PartialUsage;
            settlement.reserved_remainder = true;
            settlement.note = "cached token count exceeds reported input tokens";
            return settlement;
        }
        const auto discount = price_tokens(cached, price->input_micros_per_mtok -
                                                       price->cached_input_micros_per_mtok);
        cost -= std::min(discount, cost);
    }
    if (reported_usage.reasoning_tokens.has_value()) {
        const auto reasoning = *reported_usage.reasoning_tokens;
        if (reasoning > settlement.output_tokens) {
            settlement.quality = ReconciliationQuality::PartialUsage;
            settlement.reserved_remainder = true;
            settlement.note = "reasoning token count exceeds reported output tokens";
            return settlement;
        }
    }
    if (reported_usage.quality == UsageQuality::Partial) {
        settlement.quality = ReconciliationQuality::PartialUsage;
        settlement.reserved_remainder = true;
        settlement.note = "usage was reported partially; remainder stays reserved";
    }
    settlement.reconciled_cost_micros = cost;
    account.spent_micros = cost > std::numeric_limits<std::uint64_t>::max() - account.spent_micros
                               ? std::numeric_limits<std::uint64_t>::max()
                               : account.spent_micros + cost;

    // Release the reservation for this request proportional to the counters
    // that were actually reported.
    const auto release_requests = 1;
    account.reserved.requests = account.reserved.requests > release_requests
                                    ? account.reserved.requests - release_requests
                                    : 0;
    const auto release_input = settlement.input_tokens;
    account.reserved.input_tokens = account.reserved.input_tokens > release_input
                                        ? account.reserved.input_tokens - release_input
                                        : 0;
    const auto release_output = settlement.output_tokens;
    account.reserved.output_tokens = account.reserved.output_tokens > release_output
                                         ? account.reserved.output_tokens - release_output
                                         : 0;
    if (settlement.quality == ReconciliationQuality::Reconciled) {
        const auto release_cost = cost;
        account.reserved.cost_micros = account.reserved.cost_micros > release_cost
                                           ? account.reserved.cost_micros - release_cost
                                           : 0;
    }
    return settlement;
}

Result<void> BudgetLedger::release(const TaskId &task, const BudgetReservation &reservation) {
    auto found = accounts_.find(task);
    if (found == accounts_.end()) {
        return budget_error("task has no open budget account");
    }
    auto &held = found->second.reserved;
    held.input_tokens = held.input_tokens > reservation.input_tokens
                            ? held.input_tokens - reservation.input_tokens
                            : 0;
    held.output_tokens = held.output_tokens > reservation.output_tokens
                             ? held.output_tokens - reservation.output_tokens
                             : 0;
    held.requests = held.requests > reservation.requests ? held.requests - reservation.requests : 0;
    held.cost_micros =
        held.cost_micros > reservation.cost_micros ? held.cost_micros - reservation.cost_micros : 0;
    return Result<void>{};
}

BudgetReservation BudgetLedger::reserved(const TaskId &task) const {
    const auto found = accounts_.find(task);
    return found == accounts_.end() ? BudgetReservation{} : found->second.reserved;
}

std::uint64_t BudgetLedger::spent_micros(const TaskId &task) const {
    const auto found = accounts_.find(task);
    return found == accounts_.end() ? 0 : found->second.spent_micros;
}

void BudgetLedger::close(const TaskId &task) {
    const auto found = accounts_.find(task);
    if (found != accounts_.end()) {
        found->second.closed = true;
    }
}

} // namespace mira
