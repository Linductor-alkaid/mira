#include <mira/model_gateway.hpp>
#include <mira/model_digest.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

[[nodiscard]] std::string domain_code_text(const Error &error) {
    if (error.domain == "mira.model") {
        for (auto code = ModelDomainCode::EndpointPolicyDenied;
             code <= ModelDomainCode::ModelResourceExhausted;
             code = static_cast<ModelDomainCode>(static_cast<std::int32_t>(code) + 1)) {
            if (error.domain_code == static_cast<std::int32_t>(code)) {
                return model_domain_code_name(code);
            }
        }
    }
    return "Other";
}

[[nodiscard]] Sensitivity max_request_sensitivity(const ModelRequest &request) {
    auto result = Sensitivity::Public;
    for (const auto &item : request.input) {
        for (const auto &part : item.content) {
            if (const auto *text = std::get_if<TextPart>(&part)) {
                result = std::max(result, text->sensitivity);
            } else {
                const auto &reference = std::get_if<ImagePart>(&part) != nullptr
                                            ? std::get_if<ImagePart>(&part)->source
                                            : std::get_if<FilePart>(&part)->source;
                result = std::max(result, reference.sensitivity);
            }
        }
    }
    return result;
}

} // namespace

ModelGateway::ModelGateway(executor::Executor &executor, ModelRouter router,
                           std::shared_ptr<IArtifactSource> artifacts, PriceTable prices,
                           ModelGatewayConfig config)
    : executor_(executor), router_(std::move(router)), artifacts_(std::move(artifacts)),
      config_(config), ledger_(std::move(prices)) {
    repair_policy_ = config.repair_policy;
}

void ModelGateway::register_provider(std::shared_ptr<IModelProvider> provider) {
    if (provider == nullptr) {
        return;
    }
    const auto found = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const auto &candidate) { return candidate->profile().id == provider->profile().id; });
    if (found != providers_.end()) {
        *found = std::move(provider);
        return;
    }
    providers_.push_back(std::move(provider));
}

void ModelGateway::set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                                   SessionId session) {
    events_ = std::move(events);
    runtime_ = runtime;
    session_ = session;
}

void ModelGateway::set_admission_gate(std::shared_ptr<const TaskAdmissionGate> gate) {
    admission_ = std::move(gate);
}

CircuitState ModelGateway::circuit_state(const ModelProfileId &profile) const {
    const auto found = circuits_.find(profile);
    return found == circuits_.end() ? CircuitState::Unknown : found->second.state();
}

void ModelGateway::emit(const ModelRequest &request, std::string type, JsonValue summary,
                        EventClass classification) const {
    if (events_ == nullptr) {
        return;
    }
    // Event payloads carry identifiers and digests only: prompts, screenshots
    // and raw responses never enter the event stream (design LLM API §15/§17).
    JsonValue::Object envelope;
    envelope.emplace_back("request_id", request.request_id.to_string());
    envelope.emplace_back("operation_id", request.operation_id.to_string());
    envelope.emplace_back("task_id", request.task_id.to_string());
    envelope.emplace_back("task_epoch", static_cast<std::int64_t>(request.task_epoch));
    envelope.emplace_back("profile_id", request.profile_id.to_string());
    envelope.emplace_back("detail", std::move(summary));
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime_;
    append.session_id = session_;
    append.task_id = request.task_id;
    append.payload = EventPayload{std::move(type), to_json_string(JsonValue(std::move(envelope))),
                                  classification};
    (void)events_->append(append);
}

Result<ModelCallOutcome> ModelGateway::infer(const ModelRequest &request,
                                             const OperationContext &context,
                                             const InferOptions &options) {
    if (auto valid = validate_model_request(request); !valid) {
        return valid.error();
    }
    if (admission_ != nullptr && !admission_->admit(request.task_id, request.task_epoch)) {
        return make_model_error(ModelDomainCode::ModelCancelled,
                                "request task epoch is not admitted", false,
                                request.operation_id);
    }

    RouteQuery query;
    query.needs_text = true;
    for (const auto &item : request.input) {
        for (const auto &part : item.content) {
            if (std::get_if<ImagePart>(&part) != nullptr) {
                query.needs_image_input = true;
            }
            if (std::get_if<FilePart>(&part) != nullptr) {
                query.needs_file_input = true;
            }
        }
    }
    query.needs_strict_schema = request.output_contract.mode != OutputMode::Text;
    query.needs_tools = !request.tools.empty();
    query.needs_sse = options.stream;
    query.max_sensitivity = max_request_sensitivity(request);
    query.budget = &request.budget;
    query.data_policy = &request.data_policy;

    // Candidate profiles in registration order; the first that satisfies
    // the query is primary and later ones are fallback candidates. A
    // fallback is a new RouteDecision: only retryable failures may switch,
    // never ambiguous completions, policy or admission rejections.
    std::vector<std::shared_ptr<IModelProvider>> candidates;
    for (const auto &profile : router_.profiles()) {
        if (!profile_mismatches(*profile, query).empty()) {
            continue;
        }
        const auto provider = std::find_if(
            providers_.begin(), providers_.end(),
            [&](const auto &candidate) { return candidate->profile().id == profile->id; });
        if (provider != providers_.end()) {
            candidates.push_back(*provider);
        }
    }
    if (candidates.empty()) {
        auto route = router_.route(query);
        return route.error();
    }
    const auto selected = std::make_shared<ModelProfile>(candidates.front()->profile());

    // Budget reservation with a conservative estimate before any bytes move.
    BudgetEstimate estimate;
    estimate.input_tokens = estimate_input_tokens(request);
    std::uint64_t estimated_output = request.generation.max_output_tokens.value_or(
        selected->capabilities.limits.max_output_tokens);
    if (request.budget.max_output_tokens != 0 &&
        estimated_output > request.budget.max_output_tokens) {
        estimated_output = request.budget.max_output_tokens;
    }
    estimate.output_tokens = estimated_output;
    const auto priced = ledger_.prices().lookup(
        selected->model_selector,
        request.budget.currency.empty() ? std::string("USD") : request.budget.currency,
        std::chrono::system_clock::now());
    if (priced.has_value()) {
        estimate.cost_micros = (estimate.input_tokens * priced->input_micros_per_mtok +
                                estimate.output_tokens * priced->output_micros_per_mtok +
                                999'999) /
                               1'000'000;
        estimate.currency = priced->currency;
    } else if (config_.require_known_price) {
        return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                "price table has no entry for the model and unknown costs are "
                                "rejected by policy",
                                false, request.operation_id);
    }
    auto reservation = ledger_.reserve(request.task_id, request.budget, estimate);
    if (!reservation) {
        return reservation.error();
    }

    emit(request, "ModelRequestPrepared",
         JsonValue::Object{{"profile_digest", selected->profile_digest().to_string()},
                           {"estimated_input_tokens",
                            static_cast<std::int64_t>(estimate.input_tokens)},
                           {"estimated_output_tokens",
                            static_cast<std::int64_t>(estimate.output_tokens)},
                           {"priced", JsonValue(priced.has_value())},
                           {"candidate_profiles",
                            static_cast<std::int64_t>(candidates.size())}},
         EventClass::Diagnostic);

    ProviderSupervisor supervisor;
    const auto attempt_started = std::chrono::steady_clock::now();
    Error last_failure = make_model_error(ModelDomainCode::CapabilityMismatch,
                                          "no candidate profile executed", false,
                                          request.operation_id);
    std::uint32_t total_attempts = 0;

    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
        const auto provider = candidates[candidate_index];
        const auto &profile = provider->profile();
        auto &circuit = circuits_[profile.id];
        if (circuit.state() == CircuitState::Unknown) {
            circuit = ProviderCircuit(config_.circuit_config);
        }
        if (!circuit.admits_requests()) {
            last_failure = make_model_error(ModelDomainCode::ProviderOverloaded,
                                            "provider circuit is open", false,
                                            request.operation_id);
            continue; // Open circuits fall through to the next candidate.
        }

        ModelCallOutcome outcome;
        outcome.route.selected_profile = profile.id;
        outcome.route.dialect = profile.dialect;
        outcome.route.profile_digest = profile.profile_digest();
        outcome.route.evidence = query.min_evidence;
        outcome.reservation = reservation.value();

        for (std::uint32_t attempt = 1;; ++attempt) {
        outcome.attempts = ++total_attempts;
        outcome.attempts = attempt;
        ModelRequest attempt_request = request;
        attempt_request.request_id = ModelRequestId::generate();

        ProviderInferOptions provider_options;
        provider_options.stream = options.stream;
        provider_options.capture_raw_response = options.capture_raw_response;
        auto response = provider->infer(attempt_request, context, provider_options);
        if (response) {
            outcome.response = std::move(response).value();
            outcome.sse_stats = provider->last_sse_stats();
            circuit.record_success();
            outcome.wire_request_digest = model_request_canonical_digest(attempt_request);

            auto settlement = ledger_.reconcile(
                request.task_id, request.budget, outcome.response.usage,
                outcome.response.resolved_model.value_or(selected->model_selector));
            if (settlement) {
                outcome.settlement = std::move(settlement).value();
            }

            outcome.parse = parse_decision(attempt_request, outcome.response);
            if (outcome.parse.outcome == DecisionParseOutcome::ToolProposals) {
                auto batch = resolve_tool_calls(attempt_request, outcome.response);
                if (!batch) {
                    outcome.parse.outcome = DecisionParseOutcome::Ambiguous;
                    outcome.parse.safe_summary = batch.error().safe_message;
                } else {
                    outcome.tool_proposals = std::move(batch).value();
                }
            }
            if (admission_ != nullptr &&
                !admission_->admit(request.task_id, request.task_epoch)) {
                // The response settles as a late diagnostic; no decision or
                // action follows from it.
                outcome.admitted = false;
                outcome.rejection_reason = "task epoch or lifecycle rejected the completion";
            }
            emit(request, "ModelResponseReceived",
                 JsonValue::Object{{"attempts", static_cast<std::int64_t>(attempt)},
                                   {"admitted", outcome.admitted},
                                   {"parse_outcome",
                                    static_cast<std::int64_t>(
                                        static_cast<std::uint8_t>(outcome.parse.outcome))},
                                   {"settlement_quality",
                                    static_cast<std::int64_t>(
                                        static_cast<std::uint8_t>(outcome.settlement.quality))}},
                 EventClass::State);
            return outcome;
        }

        const auto failure = response.error();
        circuit.record_failure();
        outcome.wire_request_digest = model_request_canonical_digest(attempt_request);

        emit(request, "ModelAttemptFailed",
             JsonValue::Object{{"code", domain_code_text(failure)},
                               {"stage",
                                std::string(classify_provider_stage(provider->last_trace(),
                                                                    failure) ==
                                                RequestStage::PreWriteFailure
                                                    ? "pre-write"
                                                    : "post-write")},
                               {"attempt", static_cast<std::int64_t>(attempt)}},
             EventClass::Diagnostic);

        RetryBudget budget = config_.retry_budget;
        budget.attempts_used = attempt;
        budget.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - attempt_started);
        const auto decision = supervisor.evaluate(failure,
                                                  classify_provider_stage(
                                                      provider->last_trace(), failure),
                                                  provider->last_retry_after_hint(), budget,
                                                  circuit);
        if (decision.action == RetryAction::GiveUp) {
            // Bounded provider fallback: only retryable, non-ambiguous
            // failures may switch profiles, and only while candidates and
            // the retry time budget remain.
            const bool ambiguous = failure.domain == "mira.model" &&
                                   failure.domain_code ==
                                       static_cast<std::int32_t>(
                                           ModelDomainCode::AmbiguousCompletion);
            const bool fallback_eligible =
                failure.retryable && !ambiguous && failure.code != ErrorCode::Cancelled &&
                candidate_index + 1 < candidates.size() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - attempt_started) <
                    config_.retry_budget.total_budget;
            last_failure = failure;
            if (fallback_eligible) {
                emit(request, "ModelProfileFallback",
                     JsonValue::Object{{"failed_profile", profile.id.to_string()},
                                       {"code", domain_code_text(failure)}},
                     EventClass::Diagnostic);
                break; // Try the next candidate profile.
            }
            // Reservations for calls that never completed are released.
            // Ambiguous completions may still have been billed, so only the
            // cost part of the reservation stays held for audit.
            BudgetReservation to_release = reservation.value();
            if (ambiguous) {
                // Keep only the cost part held; tokens and request count are
                // released so later admissions are not blocked.
                to_release.cost_micros = 0;
            }
            (void)ledger_.release(request.task_id, to_release);
            // The original stable error is returned untouched; the retry
            // context is already recorded as an event.
            return failure;
        }
        if (decision.action == RetryAction::RetryAfter && decision.delay.count() > 0) {
            // Retry pacing runs through an Executor timer so shutdown and
            // cancellation can interrupt the wait.
            auto sleeper = executor_.submit_delayed_with_handle(
                static_cast<std::int64_t>(decision.delay.count()), [] {});
            if (sleeper.future.valid()) {
                try {
                    sleeper.future.get();
                } catch (...) {
                    // Timer cancellation ends the wait; the loop re-checks.
                }
            }
            if (context.cancelled()) {
                return make_model_error(ModelDomainCode::ModelCancelled,
                                        "model call was cancelled during retry backoff", false,
                                        request.operation_id);
            }
        }
        }
    }
    return last_failure;
}

} // namespace mira
