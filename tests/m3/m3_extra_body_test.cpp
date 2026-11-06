#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_digest.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace {
using namespace mira;
using namespace mira::testing;

ModelRequest request_for(const ModelProfile &profile) {
    ModelRequest request;
    request.profile_id = profile.id;
    request.output_contract.mode = OutputMode::Text;
    request.data_policy.store = false;
    request.generation.max_output_tokens = 128;
    ModelInputItem user;
    user.role = ModelRole::User;
    user.content.emplace_back(TextPart{"hello"});
    request.input.push_back(std::move(user));
    return request;
}

JsonValue::Object extensions() {
    return {{"chat_template_kwargs", JsonValue::Object{{"enable_thinking", false}}},
            {"vendor_sampling",
             JsonValue::Object{{"choices", JsonValue::Array{1, 2}}, {"weight", 0.25}}},
            {"vendor_flag", true},
            {"vendor_null", nullptr},
            {"vendor_label", "fixture"}};
}

int manifest_routing_and_wire() {
    auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    auto request = request_for(profile);
    NullArtifactSource artifacts;
    ChatCompletionsV1Mapper mapper;
    const auto original_manifest = profile.manifest_to_json();
    const auto original_digest = profile.profile_digest();
    MIRA_CHECK(original_manifest.find("extra_body") == nullptr);
    const auto original_wire = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(original_wire);

    profile.extra_body = extensions();
    MIRA_CHECK(profile.validate());
    const auto manifest = profile.manifest_to_json();
    MIRA_CHECK(*manifest.find("extra_body") == JsonValue(extensions()));
    const auto digest = profile.profile_digest();
    MIRA_CHECK(digest != original_digest);
    ModelRouter router;
    router.register_profile(std::make_shared<const ModelProfile>(profile));
    const auto route = router.route(RouteQuery{});
    MIRA_CHECK(route);
    MIRA_CHECK(route.value().profile_digest == digest);

    for (bool stream : {false, true}) {
        const auto wire = mapper.encode_request(request, profile, stream, artifacts);
        MIRA_CHECK(wire);
        MIRA_CHECK(parse_json(to_json_string(wire.value())));
        for (const auto &[key, value] : profile.extra_body) {
            MIRA_CHECK(wire.value().find(key));
            MIRA_CHECK(*wire.value().find(key) == value);
        }
        for (const auto &[key, value] : *original_wire.value().as_object()) {
            MIRA_CHECK(*wire.value().find(key) == value);
        }
        MIRA_CHECK(wire.value().find("max_completion_tokens")->as_integer() == 128);
        MIRA_CHECK(wire.value().find("extra_body") == nullptr);
        MIRA_CHECK(wire_request_digest(wire.value(), {}) !=
                   wire_request_digest(original_wire.value(), {}));
        if (stream) {
            MIRA_CHECK(wire.value().find("stream")->as_boolean() == true);
            MIRA_CHECK(wire.value().find("stream_options")->find("include_usage")->as_boolean() ==
                       true);
        }
    }

    auto reordered = profile;
    std::reverse(reordered.extra_body.begin(), reordered.extra_body.end());
    reordered.extra_body[3].second =
        JsonValue::Object{{"weight", 0.25}, {"choices", JsonValue::Array{1, 2}}};
    MIRA_CHECK(reordered.validate());
    MIRA_CHECK(reordered.profile_digest() == digest);
    const auto wire = mapper.encode_request(request, profile, false, artifacts);
    const auto reordered_wire = mapper.encode_request(request, reordered, false, artifacts);
    MIRA_CHECK(reordered_wire);
    MIRA_CHECK(wire_request_digest(wire.value(), {}) ==
               wire_request_digest(reordered_wire.value(), {}));
    auto changed = profile;
    changed.extra_body[0].second.set("enable_thinking", true);
    MIRA_CHECK(changed.profile_digest() != digest);
    router.register_profile(std::make_shared<const ModelProfile>(changed));
    const auto rerouted = router.route(RouteQuery{});
    MIRA_CHECK(rerouted);
    MIRA_CHECK(rerouted.value().selected_profile == route.value().selected_profile);
    MIRA_CHECK(rerouted.value().profile_digest == changed.profile_digest());
    MIRA_CHECK(rerouted.value().profile_digest != route.value().profile_digest);
    const auto changed_wire = mapper.encode_request(request, changed, false, artifacts);
    MIRA_CHECK(changed_wire);
    MIRA_CHECK(wire_request_digest(wire.value(), {}) !=
               wire_request_digest(changed_wire.value(), {}));
    profile.extra_body.clear();
    MIRA_CHECK(profile.manifest_to_json() == original_manifest);
    MIRA_CHECK(profile.profile_digest() == original_digest);
    MIRA_CHECK(mapper.encode_request(request, profile, false, artifacts).value() ==
               original_wire.value());

    // A configured vendor flag cannot grant canonical thinking or SSE capability.
    profile.extra_body = extensions();
    request.generation.thinking = ThinkingMode::Disabled;
    MIRA_CHECK(!mapper.encode_request(request, profile, false, artifacts));
    request.generation.thinking.reset();
    profile.capabilities.sse.supported = false;
    MIRA_CHECK(!mapper.encode_request(request, profile, true, artifacts));
    return 0;
}

int invalid_configuration_fails_closed() {
    auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    const auto request = request_for(profile);
    NullArtifactSource artifacts;
    ChatCompletionsV1Mapper mapper;
    // Include fields that are absent in this plain-text request or unsupported
    // by the canonical dialect, so collision checks cannot depend on emission.
    constexpr std::array reserved = {"model",
                                     "messages",
                                     "tools",
                                     "tool_choice",
                                     "parallel_tool_calls",
                                     "response_format",
                                     "max_tokens",
                                     "max_completion_tokens",
                                     "temperature",
                                     "top_p",
                                     "seed",
                                     "reasoning_effort",
                                     "service_tier",
                                     "stream",
                                     "stream_options",
                                     "store",
                                     "metadata",
                                     "user",
                                     "n",
                                     "stop",
                                     "presence_penalty",
                                     "frequency_penalty",
                                     "logit_bias",
                                     "logprobs",
                                     "top_logprobs",
                                     "modalities",
                                     "audio",
                                     "prediction",
                                     "web_search_options",
                                     "functions",
                                     "function_call",
                                     "safety_identifier",
                                     "prompt_cache_key",
                                     "prompt_cache_retention",
                                     "verbosity",
                                     "max_output_tokens",
                                     "thinking",
                                     "input",
                                     "instructions",
                                     "previous_response_id",
                                     "conversation",
                                     "extra_body",
                                     ""};
    for (const auto *key : reserved) {
        profile.extra_body = {{key, "private-config-marker"}};
        const auto valid = profile.validate();
        MIRA_CHECK(!valid);
        MIRA_CHECK(valid.error().safe_message.find("private-config-marker") == std::string::npos);
        MIRA_CHECK(valid.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));
        MIRA_CHECK(!mapper.encode_request(request, profile, false, artifacts));
        ModelRouter router;
        router.register_profile(std::make_shared<const ModelProfile>(profile));
        MIRA_CHECK(!router.route(RouteQuery{}));
    }
    const std::vector<JsonValue::Object> invalid = {
        {{"vendor", true}, {"vendor", false}},
        {{"vendor", JsonValue::Object{{"nested", 1}, {"nested", 2}}}},
        {{"vendor", JsonValue::Array{1, 2}}},
        {{"vendor", std::numeric_limits<double>::infinity()}},
        {{"vendor", std::numeric_limits<double>::quiet_NaN()}},
        {{"vendor", JsonValue::Object{{"nested", -std::numeric_limits<double>::infinity()}}}},
        {{"vendor", std::string(1, static_cast<char>(0xff))}},
        {{std::string(1, static_cast<char>(0xff)), false}},
    };
    for (const auto &extra : invalid) {
        profile.extra_body = extra;
        MIRA_CHECK(!profile.validate());
        MIRA_CHECK(!mapper.encode_request(request, profile, false, artifacts));
    }
    // Direct callers cannot use a Chat profile with a different mapper and
    // silently lose its declared endpoint behavior.
    profile.extra_body = extensions();
    MIRA_CHECK(!ResponsesV1Mapper{}.encode_request(request, profile, false, artifacts));
    MIRA_CHECK(!AnthropicMessagesV1Mapper{}.encode_request(request, profile, false, artifacts));
    // Finite JSON numbers outside int64 range must serialize without an
    // out-of-range integer conversion, both on the wire and in the digest.
    profile.extra_body = {{"vendor_number", std::numeric_limits<double>::max()}};
    MIRA_CHECK(profile.validate());
    const auto extreme = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(extreme);
    MIRA_CHECK(parse_json(to_json_string(extreme.value())));
    MIRA_CHECK(profile.profile_digest() == canonical_json_digest(profile.manifest_to_json()));
    JsonValue nested = false;
    for (std::size_t depth = 0; depth < JsonLimits{}.max_depth; ++depth) {
        nested = JsonValue::Object{{"nested", std::move(nested)}};
    }
    profile.extra_body = {{"vendor", std::move(nested)}};
    MIRA_CHECK(!profile.validate());
    profile.extra_body = {{"vendor", std::string(JsonLimits{}.max_string_bytes + 1, 'a')}};
    MIRA_CHECK(!profile.validate());
    profile.extra_body = {{"vendor", "configuration"}};
    profile.capabilities.limits.max_request_bytes = 4;
    MIRA_CHECK(!profile.validate());

    for (const auto dialect :
         {ProtocolDialect::OpenAIResponsesV1, ProtocolDialect::AnthropicMessagesV1}) {
        auto other = make_profile(dialect, "https://api.test");
        other.extra_body = extensions();
        MIRA_CHECK(!other.validate());
        MIRA_CHECK(!ResponsesV1Mapper{}.encode_request(request, other, false, artifacts));
        MIRA_CHECK(!AnthropicMessagesV1Mapper{}.encode_request(request, other, false, artifacts));
    }
    return 0;
}

class CountingFiles final : public IRemoteFileStore {
  public:
    Result<RemoteFileRef> upload(const ArtifactRef &, const std::string &, const ModelDataPolicy &,
                                 const OperationContext &) override {
        ++uploads;
        return make_model_error(ModelDomainCode::InvalidModelRequest, "unexpected upload");
    }
    Result<void> retire(const RemoteFileRef &, std::chrono::seconds) override { return {}; }
    std::vector<RemoteFileAudit> audit() const override { return {}; }
    int uploads = 0;
};

int provider_transmits_or_rejects_before_io() {
    auto profile = std::make_shared<ModelProfile>(
        make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test"));
    profile->extra_body = extensions();
    profile->capabilities.limits.max_request_bytes = 1024;
    const auto request = request_for(*profile);
    auto transport = std::make_shared<MockHttpTransport>(nullptr);
    auto artifacts = std::make_shared<NullArtifactSource>();
    transport->enqueue_json(200, R"({"id":"fixture","model":"test-model","choices":[
        {"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]})");
    OpenAiCompatibleProvider provider(profile, transport, artifacts);
    MIRA_CHECK(provider.infer(request, OperationContext{}, ProviderInferOptions{}));
    const auto sent = transport->recorded();
    MIRA_CHECK(sent.size() == 1);
    MIRA_CHECK(sent[0].url == "https://api.test/v1/chat/completions");
    const auto body = parse_json(sent[0].body);
    MIRA_CHECK(body);
    MIRA_CHECK(body.value().find("chat_template_kwargs")->find("enable_thinking")->as_boolean() ==
               false);

    MIRA_CHECK(provider.last_trace().write_started);
    auto oversized = request;
    oversized.input[0].content = {TextPart{std::string(2048, 'a')}};
    MIRA_CHECK(!provider.infer(oversized, OperationContext{}, ProviderInferOptions{}));
    MIRA_CHECK(transport->recorded().size() == 1);
    MIRA_CHECK(!provider.last_trace().write_started);
    MIRA_CHECK(classify_provider_stage(
                   provider.last_trace(),
                   make_model_error(ModelDomainCode::InvalidModelRequest, "too large")) ==
               RequestStage::PreWriteFailure);

    auto rejected_transport = std::make_shared<MockHttpTransport>(nullptr);
    auto invalid = std::make_shared<ModelProfile>(*profile);
    invalid->extra_body = {{"stream", true}};
    OpenAiCompatibleProvider invalid_provider(invalid, rejected_transport, artifacts);
    MIRA_CHECK(!invalid_provider.infer(request, OperationContext{}, ProviderInferOptions{}));
    MIRA_CHECK(rejected_transport->recorded().empty());

    // The extension itself fits, but the final request plus messages does not.
    auto small = std::make_shared<ModelProfile>(*profile);
    small->capabilities.limits.max_request_bytes =
        to_json_string(JsonValue(small->extra_body)).size();
    MIRA_CHECK(small->validate());
    OpenAiCompatibleProvider small_provider(small, rejected_transport, artifacts);
    const auto too_large =
        small_provider.infer(request, OperationContext{}, ProviderInferOptions{});
    MIRA_CHECK(!too_large);
    MIRA_CHECK(too_large.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::InvalidModelRequest));
    MIRA_CHECK(rejected_transport->recorded().empty());

    auto responses = std::make_shared<ModelProfile>(*profile);
    responses->dialect = ProtocolDialect::OpenAIResponsesV1;
    auto files = std::make_shared<CountingFiles>();
    auto upload_request = request;
    upload_request.data_policy.allow_uploads = true;
    upload_request.input[0].content.emplace_back(ImagePart{});
    OpenAiCompatibleProvider upload_provider(responses, rejected_transport, artifacts, nullptr,
                                             files);
    MIRA_CHECK(!upload_provider.infer(upload_request, OperationContext{}, ProviderInferOptions{}));
    MIRA_CHECK(files->uploads == 0);
    MIRA_CHECK(rejected_transport->recorded().empty());
    return 0;
}
} // namespace

int main() {
    if (const int status = manifest_routing_and_wire(); status != 0)
        return status;
    if (const int status = invalid_configuration_fails_closed(); status != 0)
        return status;
    return provider_transmits_or_rejects_before_io();
}
