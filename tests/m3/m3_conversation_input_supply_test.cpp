// ConversationLoop supplied-input seam tests (DEC-053, issue #85 /
// MIRA-20261009-001): the host-controlled per-turn structured input supply.
// Covers the zero-drift regression without a supplier, the screenshot ->
// artifact -> supply -> next-request ImagePart closure, exactly-once
// resolution per assembled request, no cross-turn accumulation, the
// per-item validation drop matrix, item/byte budgets, supplier error and
// exception degradation, invalid options degradation, recovery-retry
// re-resolution, the pre-cancelled path, the no-image-capability route
// failure and the Messages-dialect ImageDetail constraint.

#include "support/harness_support.hpp"

#include "support/test.hpp"

#include <mira/artifact_store.hpp>
#include <mira/conversation_loop.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_digest.hpp>
#include <mira/model_gateway.hpp>
#include <mira/model_schema.hpp>
#include <mira/model_tool.hpp>

#include <kairo/executor.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// DEC-053 freezes the seam on the existing content vocabulary: host items
// ride the canonical content part variant, and the declared bounds stay
// positive defaults.
static_assert(std::is_constructible_v<ModelContentPart, ImagePart>);
static_assert(std::is_constructible_v<ModelContentPart, TextPart>);
static_assert(std::is_constructible_v<ModelContentPart, FilePart>);
constexpr ModelInputSupplyOptions kSupplyDefaults{};
static_assert(kSupplyDefaults.max_items == 8);
static_assert(kSupplyDefaults.max_image_bytes == 8ULL * 1024ULL * 1024ULL);

constexpr std::size_t kScreenshotBytes = 8;

// ---------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------

// Serves fixture image bytes for direct dialect-encoder probes.
class ByteArtifactSource final : public IArtifactSource {
  public:
    [[nodiscard]] Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
        return std::vector<std::byte>(kScreenshotBytes, std::byte{0x89});
    }
};

[[nodiscard]] ArtifactRef ref_from(const ArtifactDescriptor &descriptor) {
    ArtifactRef reference;
    reference.id = descriptor.id;
    reference.digest = descriptor.digest;
    reference.byte_size = descriptor.byte_size;
    reference.media_type = descriptor.media_type;
    reference.sensitivity = descriptor.sensitivity;
    return reference;
}

// A canonical User item carrying one image reference: the shape hosts offer
// through the seam.
[[nodiscard]] ModelInputItem user_image_item(const ArtifactRef &reference,
                                             ImageDetail detail = ImageDetail::Auto) {
    ModelInputItem item;
    item.role = ModelRole::User;
    ImagePart image;
    image.source = reference;
    image.detail = detail;
    image.media_type = reference.media_type;
    item.content.emplace_back(std::move(image));
    return item;
}

// The request's primary user item under the loop's own envelope.
[[nodiscard]] const ModelInputItem *primary_user_item(const ModelRequest &request) {
    for (const auto &item : request.input) {
        if (item.role == ModelRole::User &&
            item.provenance.source == "mira.conversation-loop.request.v1") {
            return &item;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t count_image_parts(const ModelRequest &request) {
    std::size_t count = 0;
    for (const auto &item : request.input) {
        for (const auto &part : item.content) {
            if (std::holds_alternative<ImagePart>(part)) {
                ++count;
            }
        }
    }
    return count;
}

[[nodiscard]] std::vector<EventEnvelope> session_events(const MemoryEventStore &store,
                                                        const SessionId &session) {
    EventQuery query;
    query.session_id = session;
    auto page = store.read(query);
    if (!page.has_value()) {
        return {};
    }
    return std::move(page.value()).events;
}

// Parsed event summaries of one payload type, in append order.
[[nodiscard]] std::vector<JsonValue> typed_events(const std::vector<EventEnvelope> &events,
                                                  const std::string &type) {
    std::vector<JsonValue> parsed;
    for (const auto &envelope : events) {
        if (envelope.payload.type != type) {
            continue;
        }
        auto decoded = parse_json(envelope.payload.data);
        if (decoded.has_value()) {
            parsed.push_back(std::move(decoded).value());
        }
    }
    return parsed;
}

// Loop events wrap their summary under the envelope's "detail" key
// (emit_loop_event: {task_id, task_epoch, detail}).
[[nodiscard]] const JsonValue &event_summary(const JsonValue &event) {
    const auto *detail = event.find("detail");
    return detail != nullptr ? *detail : event;
}

[[nodiscard]] std::int64_t int_field(const JsonValue &event, const std::string &key) {
    const auto *field = event_summary(event).find(key);
    if (field == nullptr || !field->is_integer()) {
        return -1;
    }
    return field->as_integer().value_or(-1);
}

// Count of one drop reason on a ModelInputSupplied event; missing reasons
// count as zero so per-reason assertions stay order-free.
[[nodiscard]] std::int64_t drop_reason_count(const JsonValue &supplied_event,
                                             const std::string &reason) {
    const auto *reasons = event_summary(supplied_event).find("drop_reasons");
    if (reasons == nullptr || reasons->as_object() == nullptr) {
        return -1;
    }
    for (const auto &entry : *reasons->as_object()) {
        if (entry.first == reason) {
            return entry.second.as_integer().value_or(-1);
        }
    }
    return 0;
}

[[nodiscard]] std::size_t drop_reason_kinds(const JsonValue &supplied_event) {
    const auto *reasons = event_summary(supplied_event).find("drop_reasons");
    if (reasons == nullptr || reasons->as_object() == nullptr) {
        return 0;
    }
    return reasons->as_object()->size();
}

// String field of a loop event summary, or nullptr when absent.
[[nodiscard]] const std::string *string_field(const JsonValue &event, const std::string &key) {
    const auto *field = event_summary(event).find(key);
    return field != nullptr ? field->as_string() : nullptr;
}

class SupplyFixture final {
    // Recorded flaky provider (defined at the end of this class): every
    // dispatched attempt is captured so retry paths can be asserted on the
    // request content.
    class RecordingFlakyProvider;

  public:
    explicit SupplyFixture(ProtocolDialect dialect = ProtocolDialect::OpenAIResponsesV1) {
        kairo::ExecutorConfig executor_config;
        executor_config.min_threads = 2;
        executor_config.max_threads = 2;
        executor_config.queue_capacity = 32;
        executor_.initialize(executor_config);
        profile_ = std::make_shared<ModelProfile>(make_profile(dialect, "https://api.test"));
        router_.register_profile(profile_);
        gateway_ = std::make_unique<ModelGateway>(executor_, router_, nullptr, PriceTable{},
                                                  ModelGatewayConfig{});
        admission_ = std::make_shared<SimpleAdmissionGate>();
        gateway_->set_admission_gate(admission_);

        BuiltinToolSpec echo;
        echo.wire_name = "echo";
        echo.description = "Returns the message argument verbatim for round-trip tests.";
        echo.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"message": {"type": "string"}},
            "required": ["message"],
            "additionalProperties": false
        })json")
                                                .value()};
        echo_spec_ = echo;
        (void)registry_->register_tool(std::move(echo),
                                       [](const JsonValue &arguments, const OperationContext &) {
                                           return JsonValue(JsonValue::Object{{"echo", arguments}});
                                       });

        BuiltinToolSpec screenshot;
        screenshot.wire_name = "screenshot";
        screenshot.description = "Publishes a fixture screenshot artifact into the host store.";
        screenshot.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {},
            "additionalProperties": false
        })json")
                                                      .value()};
        screenshot_spec_ = screenshot;
        (void)registry_->register_tool(
            std::move(screenshot),
            [this](const JsonValue &, const OperationContext &) -> Result<JsonValue> {
                auto published = publish_image(kScreenshotBytes);
                if (!published) {
                    Error error;
                    error.code = ErrorCode::Internal;
                    error.domain = "mira.test";
                    error.safe_message = "fixture screenshot artifact publish failed";
                    return error;
                }
                {
                    const std::lock_guard lock(captured_mutex_);
                    captured_.push_back(ref_from(published.value()));
                }
                return JsonValue(JsonValue::Object{{"artifact", published.value().id.to_string()}});
            });

        spec_.task_id = TaskId::generate();
        spec_.session_id = SessionId::generate();
        spec_.task_epoch = 1;
        spec_.profile_id = profile_->id;
        spec_.goal = "describe the captured screen";
    }

    ~SupplyFixture() { (void)executor_.shutdown(true); }

    // Publishes a synthetic PNG payload of `payload_bytes` bytes.
    [[nodiscard]] Result<ArtifactDescriptor> publish_image(std::size_t payload_bytes) {
        ArtifactWriteSpec spec;
        spec.media_type = "image/png";
        spec.encoding = ArtifactEncoding::Binary;
        spec.sensitivity = Sensitivity::Internal;
        auto writer = artifacts_.begin(spec);
        if (!writer) {
            return writer.error();
        }
        const std::vector<std::byte> payload(payload_bytes, std::byte{0x89});
        if (auto written = writer.value().write(payload); !written) {
            return written.error();
        }
        return artifacts_.commit(writer.value());
    }

    void use_provider(std::vector<ModelResponse> script) {
        provider_ = std::make_shared<RecordingProvider>(profile_, std::move(script));
        gateway_->register_provider(provider_);
    }

    // A provider whose first calls fail with scripted errors before serving
    // the scripted responses; every dispatched attempt is recorded.
    void use_flaky_provider(std::vector<Error> failures, std::vector<ModelResponse> script) {
        provider_ = nullptr;
        flaky_ = std::make_shared<RecordingFlakyProvider>(profile_, std::move(failures),
                                                          std::move(script));
        gateway_->register_provider(flaky_);
    }

    [[nodiscard]] ConversationLoop make_loop(const ConversationLoopConfig &config) {
        ConversationLoop loop(*gateway_, config);
        loop.set_event_store(events_, runtime_, spec_.session_id);
        loop.set_tool_registry(registry_);
        admission_->activate(spec_.task_id, spec_.task_epoch);
        return loop;
    }

    kairo::Executor executor_;
    MemoryArtifactStore artifacts_;
    std::shared_ptr<ModelProfile> profile_;
    ModelRouter router_;
    std::unique_ptr<ModelGateway> gateway_;
    std::shared_ptr<SimpleAdmissionGate> admission_;
    std::shared_ptr<RecordingProvider> provider_;
    std::shared_ptr<BuiltinToolRegistry> registry_ = std::make_shared<BuiltinToolRegistry>();
    BuiltinToolSpec echo_spec_;
    BuiltinToolSpec screenshot_spec_;
    std::shared_ptr<MemoryEventStore> events_ = std::make_shared<MemoryEventStore>();
    RuntimeId runtime_ = RuntimeId::generate();
    AgentLoopSpec spec_;

    mutable std::mutex captured_mutex_;
    std::vector<ArtifactRef> captured_;
    std::shared_ptr<RecordingFlakyProvider> flaky_;

  private:
    // Like the conversation-loop fixture's FlakyProvider, but records every
    // dispatched attempt so retry paths can be asserted on request content.
    class RecordingFlakyProvider final : public IModelProvider {
      public:
        RecordingFlakyProvider(std::shared_ptr<const ModelProfile> profile,
                               std::vector<Error> failures, std::vector<ModelResponse> script)
            : profile_(std::move(profile)), failures_(std::move(failures)),
              script_(std::move(script)) {}

        [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }
        [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                                  const OperationContext &context,
                                                  const ProviderInferOptions &) override {
            if (context.cancelled()) {
                return make_model_error(ModelDomainCode::ModelCancelled, "flaky provider cancelled",
                                        false, request.operation_id);
            }
            const std::lock_guard lock(mutex_);
            requests_.push_back(request);
            if (failure_cursor_ < failures_.size()) {
                return failures_[failure_cursor_++];
            }
            if (script_cursor_ >= script_.size()) {
                return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                        "flaky provider is exhausted", false, request.operation_id);
            }
            ModelResponse response = script_[script_cursor_++];
            response.request_id = request.request_id;
            response.operation_id = request.operation_id;
            response.profile_id = request.profile_id;
            return response;
        }

        [[nodiscard]] std::vector<ModelRequest> requests() const {
            const std::lock_guard lock(mutex_);
            return requests_;
        }

      private:
        std::shared_ptr<const ModelProfile> profile_;
        mutable std::mutex mutex_;
        std::vector<Error> failures_;
        std::vector<ModelResponse> script_;
        std::vector<ModelRequest> requests_;
        std::size_t failure_cursor_ = 0;
        std::size_t script_cursor_ = 0;
    };
};

[[nodiscard]] std::vector<JsonValue> supplied_events(const SupplyFixture &fixture) {
    return typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                        "ModelInputSupplied");
}

[[nodiscard]] std::vector<JsonValue> degraded_events(const SupplyFixture &fixture) {
    return typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                        "ModelInputSupplyDegraded");
}

// ---------------------------------------------------------------------------
// 1. Zero-drift regression: without a supplier nothing changes.
// ---------------------------------------------------------------------------

int no_supplier_keeps_requests_and_events_unchanged() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("plain answer")});
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "plain answer");
    MIRA_CHECK(result.value().supplied_input_items == 0);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(result.value().supply_degradations == 0);

    MIRA_CHECK(supplied_events(fixture).empty());
    MIRA_CHECK(degraded_events(fixture).empty());

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    const auto *user = primary_user_item(requests.front());
    MIRA_CHECK(user != nullptr);
    MIRA_CHECK(user->content.size() == 1);
    const auto *goal = std::get_if<TextPart>(&user->content.front());
    MIRA_CHECK(goal != nullptr);
    MIRA_CHECK(goal->text == "Request: describe the captured screen");
    MIRA_CHECK(count_image_parts(requests.front()) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 2. Core closure: screenshot tool -> artifact -> supply -> next request.
// ---------------------------------------------------------------------------

int screenshot_artifact_flows_into_next_request() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-1"),
        text_response("I can see the captured screen"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&fixture](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            std::vector<ModelInputItem> items;
            const std::lock_guard lock(fixture.captured_mutex_);
            for (const auto &reference : fixture.captured_) {
                items.push_back(user_image_item(reference));
            }
            return items;
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "I can see the captured screen");
    MIRA_CHECK(result.value().tool_executions == 1);
    MIRA_CHECK(result.value().supplied_input_items == 1);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(result.value().supply_degradations == 0);

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    MIRA_CHECK(count_image_parts(requests[0]) == 0);

    // The second request's primary user item keeps the loop envelope and
    // carries the supplied image after the goal text.
    const auto *user = primary_user_item(requests[1]);
    MIRA_CHECK(user != nullptr);
    MIRA_CHECK(user->authority == Sensitivity::Internal);
    MIRA_CHECK(user->content.size() == 2);
    const auto *goal = std::get_if<TextPart>(&user->content.front());
    MIRA_CHECK(goal != nullptr && goal->text == "Request: describe the captured screen");
    const auto *image = std::get_if<ImagePart>(&user->content.back());
    MIRA_CHECK(image != nullptr);
    MIRA_CHECK(image->media_type == "image/png");

    // The image reference matches the published artifact (identity fields
    // only; payload bytes stay in the artifact store).
    std::optional<ArtifactRef> published;
    {
        const std::lock_guard lock(fixture.captured_mutex_);
        MIRA_CHECK(fixture.captured_.size() == 1);
        published = fixture.captured_.front();
    }
    MIRA_CHECK(image->source.id == published->id);
    MIRA_CHECK(image->source.digest == published->digest);
    MIRA_CHECK(image->source.byte_size == published->byte_size);
    MIRA_CHECK(image->source.byte_size == kScreenshotBytes);
    MIRA_CHECK(image->source.media_type == "image/png");

    // The canonical tool round trip stays intact in the same request and
    // carries no supplied image.
    const ToolCallPart *call = nullptr;
    const ToolResultPart *tool_result = nullptr;
    for (std::size_t index = 2; index < requests[1].input.size(); ++index) {
        const auto &item = requests[1].input[index];
        for (const auto &part : item.content) {
            if (const auto *call_part = std::get_if<ToolCallPart>(&part)) {
                MIRA_CHECK(item.role == ModelRole::Assistant);
                call = call_part;
            } else if (const auto *result_part = std::get_if<ToolResultPart>(&part)) {
                MIRA_CHECK(item.role == ModelRole::User);
                tool_result = result_part;
            } else {
                MIRA_CHECK(!std::holds_alternative<ImagePart>(part));
            }
        }
    }
    MIRA_CHECK(call != nullptr && call->provider_call_id.value == "call-1");
    MIRA_CHECK(call->wire_name == "screenshot");
    MIRA_CHECK(tool_result != nullptr);
    MIRA_CHECK(tool_result->provider_call_id.value == "call-1");
    MIRA_CHECK(!tool_result->failed);

    // One supply event per request: empty on turn 1, one accepted image on
    // turn 2.
    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 2);
    MIRA_CHECK(int_field(supplied[0], "offered_items") == 0);
    MIRA_CHECK(int_field(supplied[0], "accepted_items") == 0);
    MIRA_CHECK(int_field(supplied[1], "offered_items") == 1);
    MIRA_CHECK(int_field(supplied[1], "accepted_items") == 1);
    MIRA_CHECK(int_field(supplied[1], "image_bytes") ==
               static_cast<std::int64_t>(kScreenshotBytes));
    return 0;
}

// ---------------------------------------------------------------------------
// 3. Exactly one seam resolution per assembled request.
// ---------------------------------------------------------------------------

int supplier_resolved_exactly_once_per_request() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.echo_spec_, R"json({"message":"ping"})json", "call-1"),
        text_response("done after tool"),
    });
    int resolve_calls = 0;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            return std::vector<ModelInputItem>{};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 0);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(result.value().supply_degradations == 0);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 2);
    MIRA_CHECK(resolve_calls == 2);
    MIRA_CHECK(supplied_events(fixture).size() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// 4. Supplied content never accumulates across turns.
// ---------------------------------------------------------------------------

int supplied_content_does_not_accumulate_across_turns() {
    SupplyFixture fixture;
    fixture.use_provider({
        tool_call_response(fixture.screenshot_spec_, R"json({})json", "call-1"),
        tool_call_response(fixture.echo_spec_, R"json({"message":"again"})json", "call-2"),
        text_response("final answer"),
    });
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    int resolve_calls = 0;
    loop.set_model_input_supplier(
        [&fixture, &resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            std::vector<ModelInputItem> items;
            const std::lock_guard lock(fixture.captured_mutex_);
            for (const auto &reference : fixture.captured_) {
                items.push_back(user_image_item(reference));
            }
            fixture.captured_.clear(); // Host policy: replay nothing next turn.
            return items;
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 1);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(resolve_calls == 3);

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(count_image_parts(requests[0]) == 0);
    MIRA_CHECK(count_image_parts(requests[1]) == 1);
    // The third request replays only the canonical tool history: the offered
    // image entered one request and does not come back on its own.
    MIRA_CHECK(count_image_parts(requests[2]) == 0);
    std::size_t tool_calls = 0;
    std::size_t tool_results = 0;
    for (const auto &item : requests[2].input) {
        for (const auto &part : item.content) {
            tool_calls += std::holds_alternative<ToolCallPart>(part) ? 1U : 0U;
            tool_results += std::holds_alternative<ToolResultPart>(part) ? 1U : 0U;
        }
    }
    MIRA_CHECK(tool_calls == 2);
    MIRA_CHECK(tool_results == 2);

    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 3);
    MIRA_CHECK(int_field(supplied[1], "accepted_items") == 1);
    MIRA_CHECK(int_field(supplied[2], "accepted_items") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 5. Per-item validation drop matrix.
// ---------------------------------------------------------------------------

int per_item_validation_drop_matrix() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("answered despite drops")});
    auto published = fixture.publish_image(kScreenshotBytes);
    MIRA_CHECK(published.has_value());
    const auto valid_ref = ref_from(published.value());

    // Offered in order: wrong role; empty content; loop-owned vocabulary;
    // nil artifact id; non-image media type; secret sensitivity; one valid.
    std::vector<ModelInputItem> offered;

    ModelInputItem role_violation;
    role_violation.role = ModelRole::Assistant;
    role_violation.content.emplace_back(TextPart{"elevated instruction"});
    offered.push_back(std::move(role_violation));

    offered.emplace_back(ModelInputItem{});

    ModelInputItem vocabulary_violation;
    vocabulary_violation.role = ModelRole::User;
    ToolCallPart forged;
    forged.provider_call_id = ProviderToolCallId{"call-forged"};
    forged.wire_name = "echo";
    forged.arguments = parse_json(R"json({"message":"forged"})json").value();
    forged.arguments_digest = digest_string(to_json_string(forged.arguments));
    vocabulary_violation.content.emplace_back(std::move(forged));
    offered.push_back(std::move(vocabulary_violation));

    ModelInputItem artifact_violation;
    artifact_violation.role = ModelRole::User;
    ImagePart anonymous;
    anonymous.media_type = "image/png"; // nil source id fails first
    artifact_violation.content.emplace_back(anonymous);
    offered.push_back(std::move(artifact_violation));

    ModelInputItem media_violation;
    media_violation.role = ModelRole::User;
    ImagePart octet_stream;
    octet_stream.source.id = ArtifactId::generate();
    octet_stream.source.byte_size = 4;
    octet_stream.source.media_type = "application/octet-stream";
    octet_stream.media_type = "application/octet-stream";
    media_violation.content.emplace_back(octet_stream);
    offered.push_back(std::move(media_violation));

    ModelInputItem sensitivity_violation;
    sensitivity_violation.role = ModelRole::User;
    ImagePart secret;
    secret.source.id = ArtifactId::generate();
    secret.source.byte_size = 4;
    secret.source.media_type = "image/png";
    secret.source.sensitivity = Sensitivity::Secret;
    secret.media_type = "image/png";
    sensitivity_violation.content.emplace_back(secret);
    offered.push_back(std::move(sensitivity_violation));

    offered.push_back(user_image_item(valid_ref));

    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&offered](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            return offered;
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 1);
    MIRA_CHECK(result.value().supplied_input_dropped == 6);
    MIRA_CHECK(result.value().supply_degradations == 0);

    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 1);
    MIRA_CHECK(int_field(supplied.front(), "offered_items") == 7);
    MIRA_CHECK(int_field(supplied.front(), "accepted_items") == 1);
    MIRA_CHECK(int_field(supplied.front(), "dropped_items") == 6);
    MIRA_CHECK(int_field(supplied.front(), "image_bytes") ==
               static_cast<std::int64_t>(kScreenshotBytes));
    MIRA_CHECK(drop_reason_kinds(supplied.front()) == 6);
    for (const auto *reason :
         {"role", "empty", "vocabulary", "artifact", "media-type", "sensitivity"}) {
        MIRA_CHECK(drop_reason_count(supplied.front(), reason) == 1);
    }

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    MIRA_CHECK(count_image_parts(requests.front()) == 1);
    const auto *user = primary_user_item(requests.front());
    MIRA_CHECK(user != nullptr);
    const auto *image = std::get_if<ImagePart>(&user->content.back());
    MIRA_CHECK(image != nullptr && image->source.id == valid_ref.id);
    return 0;
}

// ---------------------------------------------------------------------------
// 6. Item budget: entries beyond max_items drop in supply order.
// ---------------------------------------------------------------------------

int item_budget_drops_beyond_max_items() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("answered")});
    std::vector<ArtifactRef> refs;
    for (int index = 0; index < 3; ++index) {
        auto published = fixture.publish_image(kScreenshotBytes);
        MIRA_CHECK(published.has_value());
        refs.push_back(ref_from(published.value()));
    }
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&refs](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            std::vector<ModelInputItem> items;
            for (const auto &reference : refs) {
                items.push_back(user_image_item(reference));
            }
            return items;
        },
        ModelInputSupplyOptions{/*max_items=*/2, /*max_image_bytes=*/8ULL * 1024ULL * 1024ULL});
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 2);
    MIRA_CHECK(result.value().supplied_input_dropped == 1);
    MIRA_CHECK(result.value().supply_degradations == 0);

    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 1);
    MIRA_CHECK(int_field(supplied.front(), "offered_items") == 3);
    MIRA_CHECK(int_field(supplied.front(), "accepted_items") == 2);
    MIRA_CHECK(int_field(supplied.front(), "dropped_items") == 1);
    MIRA_CHECK(drop_reason_kinds(supplied.front()) == 1);
    MIRA_CHECK(drop_reason_count(supplied.front(), "item-budget") == 1);
    MIRA_CHECK(int_field(supplied.front(), "image_bytes") ==
               static_cast<std::int64_t>(2 * kScreenshotBytes));

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    MIRA_CHECK(count_image_parts(requests.front()) == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// 7. Byte budget: an item that would overflow max_image_bytes drops whole.
// ---------------------------------------------------------------------------

int image_byte_budget_drops_over_limit_item() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("answered")});
    std::vector<ArtifactRef> refs;
    for (int index = 0; index < 2; ++index) {
        auto published = fixture.publish_image(kScreenshotBytes);
        MIRA_CHECK(published.has_value());
        refs.push_back(ref_from(published.value()));
    }
    ModelInputSupplyOptions options;
    options.max_image_bytes = 10; // one 8-byte image fits, two do not
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&refs](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            std::vector<ModelInputItem> items;
            for (const auto &reference : refs) {
                items.push_back(user_image_item(reference));
            }
            return items;
        },
        options);
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 1);
    MIRA_CHECK(result.value().supplied_input_dropped == 1);
    MIRA_CHECK(result.value().supply_degradations == 0);

    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 1);
    MIRA_CHECK(int_field(supplied.front(), "offered_items") == 2);
    MIRA_CHECK(int_field(supplied.front(), "accepted_items") == 1);
    MIRA_CHECK(int_field(supplied.front(), "dropped_items") == 1);
    MIRA_CHECK(int_field(supplied.front(), "image_bytes") ==
               static_cast<std::int64_t>(kScreenshotBytes));
    MIRA_CHECK(drop_reason_kinds(supplied.front()) == 1);
    MIRA_CHECK(drop_reason_count(supplied.front(), "image-budget") == 1);

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    MIRA_CHECK(count_image_parts(requests.front()) == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// 8. Supplier Error degrades to a diagnostic and keeps the loop running.
// ---------------------------------------------------------------------------

int supplier_error_degrades_without_blocking() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("answered without supply")});
    int resolve_calls = 0;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            Error error;
            error.code = ErrorCode::PermissionDenied;
            error.domain = "mira.test";
            error.safe_message = "screen capture is not available";
            return error;
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "answered without supply");
    MIRA_CHECK(result.value().supplied_input_items == 0);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(result.value().supply_degradations == 1);
    MIRA_CHECK(resolve_calls == 1);

    const auto degraded = degraded_events(fixture);
    MIRA_CHECK(degraded.size() == 1);
    const auto *reason = string_field(degraded.front(), "reason");
    MIRA_CHECK(reason != nullptr && *reason == "supplier-error");
    MIRA_CHECK(int_field(degraded.front(), "code") ==
               static_cast<std::int64_t>(ErrorCode::PermissionDenied));
    // A degraded turn carries no supply summary.
    MIRA_CHECK(supplied_events(fixture).empty());

    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    MIRA_CHECK(count_image_parts(requests.front()) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 9. Supplier exceptions degrade identically and never escape run().
// ---------------------------------------------------------------------------

int supplier_exception_degrades_without_escaping() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("answered after exception")});
    int resolve_calls = 0;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            throw std::runtime_error("supplier exploded");
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "answered after exception");
    MIRA_CHECK(result.value().supply_degradations == 1);
    MIRA_CHECK(result.value().supplied_input_items == 0);
    MIRA_CHECK(resolve_calls == 1);

    const auto degraded = degraded_events(fixture);
    MIRA_CHECK(degraded.size() == 1);
    const auto *reason = string_field(degraded.front(), "reason");
    MIRA_CHECK(reason != nullptr && *reason == "supplier-exception");
    // The exception text stays off the event surface.
    MIRA_CHECK(to_json_string(degraded.front()).find("supplier exploded") == std::string::npos);
    MIRA_CHECK(supplied_events(fixture).empty());
    MIRA_CHECK(count_image_parts(fixture.provider_->requests().front()) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 10. Invalid options degrade visibly instead of folding unbounded content.
// ---------------------------------------------------------------------------

int invalid_supply_options_degrade_visibly() {
    MIRA_CHECK(ModelInputSupplyOptions{}.validate().has_value());
    const auto zeroed = ModelInputSupplyOptions{0, 0};
    MIRA_CHECK(!zeroed.validate().has_value());
    MIRA_CHECK(zeroed.validate().error().code == ErrorCode::InvalidArgument);

    SupplyFixture fixture;
    fixture.use_provider({text_response("still answered")});
    auto published = fixture.publish_image(kScreenshotBytes);
    MIRA_CHECK(published.has_value());
    const auto reference = ref_from(published.value());
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&reference](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            return std::vector<ModelInputItem>{user_image_item(reference)};
        },
        ModelInputSupplyOptions{0, 0});
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supply_degradations == 1);
    MIRA_CHECK(result.value().supplied_input_items == 0);

    const auto degraded = degraded_events(fixture);
    MIRA_CHECK(degraded.size() == 1);
    const auto *reason = string_field(degraded.front(), "reason");
    MIRA_CHECK(reason != nullptr && *reason == "invalid-supply-options");
    MIRA_CHECK(supplied_events(fixture).empty());
    MIRA_CHECK(count_image_parts(fixture.provider_->requests().front()) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 11. Recovery retries re-resolve the seam: the retry never silently loses
// the image.
// ---------------------------------------------------------------------------

int recovery_retry_re_resolves_and_replays_image() {
    SupplyFixture fixture;
    fixture.use_flaky_provider(
        {make_model_error(ModelDomainCode::RateLimited, "rate limited", true, std::nullopt),
         make_model_error(ModelDomainCode::RateLimited, "rate limited", true, std::nullopt)},
        {text_response("recovered with the image")});
    auto published = fixture.publish_image(kScreenshotBytes);
    MIRA_CHECK(published.has_value());
    const auto reference = ref_from(published.value());
    int resolve_calls = 0;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&reference, &resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            return std::vector<ModelInputItem>{user_image_item(reference)};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().answer == "recovered with the image");
    MIRA_CHECK(result.value().recoveries == 1);
    MIRA_CHECK(result.value().supplied_input_items == 2);
    MIRA_CHECK(result.value().supply_degradations == 0);
    MIRA_CHECK(resolve_calls == 2);

    // Every dispatched attempt — the failed turn, its gateway retry and the
    // recovery turn — carried the same supplied image reference.
    MIRA_CHECK(fixture.flaky_ != nullptr);
    const auto attempts = fixture.flaky_->requests();
    MIRA_CHECK(attempts.size() == 3);
    for (const auto &attempt : attempts) {
        MIRA_CHECK(count_image_parts(attempt) == 1);
        const auto *user = primary_user_item(attempt);
        MIRA_CHECK(user != nullptr);
        const auto *image = std::get_if<ImagePart>(&user->content.back());
        MIRA_CHECK(image != nullptr && image->source.id == reference.id);
    }
    const auto supplied = supplied_events(fixture);
    MIRA_CHECK(supplied.size() == 2);
    MIRA_CHECK(int_field(supplied[0], "accepted_items") == 1);
    MIRA_CHECK(int_field(supplied[1], "accepted_items") == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// 12. Pre-cancelled contexts settle before the seam or the model run.
// ---------------------------------------------------------------------------

int pre_cancelled_context_never_resolves_or_calls_model() {
    SupplyFixture fixture;
    fixture.use_provider({text_response("never sent")});
    int resolve_calls = 0;
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&resolve_calls](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            ++resolve_calls;
            return std::vector<ModelInputItem>{};
        });
    auto context = plain_loop_context();
    context.cancellation_requested = [] { return true; };
    const auto result = loop.run(fixture.spec_, context);
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Cancelled);
    MIRA_CHECK(fixture.provider_->requests().empty());
    MIRA_CHECK(resolve_calls == 0);
    MIRA_CHECK(result.value().supplied_input_items == 0);
    MIRA_CHECK(result.value().supplied_input_dropped == 0);
    MIRA_CHECK(result.value().supply_degradations == 0);
    MIRA_CHECK(supplied_events(fixture).empty());
    MIRA_CHECK(degraded_events(fixture).empty());
    return 0;
}

// ---------------------------------------------------------------------------
// 13. Profiles without image input reject image-bearing requests visibly.
// ---------------------------------------------------------------------------

int profile_without_image_capability_fails_explicitly() {
    SupplyFixture fixture;
    fixture.profile_->capabilities.image_input =
        CapabilityFlag{false, CapabilityEvidence::Configured, ""};
    fixture.use_provider({text_response("never reached")});
    auto published = fixture.publish_image(kScreenshotBytes);
    MIRA_CHECK(published.has_value());
    const auto reference = ref_from(published.value());
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&reference](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            return std::vector<ModelInputItem>{user_image_item(reference)};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    // The route gate fails the image-bearing request loudly, never silently
    // answering without the supplied content.
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Failed);
    MIRA_CHECK(result.value().answer.empty());
    MIRA_CHECK(result.value().safe_summary.find("model call failed") != std::string::npos);
    MIRA_CHECK(fixture.provider_->requests().empty());
    // The item was accepted into the assembled request; the visible failure
    // comes from routing (CapabilityMismatch), not from a silent drop.
    MIRA_CHECK(result.value().supplied_input_items == 1);
    MIRA_CHECK(result.value().supply_degradations == 0);
    MIRA_CHECK(degraded_events(fixture).empty());
    return 0;
}

// ---------------------------------------------------------------------------
// 14. Messages-dialect ImageDetail constraint stays explicit.
//
// The enforced dialect contract (model_anthropic.cpp, also pinned by
// m3_anthropic_test): Messages carries exactly one verified canonical image
// mapping — ImageDetail::Auto. Any explicit detail (Low/High/Original) is
// rejected with CapabilityMismatch ("no verified canonical mapping") instead
// of degrading silently. The seam forwards `detail` untouched, so hosts keep
// control and the mismatch surfaces at the dialect boundary, never as a loop
// side effect. NOTE: DEC-053 section 5 words this constraint in the opposite
// direction ("Auto rejected"); the mapper code and the m3_anthropic_test pin
// the enforced direction verified here.
// ---------------------------------------------------------------------------

int messages_dialect_image_detail_constraint_stays_explicit() {
    const auto profile = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://api.test");
    AnthropicMessagesV1Mapper mapper;
    ByteArtifactSource artifacts;

    // A request shaped like the loop's assembly: system item plus the
    // loop-envelope user item carrying goal text and one supplied image.
    const auto build_request = [](ImageDetail detail) {
        ModelRequest request;
        request.contract_version = SchemaVersion{1, 0};
        request.request_id = ModelRequestId::generate();
        request.operation_id = OperationId::generate();
        request.task_id = TaskId::generate();
        request.profile_id = ModelProfileId::generate();
        ModelInputItem system_item;
        system_item.role = ModelRole::System;
        system_item.content.emplace_back(TextPart{"be brief"});
        ModelInputItem user_item;
        user_item.role = ModelRole::User;
        user_item.provenance.source = "mira.conversation-loop.request.v1";
        user_item.authority = Sensitivity::Internal;
        user_item.content.emplace_back(TextPart{"Request: describe the screen"});
        ImagePart image;
        image.source.id = ArtifactId::generate();
        image.source.byte_size = kScreenshotBytes;
        image.source.media_type = "image/png";
        image.detail = detail;
        image.media_type = "image/png";
        user_item.content.emplace_back(std::move(image));
        request.input.push_back(std::move(system_item));
        request.input.push_back(std::move(user_item));
        request.output_contract.mode = OutputMode::Text;
        request.generation.max_output_tokens = 1024;
        request.data_policy.store = false;
        return request;
    };

    // An explicit detail has no verified canonical mapping on Messages.
    auto explicit_detail =
        mapper.encode_request(build_request(ImageDetail::Low), profile, false, artifacts);
    MIRA_CHECK(!explicit_detail.has_value());
    MIRA_CHECK(explicit_detail.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));
    MIRA_CHECK(explicit_detail.error().safe_message.find("no verified canonical mapping") !=
               std::string::npos);

    // The seam's default (Auto) is the one verified mapping and encodes.
    auto auto_detail =
        mapper.encode_request(build_request(ImageDetail::Auto), profile, false, artifacts);
    MIRA_CHECK(auto_detail.has_value());

    // Loop level on a Messages profile over the canonical RecordingProvider
    // layer (no wire encoding in this fixture): the supplied image answers
    // and keeps its detail — the loop itself adds no silent dialect drop.
    SupplyFixture fixture(ProtocolDialect::AnthropicMessagesV1);
    fixture.use_provider({text_response("messages answer")});
    auto published = fixture.publish_image(kScreenshotBytes);
    MIRA_CHECK(published.has_value());
    const auto reference = ref_from(published.value());
    auto loop = fixture.make_loop(ConversationLoopConfig{});
    loop.set_model_input_supplier(
        [&reference](const AgentLoopSpec &) -> Result<std::vector<ModelInputItem>> {
            return std::vector<ModelInputItem>{user_image_item(reference, ImageDetail::Auto)};
        });
    const auto result = loop.run(fixture.spec_, plain_loop_context());
    MIRA_CHECK(result.has_value());
    MIRA_CHECK(result.value().outcome == ConversationOutcome::Answered);
    MIRA_CHECK(result.value().supplied_input_items == 1);
    const auto requests = fixture.provider_->requests();
    MIRA_CHECK(requests.size() == 1);
    const auto *user = primary_user_item(requests.front());
    MIRA_CHECK(user != nullptr);
    const auto *image = std::get_if<ImagePart>(&user->content.back());
    MIRA_CHECK(image != nullptr);
    MIRA_CHECK(image->detail == ImageDetail::Auto);
    MIRA_CHECK(image->source.id == reference.id);
    return 0;
}

} // namespace

int main() {
    if (const int code = no_supplier_keeps_requests_and_events_unchanged(); code != 0) {
        return code;
    }
    if (const int code = screenshot_artifact_flows_into_next_request(); code != 0) {
        return code;
    }
    if (const int code = supplier_resolved_exactly_once_per_request(); code != 0) {
        return code;
    }
    if (const int code = supplied_content_does_not_accumulate_across_turns(); code != 0) {
        return code;
    }
    if (const int code = per_item_validation_drop_matrix(); code != 0) {
        return code;
    }
    if (const int code = item_budget_drops_beyond_max_items(); code != 0) {
        return code;
    }
    if (const int code = image_byte_budget_drops_over_limit_item(); code != 0) {
        return code;
    }
    if (const int code = supplier_error_degrades_without_blocking(); code != 0) {
        return code;
    }
    if (const int code = supplier_exception_degrades_without_escaping(); code != 0) {
        return code;
    }
    if (const int code = invalid_supply_options_degrade_visibly(); code != 0) {
        return code;
    }
    if (const int code = recovery_retry_re_resolves_and_replays_image(); code != 0) {
        return code;
    }
    if (const int code = pre_cancelled_context_never_resolves_or_calls_model(); code != 0) {
        return code;
    }
    if (const int code = profile_without_image_capability_fails_explicitly(); code != 0) {
        return code;
    }
    return messages_dialect_image_detail_constraint_stays_explicit();
}
