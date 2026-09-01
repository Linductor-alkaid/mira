#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_provider.hpp>
#include <mira/model_upload.hpp>

#include <executor/executor.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

class ExecutorFixture final {
  public:
    ExecutorFixture() {
        executor::ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 32;
        executor_.initialize(config);
    }
    ~ExecutorFixture() { static_cast<void>(executor_.shutdown(true)); }
    executor::Executor &executor() noexcept { return executor_; }

  private:
    executor::Executor executor_;
};

class FixedArtifactSource final : public IArtifactSource {
  public:
    explicit FixedArtifactSource(std::string payload) : payload_(std::move(payload)) {}

    Result<std::vector<std::byte>> fetch(const ArtifactRef &) override {
        std::vector<std::byte> bytes;
        bytes.reserve(payload_.size());
        for (const char value : payload_) {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

  private:
    std::string payload_;
};

[[nodiscard]] OperationContext context() {
    OperationContext result;
    result.operation = OperationId::generate();
    result.started_at = Timestamp::now();
    result.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    return result;
}

[[nodiscard]] ArtifactRef artifact(std::string_view payload) {
    ArtifactRef result;
    result.id = ArtifactId::generate();
    result.digest = digest_string(payload);
    result.byte_size = payload.size();
    result.media_type = "text/plain";
    result.sensitivity = Sensitivity::Internal;
    return result;
}

[[nodiscard]] ModelRequest request_with_file(const ModelProfile &profile,
                                             const ArtifactRef &source) {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.profile_id = profile.id;
    ModelInputItem input;
    input.role = ModelRole::User;
    input.content.emplace_back(TextPart{"summarize", Sensitivity::Internal});
    input.content.emplace_back(FilePart{source, "text/plain", "notes.txt"});
    request.input.push_back(std::move(input));
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    request.data_policy.allow_uploads = true;
    request.data_policy.remote_retention = std::chrono::seconds{0};
    return request;
}

int provider_uploads_binds_and_deletes() {
    ExecutorFixture fixture;
    auto secrets = std::make_shared<MapSecretResolver>();
    secrets->set("test-credential", "sk-upload-test");
    auto transport = std::make_shared<MockHttpTransport>(secrets);
    auto source = std::make_shared<FixedArtifactSource>("artifact-payload-marker");
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    profile->capabilities.upload = CapabilityFlag{true, CapabilityEvidence::FixtureVerified, ""};
    auto remote =
        std::make_shared<OpenAiRemoteFileStore>(fixture.executor(), profile, transport, source);
    OpenAiCompatibleProvider provider(profile, transport, source, nullptr, remote);

    transport->enqueue_json(200, R"({"id":"file_mira_123","object":"file"})");
    transport->enqueue_json(
        200,
        R"({"id":"resp_1","status":"completed","model":"test-model","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"done"}]}],"usage":{"input_tokens":2,"output_tokens":1}})");
    transport->enqueue_json(200, R"({"id":"file_mira_123","object":"file","deleted":true})");

    const auto source_ref = artifact("artifact-payload-marker");
    auto response = provider.infer(request_with_file(*profile, source_ref), context(), {});
    MIRA_CHECK(response.has_value());
    const auto recorded = transport->recorded();
    MIRA_CHECK(recorded.size() == 3);
    MIRA_CHECK(recorded[0].url == "https://api.test/v1/files");
    MIRA_CHECK(recorded[0].body.find("name=\"purpose\"") != std::string::npos);
    MIRA_CHECK(recorded[0].body.find("user_data") != std::string::npos);
    MIRA_CHECK(recorded[0].body.find("artifact-payload-marker") != std::string::npos);
    MIRA_CHECK(recorded[1].url == "https://api.test/v1/responses");
    MIRA_CHECK(recorded[1].body.find("\"file_id\":\"file_mira_123\"") != std::string::npos);
    MIRA_CHECK(recorded[1].body.find("artifact-payload-marker") == std::string::npos);
    MIRA_CHECK(recorded[2].url == "https://api.test/v1/files/file_mira_123");
    const auto audit = remote->audit();
    MIRA_CHECK(audit.size() == 2);
    MIRA_CHECK(audit[0].kind == RemoteFileAuditKind::Uploaded);
    MIRA_CHECK(audit[1].kind == RemoteFileAuditKind::Deleted);
    for (const auto &entry : audit) {
        MIRA_CHECK(entry.safe_detail.find("file_mira_123") == std::string::npos);
    }
    remote->shutdown();
    return 0;
}

int delayed_cleanup_is_cancelled_and_settled_on_shutdown() {
    ExecutorFixture fixture;
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    auto source = std::make_shared<FixedArtifactSource>("payload");
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    auto remote =
        std::make_shared<OpenAiRemoteFileStore>(fixture.executor(), profile, transport, source);
    const RemoteFileRef file{artifact("payload"), "file_delayed_1"};
    auto scheduled = remote->retire(file, std::chrono::seconds{60});
    MIRA_CHECK(scheduled.has_value());
    remote->shutdown();
    const auto audit = remote->audit();
    MIRA_CHECK(audit.size() == 2);
    MIRA_CHECK(audit[0].kind == RemoteFileAuditKind::DeleteScheduled);
    MIRA_CHECK(audit[1].kind == RemoteFileAuditKind::DeleteCancelled);
    MIRA_CHECK(transport->recorded().empty());
    return 0;
}

int delayed_cleanup_executes_on_executor_timer() {
    ExecutorFixture fixture;
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    auto source = std::make_shared<FixedArtifactSource>("payload");
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    auto remote =
        std::make_shared<OpenAiRemoteFileStore>(fixture.executor(), profile, transport, source);
    transport->enqueue_json(200, R"({"id":"file_delayed_2","object":"file","deleted":true})");
    const RemoteFileRef file{artifact("payload"), "file_delayed_2"};
    auto scheduled = remote->retire(file, std::chrono::seconds{1});
    MIRA_CHECK(scheduled.has_value());
    auto guard = fixture.executor().submit_delayed(1'200, [] {});
    guard.get();
    const auto audit = remote->audit();
    MIRA_CHECK(audit.size() == 2);
    MIRA_CHECK(audit[0].kind == RemoteFileAuditKind::DeleteScheduled);
    MIRA_CHECK(audit[1].kind == RemoteFileAuditKind::Deleted);
    remote->shutdown();
    return 0;
}

int policy_and_delete_failures_are_visible() {
    ExecutorFixture fixture;
    auto transport = std::make_shared<MockHttpTransport>(std::make_shared<MapSecretResolver>());
    auto source = std::make_shared<FixedArtifactSource>("sensitive");
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test"));
    auto remote =
        std::make_shared<OpenAiRemoteFileStore>(fixture.executor(), profile, transport, source);
    auto sensitive = artifact("sensitive");
    sensitive.sensitivity = Sensitivity::Sensitive;
    ModelDataPolicy policy;
    policy.allow_uploads = true;
    auto denied = remote->upload(sensitive, "secret.txt", policy, context());
    MIRA_CHECK(!denied.has_value());
    MIRA_CHECK(transport->recorded().empty());

    transport->enqueue_json(500, R"({"error":{"type":"server_error"}})");
    auto deleted =
        remote->retire(RemoteFileRef{artifact("safe"), "file_failure_1"}, std::chrono::seconds{0});
    MIRA_CHECK(!deleted.has_value());
    const auto audit = remote->audit();
    MIRA_CHECK(audit.size() == 1);
    MIRA_CHECK(audit[0].kind == RemoteFileAuditKind::DeleteFailed);
    remote->shutdown();
    return 0;
}

} // namespace

int main() {
    if (const int status = provider_uploads_binds_and_deletes(); status != 0) {
        return status;
    }
    if (const int status = delayed_cleanup_is_cancelled_and_settled_on_shutdown(); status != 0) {
        return status;
    }
    if (const int status = delayed_cleanup_executes_on_executor_timer(); status != 0) {
        return status;
    }
    if (const int status = policy_and_delete_failures_are_visible(); status != 0) {
        return status;
    }
    return 0;
}
