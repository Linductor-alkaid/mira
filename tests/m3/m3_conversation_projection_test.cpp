#include "support/conversation_loop_support.hpp"
#include "support/test.hpp"

#include <mira/context_retrieval.hpp>
#include <mira/conversation_log.hpp>

#include <string>

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

    const auto view = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(view && view.value().size() == 2);
    MIRA_CHECK(view.value()[0].kind == ConversationEntry::Kind::UserMessage);
    MIRA_CHECK(view.value()[0].text == "remember this follow-up");
    const auto &outcome = view.value()[1];
    MIRA_CHECK(outcome.kind == ConversationEntry::Kind::LoopOutcome);
    MIRA_CHECK(outcome.text == "loop settled: Answered (turns 2, total_turns 2, recoveries 0, "
                               "tool_executions 1)");
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
    MIRA_CHECK(view.value()[0].session_sequence < outcome.session_sequence);

    const auto rebuilt = build_conversation_view(*fixture.events_, fixture.spec_.session_id);
    MIRA_CHECK(rebuilt && rebuilt.value().size() == view.value().size());
    const auto segments = segment_conversation(fixture.spec_.session_id, view.value(), {});
    const auto repeated = segment_conversation(fixture.spec_.session_id, rebuilt.value(), {});
    MIRA_CHECK(segments && repeated && segments.value().size() == 1);
    MIRA_CHECK(repeated.value().size() == 1);
    MIRA_CHECK(segments.value()[0].entry_count == 2);
    MIRA_CHECK(segments.value()[0].source_events.back() == outcome.origin);
    MIRA_CHECK(segments.value()[0].through_sequence == outcome.session_sequence);
    MIRA_CHECK(segments.value()[0].text.find("Answered") != std::string::npos);
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
    MIRA_CHECK(view.value()[1].text == "loop settled: Cancelled");
    MIRA_CHECK(view.value()[1].origin == minimal.event_id);
    MIRA_CHECK(view.value()[2].text ==
               "loop settled: Failed (turns 1, total_turns 9223372036854775807, recoveries 2, "
               "tool_executions 4)");
    MIRA_CHECK(view.value()[3].text == "loop settled: MaxTurns");
    return 0;
}

int recognized_corruption_fails_closed() {
    for (const std::string type : {"UserMessageInjected", "LoopSettled", "ConversationSettled"}) {
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

} // namespace

int main() {
    if (const int status = actual_loop_events_rebuild_and_segment(); status != 0) {
        return status;
    }
    if (const int status = both_settlements_and_optional_counters(); status != 0) {
        return status;
    }
    return recognized_corruption_fails_closed();
}
