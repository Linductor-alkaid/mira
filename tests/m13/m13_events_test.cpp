// M13-07: the two learning audit events (DEC-030 §5). Payloads round trip,
// fail closed on unknown fields, bad outcomes and schema mismatches, the
// closed event set accepts the new types, and offline replay rebuilds from
// the events without touching any memory backend.

#include "support/m13_support.hpp"

#include <mira/replay.hpp>
#include <mira/workflow_learning.hpp>

namespace {

using namespace mira;
using namespace mira::testing;

int learning_payloads_round_trip() {
    const WorkflowRunId run_id = WorkflowRunId::generate();

    WorkflowEpisodeRecordedEvent episode;
    episode.run_id = run_id;
    episode.workflow_id = WorkflowId::generate();
    episode.episode_digest = digest_string("episode");
    episode.outcome = "recorded";
    const auto episode_payload = to_event_payload(episode);
    MIRA_CHECK(episode_payload.type == "WorkflowEpisodeRecorded");
    MIRA_CHECK(episode_payload.classification == EventClass::State);
    auto reparsed_episode = parse_workflow_episode_recorded(episode_payload);
    MIRA_CHECK(reparsed_episode.has_value());
    MIRA_CHECK(reparsed_episode.value().run_id == run_id);
    MIRA_CHECK(reparsed_episode.value().workflow_id == episode.workflow_id);
    MIRA_CHECK(reparsed_episode.value().episode_digest == episode.episode_digest);
    MIRA_CHECK(reparsed_episode.value().outcome == "recorded");
    MIRA_CHECK(reparsed_episode.value().reason_code.empty());

    WorkflowLessonRecordedEvent lesson;
    lesson.run_id = run_id;
    lesson.workflow_id = episode.workflow_id;
    lesson.lesson_digest = digest_string("lesson");
    lesson.outcome = "failed";
    lesson.reason_code = "mira.test:1";
    const auto lesson_payload = to_event_payload(lesson);
    MIRA_CHECK(lesson_payload.type == "WorkflowLessonRecorded");
    MIRA_CHECK(lesson_payload.classification == EventClass::State);
    auto reparsed_lesson = parse_workflow_lesson_recorded(lesson_payload);
    MIRA_CHECK(reparsed_lesson.has_value());
    MIRA_CHECK(reparsed_lesson.value().lesson_digest == lesson.lesson_digest);
    MIRA_CHECK(reparsed_lesson.value().outcome == "failed");
    MIRA_CHECK(reparsed_lesson.value().reason_code == "mira.test:1");
    return 0;
}

int learning_payloads_fail_closed() {
    WorkflowEpisodeRecordedEvent event;
    event.run_id = WorkflowRunId::generate();
    event.workflow_id = WorkflowId::generate();
    event.episode_digest = digest_string("episode");
    event.outcome = "recorded";
    const auto payload = to_event_payload(event);

    // Unknown field: spliced straight into the JSON text.
    EventPayload mutated = payload;
    const auto close = mutated.data.rfind('}');
    mutated.data = mutated.data.substr(0, close) + ",\"surprise\":1}";
    MIRA_CHECK(!parse_workflow_episode_recorded(mutated).has_value());

    // Outcome outside the closed set.
    auto bad_outcome = parse_json(payload.data).value();
    *bad_outcome.find("outcome") = JsonValue{"vanished"};
    mutated.data = to_json_string(bad_outcome);
    MIRA_CHECK(!parse_workflow_episode_recorded(mutated).has_value());

    // Schema mismatch.
    auto bad_schema = parse_json(payload.data).value();
    *bad_schema.find("schema") = JsonValue{"mira.workflow.episode-recorded.v2"};
    mutated.data = to_json_string(bad_schema);
    MIRA_CHECK(!parse_workflow_episode_recorded(mutated).has_value());

    // Digest shape.
    auto bad_digest = parse_json(payload.data).value();
    *bad_digest.find("episode_digest") = JsonValue{"nope"};
    mutated.data = to_json_string(bad_digest);
    MIRA_CHECK(!parse_workflow_episode_recorded(mutated).has_value());

    // Wrong type name.
    mutated = payload;
    mutated.type = "WorkflowLessonRecorded";
    MIRA_CHECK(!parse_workflow_episode_recorded(mutated).has_value());

    // The closed event set knows both new types.
    MIRA_CHECK(is_workflow_event_type("WorkflowEpisodeRecorded"));
    MIRA_CHECK(is_workflow_event_type("WorkflowLessonRecorded"));
    MIRA_CHECK(!is_workflow_event_type("WorkflowMemoryGuessed"));
    return 0;
}

int offline_replay_of_learning_events_stays_side_effect_free() {
    // W-08 assertion for the new payload family: the audit events are
    // rebuildable facts and replay never dispatches input or calls memory.
    MemoryEventStore store;

    WorkflowEpisodeRecordedEvent episode;
    episode.run_id = WorkflowRunId::generate();
    episode.workflow_id = WorkflowId::generate();
    episode.episode_digest = digest_string("episode");
    episode.outcome = "recorded";

    WorkflowLessonRecordedEvent lesson;
    lesson.run_id = episode.run_id;
    lesson.workflow_id = episode.workflow_id;
    lesson.lesson_digest = digest_string("lesson");
    lesson.outcome = "failed";
    lesson.reason_code = "mira.test:1";

    const SessionId session = SessionId::generate();
    for (const auto &payload : {to_event_payload(episode), to_event_payload(lesson)}) {
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = RuntimeId::generate();
        request.session_id = session;
        request.payload = payload;
        MIRA_CHECK(store.append(request).has_value());
    }

    EventQuery query;
    query.session_id = session;
    const auto page = store.read(query);
    MIRA_CHECK(page.has_value());
    MIRA_CHECK(page.value().events.size() == 2);
    for (const auto &envelope : page.value().events) {
        MIRA_CHECK(is_workflow_event_type(envelope.payload.type));
    }
    auto reparsed_episode = parse_workflow_episode_recorded(page.value().events[0].payload);
    MIRA_CHECK(reparsed_episode.has_value());
    MIRA_CHECK(reparsed_episode.value().outcome == "recorded");
    auto reparsed_lesson = parse_workflow_lesson_recorded(page.value().events[1].payload);
    MIRA_CHECK(reparsed_lesson.has_value());
    MIRA_CHECK(reparsed_lesson.value().reason_code == "mira.test:1");

    // Replay holds no receipts for workflow input: a dispatch attempt
    // settles uncertain instead of executing; no memory backend exists in
    // the replay path at all.
    OfflineReplayEnvironment replay{{}, {}, EnvironmentCapabilities{}};
    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    InputSequence sequence_input;
    sequence_input.events.push_back(InputEvent{.kind = "tap", .payload = "0.5,0.5"});
    const auto dispatch = replay.execute(sequence_input, context);
    MIRA_CHECK(!dispatch.has_value());
    MIRA_CHECK(dispatch.error().code == ErrorCode::ExecutionUncertain);
    MIRA_CHECK(!replay.interrupted());
    return 0;
}

} // namespace

int main() {
    const auto scenarios = std::to_array(
        {learning_payloads_round_trip, learning_payloads_fail_closed,
         offline_replay_of_learning_events_stays_side_effect_free});
    for (const auto scenario : scenarios) {
        if (const int code = scenario(); code != 0) {
            return code;
        }
    }
    std::cout << "m13 events tests passed\n";
    return 0;
}
