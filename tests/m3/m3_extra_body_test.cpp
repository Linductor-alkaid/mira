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

int decoding_defaults_merge_render_and_manifest() {
    // DEC-105: the profile decoding defaults fill the sampling parameters the
    // canonical request leaves unset; explicit request values always win;
    // unsupported policies and request-owned fields are never filled.
    auto profile = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    auto request = request_for(profile);
    NullArtifactSource artifacts;
    ChatCompletionsV1Mapper mapper;

    // Empty decoding: wire, manifest and digest are exactly the legacy ones.
    const auto empty_wire = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(empty_wire);
    MIRA_CHECK(empty_wire.value().find("temperature") == nullptr);
    MIRA_CHECK(empty_wire.value().find("top_p") == nullptr);
    MIRA_CHECK(empty_wire.value().find("seed") == nullptr);
    const auto empty_manifest = profile.manifest_to_json();
    MIRA_CHECK(empty_manifest.find("decoding") == nullptr);
    const auto empty_digest = profile.profile_digest();
    MIRA_CHECK(empty_digest == canonical_json_digest(empty_manifest));

    profile.decoding.temperature = 0.7;
    profile.decoding.top_p = 0.9;
    profile.decoding.seed = 7;
    // The fixture profile policy ships seed=Unsupported; the defaults only
    // validate and render once the profile declares seed support.
    profile.capabilities.generation.seed = ParamMapping::Native;
    MIRA_CHECK(profile.validate());
    // The same profile with decoding cleared: the baseline the manifest,
    // digest and wire regress to when the section is emptied again.
    auto without_decoding = profile;
    without_decoding.decoding = ProfileDecodingOptions{};
    MIRA_CHECK(without_decoding.validate());
    MIRA_CHECK(without_decoding.manifest_to_json().find("decoding") == nullptr);
    const auto without_decoding_wire =
        mapper.encode_request(request, without_decoding, false, artifacts);
    MIRA_CHECK(without_decoding_wire);
    MIRA_CHECK(without_decoding_wire.value().find("temperature") == nullptr);
    MIRA_CHECK(without_decoding_wire.value().find("top_p") == nullptr);
    MIRA_CHECK(without_decoding_wire.value().find("seed") == nullptr);
    ModelGenerationOptions generation;
    generation.max_output_tokens = 128;
    generation.temperature = 0.2;
    const auto merged = effective_generation_options(profile, generation);
    MIRA_CHECK(merged.temperature.has_value() && *merged.temperature == 0.2);
    MIRA_CHECK(merged.top_p.has_value() && *merged.top_p == 0.9);
    MIRA_CHECK(merged.seed.has_value() && *merged.seed == 7);
    MIRA_CHECK(merged.max_output_tokens.has_value() && *merged.max_output_tokens == 128);
    MIRA_CHECK(!merged.reasoning_effort.has_value());
    MIRA_CHECK(!merged.thinking.has_value());
    MIRA_CHECK(!merged.service_tier.has_value());
    const auto bare = effective_generation_options(profile, ModelGenerationOptions{});
    MIRA_CHECK(bare.temperature.has_value() && *bare.temperature == 0.7);
    MIRA_CHECK(bare.top_p.has_value() && *bare.top_p == 0.9);
    MIRA_CHECK(bare.seed.has_value() && *bare.seed == 7);
    // Defense in depth: a field the profile policy declares Unsupported is
    // never filled, even for a profile that skipped validate().
    auto unsupported_seed = profile;
    unsupported_seed.capabilities.generation.seed = ParamMapping::Unsupported;
    const auto defensive = effective_generation_options(unsupported_seed, ModelGenerationOptions{});
    MIRA_CHECK(!defensive.seed.has_value());
    MIRA_CHECK(defensive.temperature.has_value() && *defensive.temperature == 0.7);
    auto unsupported_temperature = profile;
    unsupported_temperature.capabilities.generation.temperature = ParamMapping::Unsupported;
    MIRA_CHECK(!effective_generation_options(unsupported_temperature, ModelGenerationOptions{})
                    .temperature.has_value());
    auto unsupported_top_p = profile;
    unsupported_top_p.capabilities.generation.top_p = ParamMapping::Unsupported;
    MIRA_CHECK(!effective_generation_options(unsupported_top_p, ModelGenerationOptions{})
                    .top_p.has_value());

    // Chat wire: unset request fields take the defaults; explicit request
    // values win for every merged parameter.
    auto wire = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(wire);
    MIRA_CHECK(*wire.value().find("temperature")->as_number() == 0.7);
    MIRA_CHECK(*wire.value().find("top_p")->as_number() == 0.9);
    MIRA_CHECK(*wire.value().find("seed")->as_integer() == 7);
    auto explicit_request = request;
    explicit_request.generation.temperature = 0.2;
    explicit_request.generation.top_p = 0.25;
    auto explicit_wire = mapper.encode_request(explicit_request, profile, false, artifacts);
    MIRA_CHECK(explicit_wire);
    MIRA_CHECK(*explicit_wire.value().find("temperature")->as_number() == 0.2);
    MIRA_CHECK(*explicit_wire.value().find("top_p")->as_number() == 0.25);
    auto seeded_request = request;
    seeded_request.generation.seed = 99;
    auto explicit_seed_wire = mapper.encode_request(seeded_request, profile, false, artifacts);
    MIRA_CHECK(explicit_seed_wire);
    MIRA_CHECK(*explicit_seed_wire.value().find("seed")->as_integer() == 99);
    // While the profile policy cannot represent the seed, it is never filled
    // and never rendered.
    auto seedless_wire = mapper.encode_request(request, unsupported_seed, false, artifacts);
    MIRA_CHECK(seedless_wire);
    MIRA_CHECK(seedless_wire.value().find("seed") == nullptr);

    // Manifest: the decoding section appears exactly when any field is set,
    // and the digest follows it.
    const auto manifest = profile.manifest_to_json();
    const auto *decoding = manifest.find("decoding");
    MIRA_CHECK(decoding != nullptr && decoding->is_object());
    MIRA_CHECK(*decoding->find("temperature")->as_number() == 0.7);
    MIRA_CHECK(*decoding->find("top_p")->as_number() == 0.9);
    MIRA_CHECK(*decoding->find("seed")->as_integer() == 7);
    const auto digest = profile.profile_digest();
    MIRA_CHECK(digest != empty_digest);
    MIRA_CHECK(digest == canonical_json_digest(manifest));
    profile.decoding = ProfileDecodingOptions{};
    MIRA_CHECK(profile.manifest_to_json() == without_decoding.manifest_to_json());
    MIRA_CHECK(profile.profile_digest() == without_decoding.profile_digest());
    const auto restored = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(restored && restored.value() == without_decoding_wire.value());

    // decoding and extra_body coexist without key collisions.
    profile.decoding.temperature = 0.3;
    profile.extra_body = {{"vendor_sampling", JsonValue::Object{{"weight", 0.5}}}};
    MIRA_CHECK(profile.validate());
    const auto both = mapper.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(both);
    MIRA_CHECK(*both.value().find("temperature")->as_number() == 0.3);
    MIRA_CHECK(both.value().find("vendor_sampling") != nullptr);
    // The sampling keys stay reserved in extra_body; decoding is their only
    // host-side path (DEC-090 names are unchanged).
    for (const auto *key : {"temperature", "top_p", "seed"}) {
        auto reserved = profile;
        reserved.extra_body = {{key, "private-config-marker"}};
        const auto valid = reserved.validate();
        MIRA_CHECK(!valid);
        MIRA_CHECK(valid.error().safe_message.find("private-config-marker") == std::string::npos);
    }
    profile.extra_body.clear();

    // The Responses dialect applies the same temperature/top_p defaults; it
    // never renders a seed.
    auto responses = make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.test");
    responses.decoding.temperature = 0.4;
    responses.decoding.top_p = 0.5;
    MIRA_CHECK(responses.validate());
    auto responses_request = request_for(responses);
    auto responses_wire =
        ResponsesV1Mapper{}.encode_request(responses_request, responses, false, artifacts);
    MIRA_CHECK(responses_wire);
    MIRA_CHECK(*responses_wire.value().find("temperature")->as_number() == 0.4);
    MIRA_CHECK(*responses_wire.value().find("top_p")->as_number() == 0.5);
    responses_request.generation.temperature = 0.6;
    auto explicit_responses =
        ResponsesV1Mapper{}.encode_request(responses_request, responses, false, artifacts);
    MIRA_CHECK(explicit_responses);
    MIRA_CHECK(*explicit_responses.value().find("temperature")->as_number() == 0.6);
    responses.decoding.seed = 5;
    responses.capabilities.generation.seed = ParamMapping::Native;
    MIRA_CHECK(responses.validate());
    auto seeded_responses =
        ResponsesV1Mapper{}.encode_request(request_for(responses), responses, false, artifacts);
    MIRA_CHECK(seeded_responses);
    MIRA_CHECK(seeded_responses.value().find("seed") == nullptr);
    return 0;
}

int decoding_validation_fails_closed() {
    // Registration-time closure: out-of-range values and parameters the
    // profile's own generation policy declares Unsupported never validate,
    // and the safe message never echoes the configured values.
    const auto check_rejected = [](const ModelProfile &profile) -> int {
        const auto valid = profile.validate();
        MIRA_CHECK(!valid);
        MIRA_CHECK(valid.error().code == ErrorCode::UnsupportedCapability);
        MIRA_CHECK(valid.error().domain == "mira.model");
        MIRA_CHECK(valid.error().domain_code ==
                   static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));
        return 0;
    };
    auto chat = make_profile(ProtocolDialect::OpenAIChatCompletionsV1, "https://api.test");
    chat.decoding.temperature = -0.1;
    MIRA_CHECK(check_rejected(chat) == 0);
    chat.decoding.temperature = std::numeric_limits<double>::quiet_NaN();
    MIRA_CHECK(check_rejected(chat) == 0);
    chat.decoding.temperature = std::numeric_limits<double>::infinity();
    MIRA_CHECK(check_rejected(chat) == 0);
    chat.decoding.temperature = 2.01;
    const auto rejected = chat.validate();
    MIRA_CHECK(!rejected);
    MIRA_CHECK(rejected.error().safe_message.find("2.01") == std::string::npos);
    chat.decoding.temperature = 2.0;
    MIRA_CHECK(chat.validate());
    chat.decoding.temperature = 0.0;
    MIRA_CHECK(chat.validate());

    chat.decoding.temperature.reset();
    chat.decoding.top_p = 0.0;
    MIRA_CHECK(!chat.validate());
    chat.decoding.top_p = -0.5;
    MIRA_CHECK(!chat.validate());
    chat.decoding.top_p = 1.5;
    MIRA_CHECK(!chat.validate());
    chat.decoding.top_p = std::numeric_limits<double>::infinity();
    MIRA_CHECK(!chat.validate());
    chat.decoding.top_p = 1.0;
    MIRA_CHECK(chat.validate());
    chat.decoding.top_p = 0.25;
    MIRA_CHECK(chat.validate());

    chat.decoding.top_p.reset();
    chat.decoding.seed = 7;
    MIRA_CHECK(!chat.validate());
    ModelRouter router;
    router.register_profile(std::make_shared<const ModelProfile>(chat));
    MIRA_CHECK(!router.route(RouteQuery{}));
    chat.capabilities.generation.seed = ParamMapping::Native;
    MIRA_CHECK(chat.validate());
    chat.decoding.seed.reset();
    chat.capabilities.generation.temperature = ParamMapping::Unsupported;
    chat.decoding.temperature = 0.5;
    MIRA_CHECK(!chat.validate());
    chat.decoding.temperature.reset();
    chat.capabilities.generation.temperature = ParamMapping::Native;
    chat.capabilities.generation.top_p = ParamMapping::Unsupported;
    chat.decoding.top_p = 0.5;
    MIRA_CHECK(!chat.validate());

    // The Messages dialect documents a tighter temperature ceiling than the
    // OpenAI dialects, and rejects a seed default like any Unsupported field.
    auto messages = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://api.test");
    messages.decoding.temperature = 1.01;
    MIRA_CHECK(!messages.validate());
    messages.decoding.temperature = 1.0;
    MIRA_CHECK(messages.validate());
    messages.decoding.temperature.reset();
    messages.decoding.seed = 3;
    MIRA_CHECK(!messages.validate());
    return 0;
}

int anthropic_applies_chat_defaults_and_rejects_seed() {
    // DEC-105: Messages temperature/top_p defaults flow through the shared
    // Chat translation; the dialect's missing seed field fails closed on a
    // decoding default even when the profile policy claims support.
    auto profile = make_profile(ProtocolDialect::AnthropicMessagesV1, "https://api.test");
    auto request = request_for(profile);
    NullArtifactSource artifacts;
    profile.decoding.temperature = 0.6;
    profile.decoding.top_p = 0.8;
    MIRA_CHECK(profile.validate());
    auto wire = AnthropicMessagesV1Mapper{}.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(wire);
    MIRA_CHECK(*wire.value().find("temperature")->as_number() == 0.6);
    MIRA_CHECK(*wire.value().find("top_p")->as_number() == 0.8);
    MIRA_CHECK(wire.value().find("seed") == nullptr);
    request.generation.temperature = 0.3;
    auto explicit_wire =
        AnthropicMessagesV1Mapper{}.encode_request(request, profile, false, artifacts);
    MIRA_CHECK(explicit_wire);
    MIRA_CHECK(*explicit_wire.value().find("temperature")->as_number() == 0.3);
    MIRA_CHECK(*explicit_wire.value().find("top_p")->as_number() == 0.8);
    profile.capabilities.generation.seed = ParamMapping::Native;
    profile.decoding.seed = 11;
    MIRA_CHECK(profile.validate());
    auto rejected =
        AnthropicMessagesV1Mapper{}.encode_request(request_for(profile), profile, false, artifacts);
    MIRA_CHECK(!rejected);
    MIRA_CHECK(rejected.error().domain_code ==
               static_cast<std::int32_t>(ModelDomainCode::CapabilityMismatch));
    MIRA_CHECK(rejected.error().safe_message ==
               "Messages field or content has no verified canonical mapping");
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
    if (const int status = decoding_defaults_merge_render_and_manifest(); status != 0)
        return status;
    if (const int status = decoding_validation_fails_closed(); status != 0)
        return status;
    if (const int status = anthropic_applies_chat_defaults_and_rejects_seed(); status != 0)
        return status;
    return provider_transmits_or_rejects_before_io();
}
