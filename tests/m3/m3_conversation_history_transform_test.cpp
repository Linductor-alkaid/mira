// ConversationLoop history compaction seam tests (DEC-088 follow-up,
// downstream ledger MIRA-20261014-006): the loop owns the canonical
// tool-history replay, so a host that wants watermark compaction or
// summarization can never see it. The seam hands the full history to the
// host once per assembled request and replays the returned list for that
// request only. Every failure shape — Error, exception, empty result,
// structurally invalid item — degrades to the raw replay with a visible
// event; without the transformer the replay stays byte-identical.

#include "support/conversation_input_supply_fixture.hpp"
#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

namespace {

using namespace mira;
using namespace mira::testing;

// A script with two executed tool rounds: the replay under test exists from
// the second model turn onward.
void seed_two_rounds(SupplyFixture &fixture) {
    fixture.use_provider({
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-1"),
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-2"),
        text_response("done"),
    });
}

// A passthrough transformer: the returned list is the input, part for part.
[[nodiscard]] Result<std::vector<ModelInputItem>>
passthrough(const std::vector<ModelInputItem> &history) {
    return history;
}

bool request_mentions_call(const ModelRequest &request, const std::string &call_id) {
    for (const auto &item : request.input) {
        for (const auto &part : item.content) {
            if (const auto *call = std::get_if<ToolCallPart>(&part);
                call != nullptr && call->provider_call_id.value == call_id) {
                return true;
            }
            if (const auto *result = std::get_if<ToolResultPart>(&part);
                result != nullptr && result->provider_call_id.value == call_id) {
                return true;
            }
        }
    }
    return false;
}

// Structural request-input parity: roles, provenance sources and the part
// sequence with its discriminative fields. Two deterministic runs of the
// same script must produce identical replays with and without the seam.
bool same_replay(const ModelRequest &left, const ModelRequest &right) {
    if (left.input.size() != right.input.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.input.size(); ++index) {
        const auto &a = left.input[index];
        const auto &b = right.input[index];
        if (a.role != b.role || a.provenance.source != b.provenance.source ||
            a.authority != b.authority || a.content.size() != b.content.size()) {
            return false;
        }
        for (std::size_t part_index = 0; part_index < a.content.size(); ++part_index) {
            const auto &pa = a.content[part_index];
            const auto &pb = b.content[part_index];
            if (pa.index() != pb.index()) {
                return false;
            }
            if (const auto *text = std::get_if<TextPart>(&pa);
                text != nullptr && text->text != std::get<TextPart>(pb).text) {
                return false;
            }
            if (const auto *call = std::get_if<ToolCallPart>(&pa);
                call != nullptr &&
                (call->provider_call_id != std::get<ToolCallPart>(pb).provider_call_id ||
                 call->wire_name != std::get<ToolCallPart>(pb).wire_name)) {
                return false;
            }
            if (const auto *result = std::get_if<ToolResultPart>(&pa);
                result != nullptr &&
                (result->provider_call_id != std::get<ToolResultPart>(pb).provider_call_id ||
                 result->failed != std::get<ToolResultPart>(pb).failed)) {
                return false;
            }
        }
    }
    return true;
}

// Drops every history item belonging to one tool call (both the call echo and
// its result): the compaction a host performs with a watermark decision.
[[nodiscard]] Result<std::vector<ModelInputItem>>
drop_first_pair(const std::vector<ModelInputItem> &history) {
    std::vector<ModelInputItem> replay;
    for (const auto &item : history) {
        bool drop = false;
        for (const auto &part : item.content) {
            if (const auto *call = std::get_if<ToolCallPart>(&part);
                call != nullptr && call->provider_call_id.value == "call-1") {
                drop = true;
            }
            if (const auto *result = std::get_if<ToolResultPart>(&part);
                result != nullptr && result->provider_call_id.value == "call-1") {
                drop = true;
            }
        }
        if (!drop) {
            replay.push_back(item);
        }
    }
    return replay;
}

int passthrough_transform_keeps_replay_byte_identical() {
    SupplyFixture plain;
    seed_two_rounds(plain);
    auto plain_loop = plain.make_loop(ConversationLoopConfig{});
    const auto plain_result = plain_loop.run(plain.spec_, plain_loop_context());
    MIRA_CHECK(plain_result.has_value() &&
               plain_result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(plain_result.value().history_transforms == 0);
    MIRA_CHECK(plain_result.value().history_transform_degradations == 0);

    SupplyFixture transformed;
    seed_two_rounds(transformed);
    auto transformed_loop = transformed.make_loop(ConversationLoopConfig{});
    transformed_loop.set_history_transformer(
        [](const std::vector<ModelInputItem> &history) { return passthrough(history); });
    const auto transformed_result = transformed_loop.run(transformed.spec_, plain_loop_context());
    MIRA_CHECK(transformed_result.has_value() &&
               transformed_result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(transformed_result.value().history_transforms == 2);

    const auto &plain_requests = plain.provider_->requests();
    const auto &seam_requests = transformed.provider_->requests();
    MIRA_CHECK(plain_requests.size() == 3 && seam_requests.size() == 3);
    // The first request has no history yet: identical with or without a seam.
    MIRA_CHECK(same_replay(plain_requests[0], seam_requests[0]));
    MIRA_CHECK(same_replay(plain_requests[1], seam_requests[1]));
    MIRA_CHECK(same_replay(plain_requests[2], seam_requests[2]));
    return 0;
}

int transform_replaces_replay_and_is_visible() {
    SupplyFixture fixture;
    seed_two_rounds(fixture);
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_history_transformer(
        [](const std::vector<ModelInputItem> &history) { return drop_first_pair(history); });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "done");
    // Turn 2's history is only the call-1 pair: dropping it whole would strip
    // the model's only memory of executed work, so the empty result degrades
    // to the raw replay. Turn 3's history still holds the call-2 pair and the
    // transform applies.
    MIRA_CHECK(result.value().history_transforms == 1);
    MIRA_CHECK(result.value().history_transform_degradations == 1);

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(request_mentions_call(requests[1], "call-1")); // degraded to the raw replay
    MIRA_CHECK(request_mentions_call(requests[2], "call-2"));
    // The compaction decision of the host removed the first pair from the
    // second transform while the loop-owned history stays complete.
    MIRA_CHECK(!request_mentions_call(requests[2], "call-1"));

    const auto applied = typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                                      "HistoryTransformApplied");
    MIRA_CHECK(applied.size() == 1);
    MIRA_CHECK(int_field(applied[0], "items_in") == 4);
    MIRA_CHECK(int_field(applied[0], "items_out") == 2);
    const auto degraded = typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                                       "HistoryTransformDegraded");
    MIRA_CHECK(degraded.size() == 1);
    const auto *reason = string_field(degraded.front(), "reason");
    MIRA_CHECK(reason != nullptr && *reason == "empty-result");
    return 0;
}

int exception_degrades_to_raw_replay() {
    SupplyFixture fixture;
    seed_two_rounds(fixture);
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_history_transformer(
        [](const std::vector<ModelInputItem> &) -> Result<std::vector<ModelInputItem>> {
            throw std::runtime_error("host compaction exploded");
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value() && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().history_transforms == 0);
    MIRA_CHECK(result.value().history_transform_degradations == 2);
    const auto requests = fixture.provider_->requests();
    // The first request has no history yet; later replays stay raw and grow
    // with the rounds the loop actually executed.
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(request_mentions_call(requests[1], "call-1"));
    MIRA_CHECK(!request_mentions_call(requests[1], "call-2"));
    MIRA_CHECK(request_mentions_call(requests[2], "call-1"));
    MIRA_CHECK(request_mentions_call(requests[2], "call-2"));
    const auto degraded = typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                                       "HistoryTransformDegraded");
    MIRA_CHECK(degraded.size() == 2);
    for (const auto &event : degraded) {
        const auto *reason = string_field(event, "reason");
        MIRA_CHECK(reason != nullptr && *reason == "transformer-exception");
    }
    return 0;
}

int error_empty_and_invalid_results_degrade() {
    const auto run_with = [](auto transform, const std::string &expected_reason) {
        SupplyFixture fixture;
        seed_two_rounds(fixture);
        auto loop = fixture.make_loop(ConversationLoopConfig{});
        loop.set_history_transformer(transform);
        const auto result = loop.run(fixture.spec_, plain_loop_context());
        MIRA_CHECK(result.has_value() && result.value().outcome == ConversationOutcome::Answered);
        MIRA_CHECK(result.value().history_transforms == 0);
        MIRA_CHECK(result.value().history_transform_degradations == 2);
        const auto requests = fixture.provider_->requests();
        for (std::size_t index = 1; index < requests.size(); ++index) {
            MIRA_CHECK(request_mentions_call(requests[index], "call-1"));
        }
        const auto degraded = typed_events(
            session_events(*fixture.events_, fixture.spec_.session_id), "HistoryTransformDegraded");
        MIRA_CHECK(degraded.size() == 2);
        for (const auto &event : degraded) {
            const auto *reason = string_field(event, "reason");
            MIRA_CHECK(reason != nullptr && *reason == expected_reason);
        }
        return 0;
    };

    const int error_rc = run_with(
        [](const std::vector<ModelInputItem> &) -> Result<std::vector<ModelInputItem>> {
            Error error;
            error.code = ErrorCode::InvalidArgument;
            error.domain = "mira.test";
            error.safe_message = "compaction refused";
            return error;
        },
        "transformer-error");
    MIRA_CHECK(error_rc == 0);
    const int empty_rc = run_with(
        [](const std::vector<ModelInputItem> &) -> Result<std::vector<ModelInputItem>> {
            return std::vector<ModelInputItem>{};
        },
        "empty-result");
    MIRA_CHECK(empty_rc == 0);
    // A System role item is loop-owned envelope content; a host replay may
    // never inject it, so the whole result degrades instead of passing.
    const int invalid_rc = run_with(
        [](const std::vector<ModelInputItem> &history) -> Result<std::vector<ModelInputItem>> {
            ModelInputItem foreign;
            foreign.role = ModelRole::System;
            TextPart part;
            part.text = "injected policy";
            foreign.content.emplace_back(std::move(part));
            std::vector<ModelInputItem> replay{std::move(foreign)};
            for (const auto &item : history) {
                replay.push_back(item);
            }
            return replay;
        },
        "invalid-item");
    MIRA_CHECK(invalid_rc == 0);
    return 0;
}

int cancelled_run_never_invokes_the_transformer() {
    SupplyFixture fixture;
    seed_two_rounds(fixture);
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    std::uint32_t invocations = 0;
    loop.set_history_transformer([&invocations](const std::vector<ModelInputItem> &history)
                                     -> Result<std::vector<ModelInputItem>> {
        ++invocations;
        return history;
    });
    const auto result = loop.run(fixture.spec_, conversation_context([] { return true; }));
    MIRA_CHECK(result.has_value() && result.value().outcome == ConversationOutcome::Cancelled);
    // Cancellation trips at the turn boundary before any assembly work, so
    // the bounded host callback never runs.
    MIRA_CHECK(invocations == 0);
    MIRA_CHECK(result.value().history_transforms == 0);
    MIRA_CHECK(result.value().history_transform_degradations == 0);
    return 0;
}

} // namespace

int main() {
    if (const int code = passthrough_transform_keeps_replay_byte_identical(); code != 0) {
        return code;
    }
    if (const int code = transform_replaces_replay_and_is_visible(); code != 0) {
        return code;
    }
    if (const int code = exception_degrades_to_raw_replay(); code != 0) {
        return code;
    }
    if (const int code = error_empty_and_invalid_results_degrade(); code != 0) {
        return code;
    }
    if (const int code = cancelled_run_never_invokes_the_transformer(); code != 0) {
        return code;
    }
    return 0;
}
