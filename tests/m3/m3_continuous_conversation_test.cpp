#include <kairo/executor.hpp>

#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mira;
using namespace mira::testing;

ConversationLoopConfig continuous_config() {
    ConversationLoopConfig config;
    config.limit_mode = ConversationLimitMode::Continuous;
    config.max_turns = 0;
    config.max_tool_executions = 0;
    config.max_history_items = 8;
    config.max_retained_turns = 4;
    return config;
}

ModelInputItem summary_item() {
    ModelInputItem item;
    item.role = ModelRole::User;
    item.content.emplace_back(TextPart{"Settled previous rounds; keep the task goal."});
    return item;
}

// A host compactor retains a summary and the latest complete call/result pair.
Result<std::vector<ModelInputItem>> compact(const std::vector<ModelInputItem> &history) {
    if (history.size() <= 2)
        return history;
    return std::vector<ModelInputItem>{summary_item(), history[history.size() - 2], history.back()};
}

void seed_rounds(ConversationFixture &fixture, std::size_t rounds) {
    std::vector<ModelResponse> responses;
    for (std::size_t index = 0; index < rounds; ++index) {
        responses.push_back(tool_call_response(fixture.echo_spec(), R"({"message":"progress"})",
                                               "call-" + std::to_string(index)));
    }
    responses.push_back(text_response("finished"));
    fixture.use_provider(std::move(responses));
}

int long_run_compacts_owned_history_and_retains_recent_records() {
    ConversationFixture fixture;
    seed_rounds(fixture, 1024);
    auto config = continuous_config();
    config.max_output_tokens_per_turn = 16'384;
    auto loop = fixture.make_loop(config);
    std::size_t max_seen_items = 0;
    loop.set_history_transformer([&](const auto &history) {
        max_seen_items = std::max(max_seen_items, history.size());
        return compact(history);
    });
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    const auto &value = result.value();
    MIRA_CHECK(value.answer == "finished");
    MIRA_CHECK(value.total_turns == 1025 && value.tool_executions == 1024);
    MIRA_CHECK(value.turns.size() == 4 && value.turn_records_dropped == 1021);
    MIRA_CHECK(value.turns.front().turn == 1022 && value.turns.back().turn == 1025);
    MIRA_CHECK(value.retained_history_items == 3);
    MIRA_CHECK(value.retained_history_bytes < 4096);
    MIRA_CHECK(max_seen_items == 5); // proves compaction commits, not request-only
    MIRA_CHECK(value.history_transforms == 1024 && value.history_transform_degradations == 0);
    MIRA_CHECK(fixture.seen_arguments().size() == 1024);
    MIRA_CHECK(fixture.registry_->retained_dispatch_count() == 0);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1025);
    for (const auto &request : requests) {
        MIRA_CHECK(request.task_id == fixture.spec_.task_id && request.task_epoch == 1);
        MIRA_CHECK(request.budget.scope == ModelBudgetScope::Request);
        MIRA_CHECK(request.budget.max_requests == 1 && request.budget.max_output_tokens == 16'384);
        MIRA_CHECK(request.input.size() <= 5);
        MIRA_CHECK(validate_model_request(request));
    }
    // Unknown price/missing usage still retains accounting without turning it
    // into a hidden accumulated request/output-token cutoff.
    MIRA_CHECK(fixture.gateway_->ledger().reserved(fixture.spec_.task_id).requests == 1025);
    return 0;
}

int budget_scope_codec_and_validation() {
    ConversationFixture fixture;
    fixture.use_provider({text_response("done")});
    auto loop = fixture.make_loop(continuous_config());
    MIRA_CHECK(loop.run(fixture.spec_, conversation_context()));
    auto request = fixture.provider_->requests().front();
    auto encoded = model_request_to_json(request);
    const auto restored = model_request_from_json(encoded);
    MIRA_CHECK(restored && restored.value().budget.scope == ModelBudgetScope::Request);
    MIRA_CHECK(model_request_canonical_digest(restored.value()) ==
               model_request_canonical_digest(request));
    encoded.find("budget")->set("scope", "unknown");
    MIRA_CHECK(!model_request_from_json(encoded));
    encoded.find("budget")->set("scope", true);
    MIRA_CHECK(!model_request_from_json(encoded));
    request.contract_version = SchemaVersion{1, 0};
    MIRA_CHECK(!validate_model_request(request));
    request.contract_version = SchemaVersion{2, 0};
    request.generation.max_output_tokens = request.budget.max_output_tokens + 1;
    MIRA_CHECK(!validate_model_request(request));
    request.generation.max_output_tokens = request.budget.max_output_tokens;
    request.budget.max_requests = 0;
    MIRA_CHECK(!validate_model_request(request));
    request.budget.max_requests = 1;
    request.budget.max_output_tokens = 0;
    MIRA_CHECK(!validate_model_request(request));
    request.budget.scope = ModelBudgetScope::Task;
    MIRA_CHECK(model_request_to_json(request).find("budget")->find("scope") == nullptr);
    return 0;
}

int finite_zero_and_limits_remain_explicit() {
    for (const bool zero_turn : {true, false}) {
        ConversationFixture fixture;
        seed_rounds(fixture, 2);
        ConversationLoopConfig config;
        if (zero_turn)
            config.max_turns = 0;
        else
            config.max_tool_executions = 0;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result);
        MIRA_CHECK(result.value().outcome ==
                   (zero_turn ? ConversationOutcome::MaxTurns : ConversationOutcome::Failed));
        MIRA_CHECK(fixture.seen_arguments().empty());
        MIRA_CHECK(fixture.provider_->requests().size() == (zero_turn ? 0 : 1));
    }
    ConversationFixture fixture;
    seed_rounds(fixture, 3);
    ConversationLoopConfig config;
    config.max_turns = 2;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::MaxTurns);
    MIRA_CHECK(result.value().total_turns == 2 && result.value().tool_executions == 2);
    MIRA_CHECK(fixture.registry_->retained_dispatch_count() == 2);
    return 0;
}

int history_capacity_and_transform_degradation_are_visible() {
    for (const int shape : {0, 1, 2, 3, 4, 5, 6}) {
        ConversationFixture fixture;
        seed_rounds(fixture, 5);
        auto config = continuous_config();
        config.max_history_items = 2;
        if (shape == 6)
            config.max_history_bytes = 1;
        auto loop = fixture.make_loop(config);
        if (shape != 0 && shape != 6) {
            loop.set_history_transformer(
                [shape](const auto &history) -> Result<std::vector<ModelInputItem>> {
                    if (shape == 1)
                        throw std::runtime_error("private-error-text");
                    if (shape == 2)
                        return make_model_error(ModelDomainCode::InvalidModelRequest,
                                                "transform failed");
                    if (shape == 3)
                        return std::vector<ModelInputItem>{};
                    if (shape == 4)
                        return std::vector<ModelInputItem>{history.back()}; // orphan result
                    return std::vector<ModelInputItem>{summary_item()};     // drops protected round
                });
        }
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().safe_summary == "history capacity exhausted after compaction");
        MIRA_CHECK(fixture.seen_arguments().size() == (shape == 6 ? 0 : 2));
        MIRA_CHECK(fixture.provider_->requests().size() == (shape == 6 ? 0 : 2));
        if (shape > 0 && shape < 6)
            MIRA_CHECK(result.value().history_transform_degradations == 2);
    }
    return 0;
}

int batched_signed_thinking_and_recent_round_are_preserved() {
    ConversationFixture fixture(ProtocolDialect::AnthropicMessagesV1);
    const auto first = tool_call_response(fixture.echo_spec(), R"({"message":"a"})", "call-a");
    const auto second = tool_call_response(fixture.echo_spec(), R"({"message":"b"})", "call-b");
    auto round = first;
    round.output.clear();
    // Build in order: vector<variant>::insert at begin triggers a GCC 13
    // -O3 maybe-uninitialized diagnostic in the variant move machinery.
    round.output.emplace_back(ThinkingPart{"signed plan", "signature", false});
    round.output.push_back(first.output.front());
    round.output.push_back(second.output.front());
    round.output.push_back(second.output.front()); // bridge collapses identical duplicate
    fixture.use_provider({round, round, text_response("done")});
    auto loop = fixture.make_loop(continuous_config());
    loop.set_history_transformer([](const auto &history) -> Result<std::vector<ModelInputItem>> {
        if (history.size() <= 3)
            return history;
        return std::vector<ModelInputItem>{summary_item(), history[history.size() - 3],
                                           history[history.size() - 2], history.back()};
    });
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().tool_executions == 4 &&
               result.value().history_transform_degradations == 0);
    const auto requests = fixture.provider_->requests();
    const auto &assistant = requests.back().input[3];
    MIRA_CHECK(std::get<ThinkingPart>(assistant.content.front()).signature == "signature");
    MIRA_CHECK(assistant.content.size() == 3);
    MIRA_CHECK(requests.back().input.size() == 6);
    NullArtifactSource artifacts;
    MIRA_CHECK(AnthropicMessagesV1Mapper{}.encode_request(requests.back(), *fixture.profile_, false,
                                                          artifacts));
    return 0;
}

int per_round_capacity_rejects_entire_batch() {
    ConversationFixture fixture;
    auto round = tool_call_response(fixture.echo_spec(), R"({"message":"a"})", "call-a");
    const auto other = tool_call_response(fixture.echo_spec(), R"({"message":"b"})", "call-b");
    round.output.push_back(other.output.front());
    fixture.use_provider({round});
    auto config = continuous_config();
    config.max_tools_per_turn = 1;
    auto loop = fixture.make_loop(config);
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary == "per-turn tool capacity exhausted");
    MIRA_CHECK(fixture.seen_arguments().empty());
    return 0;
}

int cancellation_deadline_and_late_admission_never_dispatch() {
    for (const int path : {0, 1, 2, 3}) {
        ConversationFixture fixture;
        seed_rounds(fixture, 2);
        auto config = continuous_config();
        bool cancelled = path == 0;
        auto context = conversation_context([&] { return cancelled; });
        if (path == 1)
            context.deadline = std::chrono::steady_clock::now();
        if (path >= 2) {
            config.inference.stream = true;
            config.inference.preview_sink = [&](const auto &, const auto &) {
                if (path == 2)
                    cancelled = true;
                else
                    fixture.admission_->deactivate(fixture.spec_.task_id);
            };
        }
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, context);
        MIRA_CHECK(result &&
                   result.value().outcome ==
                       (path == 1 ? ConversationOutcome::Failed : ConversationOutcome::Cancelled));
        MIRA_CHECK(fixture.seen_arguments().empty());
        MIRA_CHECK(fixture.provider_->requests().size() == (path < 2 ? 0 : 1));
    }
    return 0;
}

int cancellation_between_tools_and_executor_shutdown() {
    ConversationFixture fixture;
    bool cancelled = false;
    BuiltinToolSpec stop = fixture.echo_spec();
    stop.tool_id = ToolId::generate();
    stop.wire_name = "stop";
    MIRA_CHECK(fixture.registry_->register_tool(
        stop, [&](const auto &, const auto &) -> Result<JsonValue> {
            cancelled = true;
            return JsonValue{true};
        }));
    auto round = tool_call_response(stop, R"({"message":"a"})", "stop-a");
    const auto other = tool_call_response(fixture.echo_spec(), R"({"message":"b"})", "echo-b");
    round.output.push_back(other.output.front());
    fixture.use_provider({round});
    auto loop = fixture.make_loop(continuous_config());
    const auto result = loop.run(fixture.spec_, conversation_context([&] { return cancelled; }));
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Cancelled);
    MIRA_CHECK(result.value().tool_executions == 1 && fixture.seen_arguments().empty());

    ConversationFixture async_fixture;
    seed_rounds(async_fixture, 20);
    auto async_loop = async_fixture.make_loop(continuous_config());
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto task = async_fixture.executor_.submit_cancellable([&](kairo::StopToken token) {
        async_loop.set_model_input_supplier(
            [&](const auto &) -> Result<std::vector<ModelInputItem>> {
                entered.set_value();
                release_future.wait_for(std::chrono::seconds{2});
                return std::vector<ModelInputItem>{};
            });
        return async_loop.run(async_fixture.spec_,
                              conversation_context([&] { return token.stop_requested(); }));
    });
    MIRA_CHECK(task.future.valid());
    const bool running =
        entered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    const auto cancellation = async_fixture.executor_.request_task_cancel(task.handle);
    release.set_value();
    MIRA_CHECK(running && cancellation.accepted());
    const auto stopped = task.future.get();
    MIRA_CHECK(stopped && stopped.value().outcome == ConversationOutcome::Cancelled);
    MIRA_CHECK(async_fixture.executor_.shutdown(true) == kairo::ShutdownResult::Completed);
    return 0;
}

int recovery_output_limit_and_tool_errors_remain_bounded() {
    for (const int path : {0, 1, 2, 3}) {
        ConversationFixture fixture;
        auto config = continuous_config();
        if (path <= 1) {
            auto response = text_response(path == 0 ? "" : "partial");
            if (path == 1) {
                response.status = ModelCompletionStatus::Incomplete;
                response.incomplete_reason = IncompleteReason::MaxOutputTokens;
            }
            fixture.use_provider({response, response, response, text_response("never")});
        } else if (path == 2) {
            // Invalid arguments are an executed failed record, then visible to
            // the next model; no hidden retry by the dispatch path.
            fixture.use_provider({tool_call_response(fixture.echo_spec(), R"({"extra":true})"),
                                  text_response("handled the error")});
        } else {
            fixture.use_flaky_provider(
                {make_model_error(ModelDomainCode::AuthenticationFailed, "auth rejected", false)},
                {});
        }
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result);
        if (path == 0) {
            MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed &&
                       result.value().recoveries == 2);
            MIRA_CHECK(fixture.provider_->requests().size() == 3);
        } else if (path == 1) {
            MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed &&
                       result.value().output_continuations == 2);
            MIRA_CHECK(result.value().safe_summary.find("output-limit") != std::string::npos);
            MIRA_CHECK(fixture.provider_->requests().size() == 3);
        } else if (path == 2) {
            MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered &&
                       result.value().tool_executions == 1);
            MIRA_CHECK(fixture.seen_arguments().empty());
            const auto requests = fixture.provider_->requests();
            MIRA_CHECK(
                std::get<ToolResultPart>(requests.back().input.back().content.front()).failed);
        } else {
            MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed &&
                       result.value().recoveries == 0);
            MIRA_CHECK(fixture.flaky_infer_calls() == 1);
        }
    }
    return 0;
}

int successful_tool_round_resets_failure_and_continuation_episodes() {
    for (const bool cutoff : {false, true}) {
        ConversationFixture fixture;
        std::vector<ModelResponse> responses;
        for (int episode = 0; episode < 4; ++episode) {
            auto response = text_response(cutoff ? "partial" : "");
            if (cutoff) {
                response.status = ModelCompletionStatus::Incomplete;
                response.incomplete_reason = IncompleteReason::MaxOutputTokens;
            }
            responses.push_back(response);
            responses.push_back(tool_call_response(fixture.echo_spec(), R"({"message":"progress"})",
                                                   "call-" + std::to_string(episode)));
        }
        responses.push_back(text_response("done"));
        fixture.use_provider(responses);
        auto config = continuous_config();
        config.max_recoveries = 1;
        config.max_output_continuations = 1;
        config.max_history_items = 32;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(result.value().total_turns == 9 && result.value().tool_executions == 4);
        MIRA_CHECK((cutoff ? result.value().output_continuations : result.value().recoveries) == 4);

        ConversationFixture finite;
        // Rebind the tool IDs to the finite fixture's registry.
        for (std::size_t index = 1; index < responses.size(); index += 2)
            responses[index] = tool_call_response(finite.echo_spec(), R"({"message":"progress"})",
                                                  "call-" + std::to_string(index));
        finite.use_provider(responses);
        config.limit_mode = ConversationLimitMode::Finite;
        config.max_turns = 12;
        config.max_tool_executions = 32;
        auto finite_loop = finite.make_loop(config);
        const auto stopped = finite_loop.run(finite.spec_, conversation_context());
        MIRA_CHECK(stopped && stopped.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(stopped.value().tool_executions == 1);
    }
    return 0;
}

int invalid_config_and_finite_envelope_overflow_fail_before_dispatch() {
    for (const int path : {0, 1, 2, 3}) {
        ConversationFixture fixture;
        fixture.use_provider({text_response("never")});
        auto config = continuous_config();
        if (path == 0)
            config.max_retained_turns = 0;
        if (path == 1)
            config.max_history_items = 255;
        if (path == 2) {
            config.limit_mode = ConversationLimitMode::Finite;
            config.max_turns = std::numeric_limits<std::uint32_t>::max();
        }
        if (path == 3) {
            config.limit_mode = ConversationLimitMode::Finite;
            config.max_turns = 2;
            config.max_output_tokens_per_turn = std::numeric_limits<std::uint64_t>::max();
        }
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(!result || result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(fixture.provider_->requests().empty());
    }
    return 0;
}
} // namespace

int main() {
    for (const auto test : {budget_scope_codec_and_validation,
                            long_run_compacts_owned_history_and_retains_recent_records,
                            finite_zero_and_limits_remain_explicit,
                            history_capacity_and_transform_degradation_are_visible,
                            batched_signed_thinking_and_recent_round_are_preserved,
                            per_round_capacity_rejects_entire_batch,
                            cancellation_deadline_and_late_admission_never_dispatch,
                            cancellation_between_tools_and_executor_shutdown,
                            recovery_output_limit_and_tool_errors_remain_bounded,
                            successful_tool_round_resets_failure_and_continuation_episodes,
                            invalid_config_and_finite_envelope_overflow_fail_before_dispatch}) {
        if (const int code = test(); code != 0)
            return code;
    }
    return 0;
}
