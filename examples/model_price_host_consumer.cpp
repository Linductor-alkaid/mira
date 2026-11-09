#include <kairo/executor.hpp>
#include <mira/model_gateway.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace mira;
using namespace std::chrono_literals;

enum class Scenario {
    Known,
    UnknownRejected,
    UnknownAllowed,
    CostRejected,
    ResolvedUnknown,
    Missing,
    Partial,
    AdmissionRejected,
    CancelBeforeWrite,
    LateSuccess,
    LateMissing,
    AmbiguousCancelled
};

// Artificial rates, never a supplier quote: 1 micro USD / input token and
// 2 micro USD / output token. Epoch activation keeps this fixture deterministic.
PriceTable fixture_prices() {
    PriceEntry entry;
    entry.model = "fixture-priced-model";
    entry.currency = "USD";
    entry.input_micros_per_mtok = 1'000'000;
    entry.output_micros_per_mtok = 2'000'000;
    entry.version = 7;
    entry.source = "offline-fixture-not-a-supplier-quote";
    PriceTable prices;
    prices.add(std::move(entry));
    return prices;
}

class FixtureProvider final : public IModelProvider {
  public:
    FixtureProvider(Scenario scenario, std::shared_ptr<SimpleAdmissionGate> gate)
        : scenario_(scenario), gate_(std::move(gate)) {
        profile_.id = ModelProfileId::generate();
        profile_.display_name = "offline price fixture";
        profile_.version = {1, 0, 0};
        profile_.endpoint_origin = "https://price-fixture.invalid";
        profile_.api_prefix = "/v1";
        profile_.model_selector = "fixture-priced-model";
        profile_.capabilities.text = {true, CapabilityEvidence::FixtureVerified, "offline only"};
        profile_.default_data_policy.store = false;
    }
    const ModelProfile &profile() const override { return profile_; }
    Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &context,
                                const ProviderInferOptions &) override {
        ++calls;
        // A real provider must check cancellation before moving any bytes.
        if (context.cancelled())
            return make_model_error(ModelDomainCode::ModelCancelled, "cancelled before write",
                                    false, request.operation_id);
        if (scenario_ == Scenario::AmbiguousCancelled) {
            trace_.write_started = true;
            cancelled = true;
            gate_->deactivate(request.task_id);
            return make_model_error(ModelDomainCode::AmbiguousCompletion,
                                    "cancelled after write; billing requires audit", false,
                                    request.operation_id);
        }
        if (scenario_ == Scenario::LateSuccess || scenario_ == Scenario::LateMissing) {
            cancelled = true;
            gate_->deactivate(request.task_id);
        }
        ModelResponse response;
        response.contract_version = request.contract_version;
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_.id;
        response.requested_model = profile_.model_selector;
        response.resolved_model = scenario_ == Scenario::ResolvedUnknown
                                      ? "fixture-unpriced-revision"
                                      : profile_.model_selector;
        response.status = ModelCompletionStatus::Completed;
        response.usage.input_tokens = 3;
        response.usage.output_tokens = 4;
        response.usage.quality = UsageQuality::ProviderReported;
        if (scenario_ == Scenario::Missing || scenario_ == Scenario::LateMissing)
            response.usage = ModelUsage{};
        if (scenario_ == Scenario::Partial) {
            response.usage.output_tokens.reset();
            response.usage.quality = UsageQuality::Partial;
        }
        return response;
    }
    const TransportTrace &last_trace() const override { return trace_; }
    unsigned calls = 0;
    std::atomic<bool> cancelled{false};

  private:
    Scenario scenario_;
    std::shared_ptr<SimpleAdmissionGate> gate_;
    ModelProfile profile_;
    TransportTrace trace_;
};

ModelRequest request_for(const ModelProfile &profile) {
    ModelRequest request;
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.task_epoch = 1;
    request.profile_id = profile.id;
    ModelInputItem prompt;
    prompt.role = ModelRole::System;
    prompt.content.emplace_back(TextPart{"fixture"}); // Current estimate: 7 / 3 + 1 = 3.
    request.input.push_back(std::move(prompt));
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    request.generation.max_output_tokens = 4;
    request.budget.max_input_tokens = 100;
    request.budget.max_output_tokens = 4;
    request.budget.max_requests = 1;
    request.budget.max_total_cost_micros = 20;
    request.budget.currency = "USD";
    return request;
}

bool check(bool condition, const char *name) {
    if (!condition)
        std::cerr << "price host validation failed: " << name << '\n';
    return condition;
}

bool run_case(kairo::Executor &executor, Scenario scenario, const char *name) {
    auto gate = std::make_shared<SimpleAdmissionGate>();
    auto provider = std::make_shared<FixtureProvider>(scenario, gate);
    ModelRouter router;
    router.register_profile(std::make_shared<ModelProfile>(provider->profile()));
    ModelGatewayConfig config;
    config.require_known_price = scenario != Scenario::UnknownAllowed;
    config.retry_budget.max_attempts = 1;
    const bool unpriced =
        scenario == Scenario::UnknownRejected || scenario == Scenario::UnknownAllowed;
    ModelGateway gateway(executor, router, nullptr, unpriced ? PriceTable{} : fixture_prices(),
                         config);
    gateway.register_provider(provider);
    gateway.set_admission_gate(gate);
    auto events = std::make_shared<MemoryEventStore>();
    const auto session = SessionId::generate();
    gateway.set_event_store(events, RuntimeId::generate(), session);
    auto request = request_for(provider->profile());
    gate->activate(request.task_id, request.task_epoch);
    if (scenario == Scenario::CostRejected)
        request.budget.max_total_cost_micros = 10; // Fixture estimate costs 11.
    if (scenario == Scenario::AdmissionRejected)
        gate->deactivate(request.task_id);
    if (scenario == Scenario::CancelBeforeWrite)
        provider->cancelled = true;
    OperationContext context;
    context.operation = request.operation_id;
    context.session = session;
    context.task = request.task_id;
    context.task_epoch = request.task_epoch;
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + 2s;
    context.cancellation_requested = [provider] { return provider->cancelled.load(); };

    // Gateway mutation is serialized; no simultaneous infer/ledger reads.
    auto pending = executor.submit_auto([&] { return gateway.infer(request, context); });
    auto result = pending.get();
    const auto held = gateway.ledger().reserved(request.task_id);
    const auto spent = gateway.ledger().spent_micros(request.task_id);
    const bool rejected = scenario == Scenario::UnknownRejected ||
                          scenario == Scenario::CostRejected ||
                          scenario == Scenario::AdmissionRejected;
    if (rejected || scenario == Scenario::CancelBeforeWrite ||
        scenario == Scenario::AmbiguousCancelled) {
        const auto code =
            scenario == Scenario::AdmissionRejected || scenario == Scenario::CancelBeforeWrite
                ? ErrorCode::Cancelled
                : (scenario == Scenario::AmbiguousCancelled ? ErrorCode::ExecutionUncertain
                                                            : ErrorCode::ResourceExhausted);
        if (!check(!result && result.error().code == code, name) ||
            !check(provider->calls ==
                       (rejected || scenario == Scenario::CancelBeforeWrite ? 0U : 1U),
                   name) ||
            !check(spent == 0 && held.requests == 0 && held.input_tokens == 0 &&
                       held.output_tokens == 0 &&
                       held.cost_micros == (scenario == Scenario::AmbiguousCancelled ? 11U : 0U),
                   name))
            return false;
    } else {
        if (!check(result && provider->calls == 1 && result.value().reservation.has_value(), name))
            return false;
        const bool missing = scenario == Scenario::Missing || scenario == Scenario::LateMissing;
        const bool unknown =
            scenario == Scenario::UnknownAllowed || scenario == Scenario::ResolvedUnknown;
        const bool partial = scenario == Scenario::Partial;
        const auto quality = missing   ? ReconciliationQuality::MissingUsage
                             : unknown ? ReconciliationQuality::UnknownPrice
                             : partial ? ReconciliationQuality::PartialUsage
                                       : ReconciliationQuality::Reconciled;
        const bool late = scenario == Scenario::LateSuccess || scenario == Scenario::LateMissing;
        if (!check(result.value().settlement.quality == quality &&
                       result.value().settlement.reserved_remainder ==
                           (missing || unknown || partial) &&
                       result.value().admitted == !late,
                   name))
            return false;
        if (!check(spent == (missing || unknown ? 0U : (partial ? 3U : 11U)), name) ||
            !check(held.requests == (missing || unknown ? 1U : 0U) &&
                       held.input_tokens == (missing || unknown ? 3U : 0U) &&
                       held.output_tokens == (missing || unknown || partial ? 4U : 0U) &&
                       held.cost_micros ==
                           (unpriced ? 0U : (missing || unknown || partial ? 11U : 0U)),
                   name))
            return false;
        // Consume the same safe event metadata a host can expose to diagnostics.
        auto page = events->read({session, std::nullopt, 16});
        if (!check(page.has_value(), name))
            return false;
        std::optional<JsonValue> prepared, completed;
        for (const auto &event : page.value().events) {
            if (event.payload.type == "ModelRequestPrepared") {
                auto parsed = parse_json(event.payload.data);
                if (parsed)
                    prepared = std::move(parsed).value();
            } else if (event.payload.type == "ModelResponseReceived") {
                auto parsed = parse_json(event.payload.data);
                if (parsed)
                    completed = std::move(parsed).value();
            }
        }
        if (!check(prepared && completed, name))
            return false;
        const auto *pre = prepared->find("detail");
        const auto *post = completed->find("detail");
        if (!check(pre && post && pre->find("priced") && post->find("settlement_quality"), name) ||
            !check(pre->find("priced")->as_boolean() == !unpriced &&
                       post->find("settlement_quality")->as_integer() ==
                           static_cast<std::int64_t>(quality),
                   name))
            return false;
    }
    const bool known_total =
        result && result.value().settlement.quality == ReconciliationQuality::Reconciled;
    std::string cost = "unknown";
    std::string quality = "AmbiguousCompletion";
    if (rejected || scenario == Scenario::CancelBeforeWrite) {
        cost = rejected ? "not-dispatched" : "not-sent";
        quality = "Rejected";
    } else if (result) {
        switch (result.value().settlement.quality) {
        case ReconciliationQuality::Reconciled:
            quality = "Reconciled";
            cost = std::to_string(spent) + " micro USD";
            break;
        case ReconciliationQuality::MissingUsage:
            quality = "MissingUsage";
            break;
        case ReconciliationQuality::PartialUsage:
            quality = "PartialUsage";
            cost = "partial";
            break;
        case ReconciliationQuality::UnknownPrice:
            quality = "UnknownPrice";
            break;
        }
    }
    std::cout << name << ": quality=" << quality << " cost=" << cost << " reported_spent=" << spent
              << " reserved_cost=" << held.cost_micros << " reserved_input=" << held.input_tokens
              << " reserved_output=" << held.output_tokens << " reserved_requests=" << held.requests
              << " audit=" << (!rejected && !known_total && scenario != Scenario::CancelBeforeWrite)
              << '\n';
    return true;
}
} // namespace

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = config.max_threads = 2;
    if (!executor.initialize(config))
        return 1;
    bool ok = true;
    try {
        const auto price = fixture_prices().lookup("fixture-priced-model", "USD", {});
        if (!check(price && price->version == 7 &&
                       price->source == "offline-fixture-not-a-supplier-quote",
                   "fixture-price-metadata")) {
            ok = false;
        }
        const std::pair<Scenario, const char *> cases[] = {
            {Scenario::Known, "fixture-known"},
            {Scenario::UnknownRejected, "unknown-rejected"},
            {Scenario::UnknownAllowed, "unknown-allowed"},
            {Scenario::CostRejected, "cost-rejected"},
            {Scenario::ResolvedUnknown, "resolved-price-unknown"},
            {Scenario::Missing, "usage-missing"},
            {Scenario::Partial, "usage-partial"},
            {Scenario::AdmissionRejected, "admission-rejected"},
            {Scenario::CancelBeforeWrite, "cancel-before-write"},
            {Scenario::LateSuccess, "cancel-late-success"},
            {Scenario::LateMissing, "cancel-late-usage-missing"},
            {Scenario::AmbiguousCancelled, "cancel-ambiguous"}};
        std::cout << "OFFLINE FIXTURE PRICES; no supplier quote or network billing\n";
        for (const auto &[scenario, name] : cases) {
            if (!ok)
                break;
            if (!run_case(executor, scenario, name)) {
                ok = false;
                break;
            }
        }
    } catch (...) {
        std::cerr << "price host task or submission failed\n";
        ok = false;
    }
    if (executor.shutdown(true) != kairo::ShutdownResult::Completed)
        ok = false;
    return ok ? 0 : 1;
}
