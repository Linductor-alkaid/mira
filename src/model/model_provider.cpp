#include <mira/model_provider.hpp>
#include <mira/model_digest.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

[[nodiscard]] TransportLimits limits_from_profile(const ModelProfile &profile,
                                                 const ModelRequest &request) {
    TransportLimits limits;
    limits.deadlines = profile.deadlines;
    limits.max_response_bytes = profile.capabilities.limits.max_response_bytes;
    limits.max_redirects = profile.max_redirects;
    limits.allow_private_endpoints = false;
    if (profile.endpoint_origin.rfind("http://", 0) == 0) {
        // Plain-http origins are a test/dev configuration; production
        // profiles use https.
    }
    static_cast<void>(request);
    return limits;
}

} // namespace

const TransportTrace &IModelProvider::last_trace() const {
    static const TransportTrace kEmpty{};
    return kEmpty;
}

const SseStreamStats &IModelProvider::last_sse_stats() const {
    static const SseStreamStats kEmpty{};
    return kEmpty;
}

std::optional<std::chrono::milliseconds> IModelProvider::last_retry_after_hint() const {
    return std::nullopt;
}

RequestStage classify_provider_stage(const TransportTrace &trace, const Error &failure) {
    if (trace.headers_received) {
        return RequestStage::AwaitingResponse;
    }
    if (trace.write_completed || trace.write_started) {
        // Bytes left the process: the remote may have received the request.
        return RequestStage::AwaitingResponse;
    }
    if (failure.domain == "mira.model" &&
        failure.domain_code == static_cast<std::int32_t>(ModelDomainCode::AmbiguousCompletion)) {
        return RequestStage::AwaitingResponse;
    }
    return RequestStage::PreWriteFailure;
}

OpenAiCompatibleProvider::OpenAiCompatibleProvider(
    std::shared_ptr<const ModelProfile> profile, std::shared_ptr<IHttpTransport> transport,
    std::shared_ptr<IArtifactSource> artifacts,
    std::shared_ptr<IArtifactStore> protected_artifacts)
    : profile_(std::move(profile)), transport_(std::move(transport)),
      artifacts_(std::move(artifacts)), protected_artifacts_(std::move(protected_artifacts)) {}

OpenAiCompatibleProvider::~OpenAiCompatibleProvider() = default;

std::optional<std::chrono::milliseconds> OpenAiCompatibleProvider::last_retry_after_hint() const {
    return parse_retry_after(last_headers_, std::chrono::milliseconds{30'000});
}

std::optional<UnvalidatedModelPreview> OpenAiCompatibleProvider::take_last_preview() {
    if (last_preview_.has_value()) {
        auto preview = std::move(last_preview_);
        last_preview_.reset();
        return preview;
    }
    return std::nullopt;
}

Result<ModelResponse> OpenAiCompatibleProvider::infer(const ModelRequest &request,
                                                      const OperationContext &context,
                                                      const ProviderInferOptions &options) {
    const IDialectMapper &mapper =
        profile_->dialect == ProtocolDialect::OpenAIResponsesV1
            ? static_cast<const IDialectMapper &>(responses_)
            : static_cast<const IDialectMapper &>(chat_);

    auto wire = mapper.encode_request(request, *profile_, options.stream, *artifacts_);
    if (!wire) {
        return wire.error();
    }

    HttpRequest http;
    http.method = "POST";
    http.url = profile_->endpoint_url();
    http.body = to_json_string(wire.value());
    http.headers.emplace_back("Content-Type", "application/json");
    http.headers.emplace_back(
        "Accept", options.stream ? "text/event-stream" : "application/json");
    if (request.data_policy.organization.has_value()) {
        http.headers.emplace_back("OpenAI-Organization", *request.data_policy.organization);
    }
    if (request.data_policy.project.has_value()) {
        http.headers.emplace_back("OpenAI-Project", *request.data_policy.project);
    }
    http.authorization = profile_->credential;

    TransportTrace trace;
    last_trace_ = TransportTrace{};
    last_headers_.clear();

    const auto stash_headers = [this](const std::vector<std::pair<std::string, std::string>> &headers) {
        last_headers_ = headers;
    };
    static_cast<void>(stash_headers);

    if (options.stream) {
        if (profile_->dialect != ProtocolDialect::OpenAIResponsesV1) {
            return make_model_error(ModelDomainCode::CapabilityMismatch,
                                    "streaming is not enabled for this dialect");
        }
        ResponsesSseParser parser(request, *profile_);
        std::string raw;
        std::optional<Error> parse_failure;
        auto info = transport_->execute(
            http, limits_from_profile(*profile_, request), context,
            [&](std::string_view chunk) {
                raw.append(chunk.data(), chunk.size());
                if (parse_failure.has_value()) {
                    return; // Stop feeding after the first protocol failure.
                }
                auto status = parser.feed(chunk);
                if (!status) {
                    parse_failure = status.error();
                }
            },
            trace);
        last_trace_ = trace;
        last_headers_ = std::move(info.has_value() ? info.value().headers
                                                   : std::vector<std::pair<std::string, std::string>>{});
        sse_stats_ = parser.stats();
        last_preview_ = parser.take_preview();
        if (!info) {
            return info.error();
        }
        if (parse_failure.has_value()) {
            return parse_failure.value();
        }
        if (info.value().status < 200 || info.value().status >= 300) {
            return map_http_error_status(WireHttpResponse{
                info.value().status, std::move(info.value().headers), std::move(raw)});
        }
        auto finished = parser.finish();
        if (!finished) {
            return finished.error();
        }
        auto result = std::move(finished).value();
        result.rate_limit = parse_rate_limit_headers(info.value().headers);
        if (options.capture_raw_response && protected_artifacts_ != nullptr) {
            auto reference = store_raw_response(raw);
            if (reference) {
                result.protected_raw_response = std::move(reference).value();
            }
        }
        return result;
    }

    std::string raw;
    auto info = transport_->execute(
        http, limits_from_profile(*profile_, request), context,
        [&](std::string_view chunk) { raw.append(chunk.data(), chunk.size()); }, trace);
    last_trace_ = trace;
    if (info) {
        last_headers_ = std::move(info.value().headers);
    }
    if (!info) {
        return info.error();
    }
    auto decoded = mapper.decode_response(
        request, *profile_,
        WireHttpResponse{info.value().status, std::move(info.value().headers), std::move(raw)});
    if (!decoded) {
        return decoded.error();
    }
    if (options.capture_raw_response && protected_artifacts_ != nullptr) {
        auto reference = store_raw_response(raw);
        if (reference) {
            decoded.value().protected_raw_response = std::move(reference).value();
        }
    }
    return decoded;
}

Result<ArtifactRef> OpenAiCompatibleProvider::store_raw_response(const std::string &raw) const {
    if (protected_artifacts_ == nullptr) {
        return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                "no protected artifact store is configured");
    }
    ArtifactWriteSpec spec;
    spec.media_type = "application/json";
    spec.encoding = ArtifactEncoding::Utf8;
    spec.sensitivity = Sensitivity::Sensitive;
    spec.content_schema = SchemaVersion{1, 0};
    spec.max_bytes = 16ULL * 1024ULL * 1024ULL;
    auto writer = protected_artifacts_->begin(spec);
    if (!writer) {
        return writer.error();
    }
    if (auto written = writer.value().write(raw.data(), raw.size()); !written) {
        return written.error();
    }
    auto descriptor = protected_artifacts_->commit(writer.value());
    if (!descriptor) {
        return descriptor.error();
    }
    ArtifactRef reference;
    reference.id = descriptor.value().id;
    reference.digest = descriptor.value().digest;
    reference.byte_size = descriptor.value().byte_size;
    reference.media_type = descriptor.value().media_type;
    reference.sensitivity = Sensitivity::Sensitive;
    return reference;
}

} // namespace mira
