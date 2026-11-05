#include "support/test.hpp"
#include "support/workflow_event_samples.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

int main() {
    using namespace mira;
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/workflow-event-wire-v1.json";
    std::ifstream input(path);
    MIRA_CHECK(input.good());
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto fixture = parse_json(text);
    MIRA_CHECK(fixture && fixture.value().is_array());
    const auto samples = testing::workflow_event_samples();
    const auto &expected = *fixture.value().as_array();
    MIRA_CHECK(expected.size() == 20 && samples.size() == expected.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto &record = expected[index];
        const auto *type = record.find("type");
        const auto *classification = record.find("classification");
        const auto *data = record.find("data");
        MIRA_CHECK(type && type->is_string() && *type->as_string() == samples[index].type);
        MIRA_CHECK(classification && classification->as_integer() ==
                                         static_cast<std::int64_t>(samples[index].classification));
        MIRA_CHECK(data && data->is_string() && *data->as_string() == samples[index].data);
    }

    // This public replay parser remains available even without an internal
    // consumer. Serializing is distinct from validating a publish outcome.
    auto event = testing::workflow_event_sample<WorkflowPublishRejectedEvent>();
    auto payload = to_event_payload(event);
    auto parsed = parse_workflow_publish_rejected(payload);
    MIRA_CHECK(parsed && parsed.value().workflow_id == event.workflow_id);
    MIRA_CHECK(parsed.value().reason_code == event.reason_code);
    payload.type = "WrongType";
    MIRA_CHECK(!parse_workflow_publish_rejected(payload));
    payload = to_event_payload(event);
    auto json = parse_json(payload.data).value();
    json.set("schema", "mira.workflow.publish-rejected.v999");
    payload.data = to_json_string(json);
    auto future = parse_workflow_publish_rejected(payload);
    MIRA_CHECK(!future && future.error().code == ErrorCode::UnsupportedVersion);
    payload = to_event_payload(event);
    json = parse_json(payload.data).value();
    json.set("extra", true);
    payload.data = to_json_string(json);
    MIRA_CHECK(!parse_workflow_publish_rejected(payload));
    return 0;
}
