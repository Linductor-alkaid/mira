// ConversationLoop terminal truncation observability (DEC-065, issue #100 /
// MIRA-20261022-001): a run that died on the output limit carries a
// structured terminal reason on the result and on the ConversationSettled
// payload, so hosts classify the terminal state without reading the
// safe_summary text anchor. Every other terminal stays silent: the reason is
// absent from the payload, never empty.

#include "support/conversation_loop_support.hpp"

#include "support/test.hpp"

#include <mira/conversation_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/model_contracts.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// The settled payload of one run: the truncation counter is always reported,
// the terminal reason only when the run ended on an output-limit exhaustion.
struct SettledFacts final {
    bool has_counter = false;
    std::int64_t continuations = -1;
    bool has_reason = false;
    std::string reason;
};

[[nodiscard]] SettledFacts settled_facts(const ConversationFixture &fixture) {
    SettledFacts facts;
    const auto detail = settled_detail(*fixture.events_, fixture.spec_.session_id);
    if (!detail.has_value()) {
        return facts;
    }
    if (const auto *counter = detail.value().find("output_continuations")) {
        facts.has_counter = true;
        facts.continuations = counter->as_integer().value_or(-1);
    }
    if (const auto *reason = detail.value().find("incomplete_reason")) {
        facts.has_reason = true;
        if (const auto *text = reason->as_string()) {
            facts.reason = *text;
        }
    }
    return facts;
}

int output_limit_exhaustion_reports_the_terminal_reason() {
    ConversationFixture fixture;
    fixture.use_provider({incomplete_response(IncompleteReason::MaxOutputTokens, ""),
                          incomplete_response(IncompleteReason::MaxOutputTokens, ""),
                          incomplete_response(IncompleteReason::MaxOutputTokens, "")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().output_continuations == 2);
    // The structured reason replaces the summary text anchor.
    MIRA_CHECK(result.value().terminal_incomplete_reason == IncompleteReason::MaxOutputTokens);

    const auto facts = settled_facts(fixture);
    MIRA_CHECK(facts.has_counter);
    MIRA_CHECK(facts.continuations == 2);
    MIRA_CHECK(facts.has_reason);
    MIRA_CHECK(facts.reason == incomplete_reason_name(IncompleteReason::MaxOutputTokens));
    return 0;
}

int other_terminals_carry_no_truncation_reason() {
    // A plain answer: no cutoff at all.
    {
        ConversationFixture fixture;
        fixture.use_provider({text_response("done")});
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 0);
        MIRA_CHECK(!facts.has_reason);
    }
    // A tolerated cutoff the model recovered from: the counter reports the
    // cutoff, the terminal reason stays empty.
    {
        ConversationFixture fixture;
        fixture.use_provider({incomplete_response(IncompleteReason::MaxOutputTokens, "partial"),
                              text_response("done")});
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(result.value().output_continuations == 1);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 1);
        MIRA_CHECK(!facts.has_reason);
    }
    // A cutoff the provider did not attribute to the output limit: the
    // recovery budget is what runs out, not the continuation budget.
    // DEC-066 amends DEC-065 here: the terminal reason now reports the
    // provider's own cause, so this terminal is not reason-free.
    {
        ConversationFixture fixture;
        fixture.use_provider({incomplete_response(IncompleteReason::ContextWindow, ""),
                              incomplete_response(IncompleteReason::ContextWindow, ""),
                              incomplete_response(IncompleteReason::ContextWindow, "")});
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().safe_summary.find("remained incomplete after recovery budget") !=
                   std::string::npos);
        MIRA_CHECK(result.value().recoveries == 2);
        MIRA_CHECK(result.value().output_continuations == 0);
        MIRA_CHECK(result.value().terminal_incomplete_reason == IncompleteReason::ContextWindow);
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 0);
        MIRA_CHECK(facts.has_reason);
        MIRA_CHECK(facts.reason == incomplete_reason_name(IncompleteReason::ContextWindow));
    }
    // The tool budget runs out.
    {
        ConversationFixture fixture;
        fixture.use_provider(
            {tool_call_response(fixture.echo_spec(), R"json({"message": "one"})json", "call-1"),
             tool_call_response(fixture.echo_spec(), R"json({"message": "two"})json", "call-2")});
        ConversationLoopConfig config;
        config.max_tool_executions = 1;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().safe_summary.find("tool execution budget") != std::string::npos);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 0);
        MIRA_CHECK(!facts.has_reason);
    }
    // The turn budget runs out.
    {
        ConversationFixture fixture;
        fixture.use_provider(
            {tool_call_response(fixture.echo_spec(), R"json({"message": "one"})json", "call-1")});
        ConversationLoopConfig config;
        config.max_turns = 1;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::MaxTurns);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 0);
        MIRA_CHECK(!facts.has_reason);
    }
    // Cancellation.
    {
        ConversationFixture fixture;
        fixture.use_provider({text_response("never sent")});
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        const auto result = loop.run(fixture.spec_, conversation_context([] { return true; }));
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Cancelled);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 0);
        MIRA_CHECK(!facts.has_reason);
    }
    return 0;
}

[[nodiscard]] ConversationLoopConfig continuous_config() {
    ConversationLoopConfig config;
    config.limit_mode = ConversationLimitMode::Continuous;
    config.max_turns = 0;
    config.max_tool_executions = 0;
    config.max_history_items = 32;
    config.max_retained_turns = 8;
    return config;
}

int continuous_mode_keeps_the_same_terminal_semantics() {
    // The episode budget runs out: the terminal reason is set exactly like in
    // the finite mode.
    {
        ConversationFixture fixture;
        fixture.use_provider({incomplete_response(IncompleteReason::MaxOutputTokens, ""),
                              incomplete_response(IncompleteReason::MaxOutputTokens, ""),
                              incomplete_response(IncompleteReason::MaxOutputTokens, "")});
        auto config = continuous_config();
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
        MIRA_CHECK(result.value().output_continuations == 2);
        MIRA_CHECK(result.value().terminal_incomplete_reason == IncompleteReason::MaxOutputTokens);
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 2);
        MIRA_CHECK(facts.has_reason);
        MIRA_CHECK(facts.reason == incomplete_reason_name(IncompleteReason::MaxOutputTokens));
    }
    // A complete tool round resets the episode budget, so the run survives two
    // cutoffs and answers. The counter keeps the run total, the terminal
    // reason stays empty.
    {
        ConversationFixture fixture;
        fixture.use_provider({incomplete_response(IncompleteReason::MaxOutputTokens, "partial"),
                              tool_call_response(fixture.echo_spec(),
                                                 R"json({"message": "progress"})json", "call-1"),
                              incomplete_response(IncompleteReason::MaxOutputTokens, "partial"),
                              tool_call_response(fixture.echo_spec(),
                                                 R"json({"message": "progress"})json", "call-2"),
                              text_response("done")});
        auto config = continuous_config();
        config.max_output_continuations = 1;
        auto loop = fixture.make_loop(config);
        const auto result = loop.run(fixture.spec_, conversation_context());
        MIRA_CHECK(result.has_value());
        MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(result.value().output_continuations == 2);
        MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
        const auto facts = settled_facts(fixture);
        MIRA_CHECK(facts.has_counter && facts.continuations == 2);
        MIRA_CHECK(!facts.has_reason);
    }
    return 0;
}

int an_incomplete_output_is_never_the_answer() {
    // A cutoff the provider attributed to the context window: the fragment it
    // returned must not be settled as the answer. The loop recovers with
    // cause-aware guidance and the retry answers.
    ConversationFixture fixture;
    fixture.use_provider({incomplete_response(IncompleteReason::ContextWindow, "partial answer"),
                          text_response("the complete answer")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "the complete answer");
    MIRA_CHECK(result.value().recoveries == 1);
    // The fragment never became the answer and never became a terminal
    // reason: the run answered on the retry.
    MIRA_CHECK(result.value().answer.find("partial answer") == std::string::npos);
    MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
    MIRA_CHECK(result.value().turns.size() == 2);
    MIRA_CHECK(result.value().turns.front().summary != "answer");
    MIRA_CHECK(result.value().turns.back().summary == "answer");
    // The retry names the cause the provider reported.
    const auto retry_text = fixture.provider_->request_text(1);
    MIRA_CHECK(retry_text.find("context window") != std::string::npos);
    MIRA_CHECK(retry_text.find("output-token limit") == std::string::npos);
    const auto facts = settled_facts(fixture);
    MIRA_CHECK(facts.has_counter && facts.continuations == 0);
    MIRA_CHECK(!facts.has_reason);
    return 0;
}

int an_incomplete_output_fails_closed_with_its_reason() {
    // The recovery budget runs out on context-window cutoffs: the run fails
    // closed and reports the provider's reason, so the failure class stays
    // observable.
    ConversationFixture fixture;
    fixture.use_provider({incomplete_response(IncompleteReason::ContextWindow, "partial one"),
                          incomplete_response(IncompleteReason::ContextWindow, "partial two"),
                          incomplete_response(IncompleteReason::ContextWindow, "partial three")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().safe_summary.find("remained incomplete after recovery budget") !=
               std::string::npos);
    MIRA_CHECK(result.value().recoveries == 2);
    // No fragment leaks into the answer of a failed run.
    MIRA_CHECK(result.value().answer.empty());
    MIRA_CHECK(result.value().terminal_incomplete_reason == IncompleteReason::ContextWindow);
    const auto facts = settled_facts(fixture);
    MIRA_CHECK(facts.has_counter && facts.continuations == 0);
    MIRA_CHECK(facts.has_reason);
    MIRA_CHECK(facts.reason == incomplete_reason_name(IncompleteReason::ContextWindow));
    return 0;
}

int an_incomplete_output_without_a_reason_reports_none() {
    // The provider did not say why the output was incomplete: the run still
    // fails closed, but there is no reason to report - the field stays empty
    // rather than inventing one.
    ConversationFixture fixture;
    std::vector<ModelResponse> script;
    for (int index = 0; index < 3; ++index) {
        auto response = incomplete_response(IncompleteReason::Other, "partial");
        // The provider reported no reason at all.
        response.incomplete_reason.reset();
        script.push_back(std::move(response));
    }
    fixture.use_provider(script);
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().answer.empty());
    MIRA_CHECK(!result.value().terminal_incomplete_reason.has_value());
    const auto facts = settled_facts(fixture);
    MIRA_CHECK(facts.has_counter && facts.continuations == 0);
    MIRA_CHECK(!facts.has_reason);
    return 0;
}

} // namespace

int main() {
    if (const int code = output_limit_exhaustion_reports_the_terminal_reason(); code != 0) {
        return code;
    }
    if (const int code = other_terminals_carry_no_truncation_reason(); code != 0) {
        return code;
    }
    if (const int code = continuous_mode_keeps_the_same_terminal_semantics(); code != 0) {
        return code;
    }
    if (const int code = an_incomplete_output_is_never_the_answer(); code != 0) {
        return code;
    }
    if (const int code = an_incomplete_output_fails_closed_with_its_reason(); code != 0) {
        return code;
    }
    if (const int code = an_incomplete_output_without_a_reason_reports_none(); code != 0) {
        return code;
    }
    return 0;
}
