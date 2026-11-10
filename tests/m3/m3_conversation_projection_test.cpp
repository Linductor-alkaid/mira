#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <mira/context_retrieval.hpp>
#include <mira/conversation_log.hpp>

#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

AppendRequest event(const SessionId &session, std::string type, std::string payload) {
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = RuntimeId::generate();
    request.session_id = session;
    request.payload = EventPayload{std::move(type), std::move(payload), EventClass::State};
    return request;
}

int actual_loop_events_rebuild_and_segment() {
    ConversationFixture fixture;
    fixture.use_provider(
        {tool_call_response(fixture.echo_spec(), R"({"message":"ping"})"), text_response("done")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    MIRA_CHECK(loop.enqueue_user_message("remember this follow-up"));
    const auto result = loop.run(fixture.spec_, conversation_context());
    MIRA_CHECK(result && result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().total_turns == 2);
    MIRA_CHECK(result.value().tool_executions == 1);

    // DEC-104: the real run projects as user message, the assistant's tool
    // speech, the assistant's terminal answer, and the settled record.
    const auto view = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(view && view.value().size() == 4);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[0].text == "remember this follow-up");
    const auto &tool_speech = view.value()[1];
    MIRA_CHECK(tool_speech.kind == ConversationEntry::Kind::AssistantSpeech);
    // DEC-108: the speaker is carried structurally by `kind`; entry text is
    // the bare speech and `entry_line` is the single prefix renderer.
    MIRA_CHECK(tool_speech.text == "tool call echo {\"message\":\"ping\"} (executed)");
    const auto &answer_speech = view.value()[2];
    MIRA_CHECK(answer_speech.kind == ConversationEntry::Kind::AssistantSpeech);
    MIRA_CHECK(answer_speech.text == "done");
    const auto &outcome = view.value()[3];
    MIRA_CHECK(outcome.kind == ConversationEntry::Kind::LoopOutcome);
    MIRA_CHECK(outcome.text ==
               "loop settled: Answered (terminal text + 1 tool call) (turns 2, total_turns 2, "
               "recoveries 0, tool_executions 1)");
    EventQuery query;
    query.session_id = fixture.spec_.session_id;
    const auto page = fixture.events_->read(query);
    MIRA_CHECK(page && !page.value().events.empty());
    const auto &settled = page.value().events.back();
    MIRA_CHECK(settled.payload.type == "ConversationSettled");
    MIRA_CHECK(outcome.origin == settled.event_id);
    MIRA_CHECK(outcome.session_sequence == settled.session_sequence);
    MIRA_CHECK(outcome.recorded_at.wall == settled.timestamp.wall);
    MIRA_CHECK(outcome.recorded_at.monotonic == settled.timestamp.monotonic);
    MIRA_CHECK(view.value()[0].session_sequence < tool_speech.session_sequence);
    MIRA_CHECK(tool_speech.session_sequence < answer_speech.session_sequence);
    MIRA_CHECK(answer_speech.session_sequence < outcome.session_sequence);

    const auto rebuilt = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(rebuilt && rebuilt.value().size() == view.value().size());
    const auto segments = segment_conversation(fixture.spec_.session_id, view.value(), {});
    const auto repeated = segment_conversation(fixture.spec_.session_id, rebuilt.value(), {});
    MIRA_CHECK(segments && repeated && segments.value().size() == 1);
    MIRA_CHECK(repeated.value().size() == 1);
    MIRA_CHECK(segments.value()[0].entry_count == 4);
    MIRA_CHECK(segments.value()[0].source_events.back() == outcome.origin);
    MIRA_CHECK(segments.value()[0].through_sequence == outcome.session_sequence);
    MIRA_CHECK(segments.value()[0].text.find("Answered") != std::string::npos);
    // DEC-104/DEC-108: the segment lines carry the speaker role prefixes
    // exactly once, ahead of the bare entry text.
    MIRA_CHECK(segments.value()[0].text.find("user: remember this follow-up") != std::string::npos);
    MIRA_CHECK(segments.value()[0].text.find(
                   "assistant: tool call echo {\"message\":\"ping\"} (executed)") !=
               std::string::npos);
    MIRA_CHECK(segments.value()[0].text.find("assistant: done") != std::string::npos);
    MIRA_CHECK(segments.value()[0].text.find("assistant said:") == std::string::npos);
    MIRA_CHECK(segments.value()[0].text == repeated.value()[0].text);
    MIRA_CHECK(segments.value()[0].asset_id == repeated.value()[0].asset_id);
    return 0;
}

int both_settlements_and_optional_counters() {
    MemoryEventStore store;
    const auto session = SessionId::generate();
    MIRA_CHECK(build_conversation_view(store, session).value().empty());
    const auto old =
        event(session, "LoopSettled", R"({"detail":{"outcome":"Completed","steps":3}})");
    MIRA_CHECK(store.append(old));
    // Unrelated payloads are never parsed, including unknown future types.
    for (const std::string type : {"ActionDispatched", "InputDelivered", "FutureEvent"}) {
        MIRA_CHECK(store.append(event(session, type, "{not-json")));
    }
    const auto minimal =
        event(session, "ConversationSettled", R"({"detail":{"outcome":"Cancelled"}})");
    MIRA_CHECK(store.append(minimal));
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"Failed","turns":1,"total_turns":9223372036854775807,"recoveries":2,"tool_executions":4}})")));
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"MaxTurns","turns":null,"total_turns":"bad","recoveries":false,"tool_executions":{}}})")));
    // The selected session is isolated from a malformed recognized event elsewhere.
    MIRA_CHECK(store.append(event(SessionId::generate(), "ConversationSettled", "{bad")));
    const auto view = build_conversation_view(store, session);
    MIRA_CHECK(view && view.value().size() == 4);
    MIRA_CHECK(view.value()[0].text == "loop settled: Completed (steps 3)");
    MIRA_CHECK(view.value()[0].origin == old.event_id);
    // Non-Answered outcomes keep the legacy wording byte for byte (DEC-104).
    MIRA_CHECK(view.value()[1].text == "loop settled: Cancelled");
    MIRA_CHECK(view.value()[1].origin == minimal.event_id);
    MIRA_CHECK(view.value()[2].text ==
               "loop settled: Failed (turns 1, total_turns 9223372036854775807, recoveries 2, "
               "tool_executions 4)");
    MIRA_CHECK(view.value()[3].text == "loop settled: MaxTurns");
    return 0;
}

int answered_settlement_distinguishes_tool_speech() {
    // DEC-104: an Answered settlement reads differently depending on whether
    // the run executed tool calls; the counter group of the legacy wording
    // still follows the fact lines.
    MemoryEventStore store;
    const auto session = SessionId::generate();
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"Answered","turns":2,"total_turns":2,"recoveries":0,"tool_executions":1}})")));
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"Answered","turns":4,"total_turns":4,"recoveries":1,"tool_executions":3}})")));
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"Answered","turns":1,"total_turns":1,"recoveries":0,"tool_executions":0}})")));
    MIRA_CHECK(store.append(
        event(session, "ConversationSettled", R"({"detail":{"outcome":"Answered"}})")));
    MIRA_CHECK(store.append(event(session, "ConversationSettled",
                                  R"({"detail":{"outcome":"Answered","tool_executions":"bad"}})")));
    const auto view = build_conversation_view(store, session);
    MIRA_CHECK(view && view.value().size() == 5);
    MIRA_CHECK(view.value()[0].text ==
               "loop settled: Answered (terminal text + 1 tool call) (turns 2, total_turns 2, "
               "recoveries 0, tool_executions 1)");
    MIRA_CHECK(view.value()[1].text ==
               "loop settled: Answered (terminal text + 3 tool calls) (turns 4, total_turns 4, "
               "recoveries 1, tool_executions 3)");
    MIRA_CHECK(view.value()[2].text ==
               "loop settled: Answered (terminal text only) (turns 1, total_turns 1, recoveries "
               "0, tool_executions 0)");
    MIRA_CHECK(view.value()[3].text == "loop settled: Answered (terminal text only)");
    // A non-numeric counter is treated as absent, exactly like the legacy
    // counter rendering.
    MIRA_CHECK(view.value()[4].text == "loop settled: Answered (terminal text only)");
    return 0;
}

int assistant_speech_entries_render_lines() {
    // DEC-104: one entry per speech event, one line per content piece.
    MemoryEventStore store;
    const auto session = SessionId::generate();
    const auto user = event(session, "UserMessageInjected", R"({"detail":{"text":"hello there"}})");
    MIRA_CHECK(store.append(user));
    const auto text_only =
        event(session, "AssistantSpeech",
              R"({"detail":{"turn":2,"sensitivity":"Internal","text":"spoken answer"}})");
    MIRA_CHECK(store.append(text_only));
    const auto calls_only = event(session, "AssistantSpeech",
                                  R"({"detail":{"turn":1,"sensitivity":"Internal","tool_calls":[
            {"wire_name":"echo","arguments":"{\"message\":\"ping\"}","arguments_digest":"aa","status":"executed"},
            {"wire_name":"echo","arguments":"","arguments_digest":"bb","status":"skipped"},
            {"wire_name":"echo","status":"failed"},
            {"wire_name":"echo","arguments":42,"status":"not_executed"}]}})");
    MIRA_CHECK(store.append(calls_only));
    const auto mixed = event(session, "AssistantSpeech",
                             R"({"detail":{"turn":3,"sensitivity":"Internal","text":"working on it",
            "tool_calls":[{"wire_name":"echo","arguments":"{}","arguments_digest":"cc","status":"executed"}]}})");
    MIRA_CHECK(store.append(mixed));
    const auto view = build_conversation_view(store, session);
    MIRA_CHECK(view && view.value().size() == 4);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[0].text == "hello there");
    MIRA_CHECK(view.value()[1].kind == ConversationEntry::Kind::AssistantSpeech);
    MIRA_CHECK(view.value()[1].text == "spoken answer");
    EventQuery query;
    query.session_id = session;
    const auto page = store.read(query);
    MIRA_CHECK(page && page.value().events.size() == 4);
    const auto &speech_envelope = page.value().events[1];
    MIRA_CHECK(speech_envelope.payload.type == "AssistantSpeech");
    MIRA_CHECK(view.value()[1].origin == speech_envelope.event_id);
    MIRA_CHECK(view.value()[1].session_sequence == speech_envelope.session_sequence);
    MIRA_CHECK(view.value()[1].recorded_at.wall == speech_envelope.timestamp.wall);
    MIRA_CHECK(view.value()[1].recorded_at.monotonic == speech_envelope.timestamp.monotonic);
    MIRA_CHECK(view.value()[2].text == "tool call echo {\"message\":\"ping\"} (executed)\n"
                                       "tool call echo (skipped)\n"
                                       "tool call echo (failed)\n"
                                       "tool call echo (not_executed)");
    MIRA_CHECK(view.value()[3].text == "working on it\ntool call echo {} (executed)");
    const auto rebuilt = build_conversation_view(store, session);
    MIRA_CHECK(rebuilt && rebuilt.value().size() == view.value().size());
    for (std::size_t index = 0; index < view.value().size(); ++index) {
        MIRA_CHECK(rebuilt.value()[index].text == view.value()[index].text);
        MIRA_CHECK(rebuilt.value()[index].kind == view.value()[index].kind);
    }
    return 0;
}

int assistant_speech_corruption_fails_closed() {
    // Every unreadable shape of a recognized AssistantSpeech payload is a
    // DataLoss projection, never a silently dropped entry (DEC-104).
    const std::vector<std::string> payloads = {
        "{bad-json",
        "{}",
        R"({"detail":[]})",
        R"({"detail":{}})",
        R"({"detail":{"text":""}})",
        R"({"detail":{"text":"","tool_calls":[]}})",
        R"({"detail":{"text":7}})",
        R"({"detail":{"text":["spoken"]}})",
        R"({"detail":{"tool_calls":{}}})",
        R"({"detail":{"tool_calls":3}})",
        R"({"detail":{"tool_calls":["echo"]}})",
        R"({"detail":{"tool_calls":[{}]}})",
        R"({"detail":{"tool_calls":[{"wire_name":"echo"}]}})",
        R"({"detail":{"tool_calls":[{"wire_name":7,"status":"executed"}]}})",
        R"({"detail":{"tool_calls":[{"wire_name":"echo","status":true}]}})",
    };
    for (const auto &payload : payloads) {
        MemoryEventStore store;
        const auto session = SessionId::generate();
        MIRA_CHECK(store.append(event(session, "AssistantSpeech", payload)));
        const auto view = build_conversation_view(store, session);
        MIRA_CHECK(!view);
        MIRA_CHECK(view.error().code == ErrorCode::DataLoss);
        MIRA_CHECK(view.error().domain == "mira.conversation");
        MIRA_CHECK(view.error().safe_message.find("AssistantSpeech") != std::string::npos);
    }
    return 0;
}

int recognized_corruption_fails_closed() {
    for (const std::string type :
         {"UserMessageInjected", "AssistantSpeech", "LoopSettled", "ConversationSettled"}) {
        for (const std::string payload : {"{bad-json", "{}", R"({"detail":[]})", R"({"detail":{}})",
                                          R"({"detail":{"outcome":7,"text":7}})"}) {
            MemoryEventStore store;
            const auto session = SessionId::generate();
            MIRA_CHECK(store.append(event(session, type, payload)));
            const auto view = build_conversation_view(store, session);
            MIRA_CHECK(!view);
            MIRA_CHECK(view.error().code == ErrorCode::DataLoss);
            MIRA_CHECK(view.error().domain == "mira.conversation");
            MIRA_CHECK(view.error().safe_message.find(type) != std::string::npos);
        }
    }
    return 0;
}

int segment_lines_carry_role_prefixes() {
    // DEC-104: the M17 conversation segmentation prefixes each projected entry
    // with its speaker role.
    MemoryEventStore store;
    const auto session = SessionId::generate();
    MIRA_CHECK(store.append(
        event(session, "UserMessageInjected", R"({"detail":{"text":"please answer"}})")));
    MIRA_CHECK(
        store.append(event(session, "AssistantSpeech",
                           R"({"detail":{"turn":1,"sensitivity":"Internal","text":"the answer",
            "tool_calls":[{"wire_name":"echo","arguments":"{}","arguments_digest":"dd","status":"executed"}]}})")));
    MIRA_CHECK(store.append(event(
        session, "ConversationSettled",
        R"({"detail":{"outcome":"Answered","turns":1,"total_turns":1,"recoveries":0,"tool_executions":1}})")));
    const auto view = build_conversation_view(store, session);
    MIRA_CHECK(view && view.value().size() == 3);
    const auto segments = segment_conversation(session, view.value(), {});
    MIRA_CHECK(segments && segments.value().size() == 1);
    const auto &text = segments.value()[0].text;
    // The role prefix attaches to each entry: a multi-line assistant entry
    // carries it once, ahead of its first rendered line. Entry text itself is
    // prefix-free since DEC-108; only `entry_line` renders prefixes.
    MIRA_CHECK(text ==
               "user: please answer\n"
               "assistant: the answer\n"
               "tool call echo {} (executed)\n"
               "loop: loop settled: Answered (terminal text + 1 tool call) (turns 1, total_turns "
               "1, recoveries 0, tool_executions 1)");
    MIRA_CHECK(segments.value()[0].entries.size() == 3);
    MIRA_CHECK(segments.value()[0].entries[0].text == "user: please answer");
    // Segment entries mirror the exact prefixed line carried in the segment
    // text; prefix stripping happens only at replay conversion (DEC-108).
    MIRA_CHECK(segments.value()[0].entries[1].text ==
               "assistant: the answer\ntool call echo {} (executed)");
    // DEC-108: the speaker travels structurally into every segment entry.
    MIRA_CHECK(segments.value()[0].entries[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(segments.value()[0].entries[1].kind == ConversationEntry::Kind::AssistantSpeech);
    MIRA_CHECK(segments.value()[0].entries[2].kind == ConversationEntry::Kind::LoopOutcome);
    MIRA_CHECK(segments.value()[0].entries[2].text ==
               "loop: loop settled: Answered (terminal text + 1 tool call) (turns 1, total_turns "
               "1, recoveries 0, tool_executions 1)");
    return 0;
}

} // namespace

int main() {
    if (const int status = actual_loop_events_rebuild_and_segment(); status != 0) {
        return status;
    }
    if (const int status = both_settlements_and_optional_counters(); status != 0) {
        return status;
    }
    if (const int status = answered_settlement_distinguishes_tool_speech(); status != 0) {
        return status;
    }
    if (const int status = assistant_speech_entries_render_lines(); status != 0) {
        return status;
    }
    if (const int status = assistant_speech_corruption_fails_closed(); status != 0) {
        return status;
    }
    if (const int status = recognized_corruption_fails_closed(); status != 0) {
        return status;
    }
    return segment_lines_carry_role_prefixes();
}
