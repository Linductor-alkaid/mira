#pragma once

// Shared ConversationLoop supplied-input seam fixtures and event probes for
// the DEC-053 supply tests and the DEC-088 input-reuse annotation tests. The
// loop-facing behavior stays in the m3 test binaries; this header only hosts
// the recording fixture, the artifact helpers and the event summary probes.

#include "harness_support.hpp"

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

namespace mira::testing {

inline constexpr std::size_t kScreenshotBytes = 8;

// Serves fixture image bytes for direct dialect-encoder probes.
class ByteArtifactSource final : public IArtifactSource {
  public:
    [[nodiscard]] Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
        return std::vector<std::byte>(kScreenshotBytes, std::byte{0x89});
    }
};

[[nodiscard]] inline ArtifactRef ref_from(const ArtifactDescriptor &descriptor) {
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
[[nodiscard]] inline ModelInputItem user_image_item(const ArtifactRef &reference,
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
[[nodiscard]] inline const ModelInputItem *primary_user_item(const ModelRequest &request) {
    for (const auto &item : request.input) {
        if (item.role == ModelRole::User &&
            item.provenance.source == "mira.conversation-loop.request.v1") {
            return &item;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::size_t count_image_parts(const ModelRequest &request) {
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

[[nodiscard]] inline std::vector<EventEnvelope> session_events(const MemoryEventStore &store,
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
[[nodiscard]] inline std::vector<JsonValue> typed_events(const std::vector<EventEnvelope> &events,
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
[[nodiscard]] inline const JsonValue &event_summary(const JsonValue &event) {
    const auto *detail = event.find("detail");
    return detail != nullptr ? *detail : event;
}

[[nodiscard]] inline std::int64_t int_field(const JsonValue &event, const std::string &key) {
    const auto *field = event_summary(event).find(key);
    if (field == nullptr || !field->is_integer()) {
        return -1;
    }
    return field->as_integer().value_or(-1);
}

// Boolean field of a loop event summary; nullopt when absent or not a bool.
[[nodiscard]] inline std::optional<bool> bool_field(const JsonValue &event,
                                                    const std::string &key) {
    const auto *field = event_summary(event).find(key);
    if (field == nullptr || !field->is_boolean()) {
        return std::nullopt;
    }
    return field->as_boolean().value_or(false);
}

// Count of one drop reason on a ModelInputSupplied event; missing reasons
// count as zero so per-reason assertions stay order-free.
[[nodiscard]] inline std::int64_t drop_reason_count(const JsonValue &supplied_event,
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

[[nodiscard]] inline std::size_t drop_reason_kinds(const JsonValue &supplied_event) {
    const auto *reasons = event_summary(supplied_event).find("drop_reasons");
    if (reasons == nullptr || reasons->as_object() == nullptr) {
        return 0;
    }
    return reasons->as_object()->size();
}

// String field of a loop event summary, or nullptr when absent.
[[nodiscard]] inline const std::string *string_field(const JsonValue &event,
                                                     const std::string &key) {
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

[[nodiscard]] inline std::vector<JsonValue> supplied_events(const SupplyFixture &fixture) {
    return typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                        "ModelInputSupplied");
}

[[nodiscard]] inline std::vector<JsonValue> degraded_events(const SupplyFixture &fixture) {
    return typed_events(session_events(*fixture.events_, fixture.spec_.session_id),
                        "ModelInputSupplyDegraded");
}

} // namespace mira::testing
