#include "support/m3_support.hpp"
#include "support/test.hpp"

#include <mira/model_digest.hpp>

#include <string>

namespace {

using namespace mira;

[[nodiscard]] ModelRequest sample_request() {
    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = TaskId::generate();
    request.task_epoch = 3;
    request.profile_id = ModelProfileId::generate();

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    TextPart system_text;
    system_text.text = "system prompt";
    system_item.content.emplace_back(std::move(system_text));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    TextPart user_text;
    user_text.text = "hello";
    user_text.sensitivity = Sensitivity::Public;
    user_item.content.emplace_back(std::move(user_text));
    request.input = {std::move(system_item), std::move(user_item)};

    request.output_contract.mode = OutputMode::StrictJsonSchema;
    request.output_contract.schema_id = SchemaId::generate();
    request.output_contract.schema_version = SemanticVersion{1, 2, 0};
    const auto schema = parse_json(
        R"({"type":"object","properties":{"a":{"type":"integer"}},"required":["a"],"additionalProperties":false})");
    request.output_contract.schema.root = schema.value();
    request.output_contract.canonical_schema_digest =
        canonical_json_digest(request.output_contract.schema.root);

    request.budget.max_input_tokens = 1000;
    request.budget.max_output_tokens = 100;
    request.data_policy.store = false;
    request.prompt_provenance.system_template_digest = digest_string("template");
    return request;
}

[[nodiscard]] ModelResponse sample_response(const ModelRequest &request) {
    ModelResponse response;
    response.contract_version = SchemaVersion{1, 0};
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    response.requested_model = "test-model";
    response.status = ModelCompletionStatus::Completed;
    MessageOutput message;
    message.role = ModelRole::Assistant;
    OutputTextPart part;
    part.text = "{\"a\":1}";
    message.content.emplace_back(std::move(part));
    response.output.emplace_back(std::move(message));
    response.usage.input_tokens = 12;
    response.usage.output_tokens = 4;
    response.usage.quality = UsageQuality::ProviderReported;
    return response;
}

int validation_rejects_broken_requests() {
    auto request = sample_request();
    MIRA_CHECK(validate_model_request(request).has_value());

    auto no_prompt = request;
    no_prompt.input.erase(no_prompt.input.begin());
    MIRA_CHECK(!validate_model_request(no_prompt).has_value());

    auto secret_text = request;
    TextPart secret;
    secret.sensitivity = Sensitivity::Secret;
    secret.text = "token";
    secret_text.input[1].content.emplace_back(std::move(secret));
    MIRA_CHECK(!validate_model_request(secret_text).has_value());

    auto nil_ids = request;
    nil_ids.request_id = ModelRequestId{};
    MIRA_CHECK(!validate_model_request(nil_ids).has_value());

    auto bad_digest = request;
    bad_digest.output_contract.canonical_schema_digest = Sha256Digest{};
    MIRA_CHECK(!validate_model_request(bad_digest).has_value());

    auto named_choice = request;
    named_choice.tool_choice.mode = ToolChoiceMode::Named;
    named_choice.tool_choice.required_tool = ToolId::generate();
    MIRA_CHECK(!validate_model_request(named_choice).has_value());

    auto bad_temperature = request;
    bad_temperature.generation.temperature = 5.0;
    MIRA_CHECK(!validate_model_request(bad_temperature).has_value());
    return 0;
}

int response_validation_and_status_rules() {
    const auto request = sample_request();
    auto response = sample_response(request);
    MIRA_CHECK(validate_model_response(response).has_value());

    auto unknown_status = response;
    unknown_status.status = ModelCompletionStatus::Unknown;
    MIRA_CHECK(!validate_model_response(unknown_status).has_value());

    auto incomplete_without_reason = response;
    incomplete_without_reason.status = ModelCompletionStatus::Incomplete;
    MIRA_CHECK(!validate_model_response(incomplete_without_reason).has_value());

    auto incomplete_with_reason = incomplete_without_reason;
    incomplete_with_reason.incomplete_reason = IncompleteReason::MaxOutputTokens;
    MIRA_CHECK(validate_model_response(incomplete_with_reason).has_value());

    auto usage_conflict = response;
    usage_conflict.usage.quality = UsageQuality::Missing;
    MIRA_CHECK(!validate_model_response(usage_conflict).has_value());
    return 0;
}

int json_round_trip_and_unknown_enums() {
    const auto request = sample_request();
    const auto json = model_request_to_json(request);
    auto parsed = parse_json(to_json_string(json));
    MIRA_CHECK(parsed.has_value());
    auto restored = model_request_from_json(parsed.value());
    MIRA_CHECK(restored.has_value());

    // Canonical digest is order-independent and stable across round trips.
    const auto direct = model_request_canonical_digest(request);
    const auto round_trip = model_request_canonical_digest(restored.value());
    MIRA_CHECK(direct == round_trip);

    // Unknown enum values fail closed instead of coercing.
    auto replace_first_input_item = [](JsonValue root, JsonValue item) {
        JsonValue::Array items =
            root.find("input")->as_array() ? *root.find("input")->as_array() : JsonValue::Array{};
        if (!items.empty()) {
            items[0] = std::move(item);
        }
        root.set("input", JsonValue(std::move(items)));
        return root;
    };
    auto with_unknown_role = parse_json(to_json_string(json));
    MIRA_CHECK(with_unknown_role.has_value());
    with_unknown_role.value() = replace_first_input_item(
        std::move(with_unknown_role.value()),
        JsonValue::Object{{"role", "wizard"}, {"content", JsonValue::Array{}}});
    MIRA_CHECK(!model_request_from_json(with_unknown_role.value()).has_value());

    auto with_unknown_part = parse_json(to_json_string(json));
    with_unknown_part.value() = replace_first_input_item(
        std::move(with_unknown_part.value()),
        JsonValue::Object{{"role", "user"},
                          {"content", JsonValue::Array{JsonValue::Object{{"kind", "hologram"}}}}});
    MIRA_CHECK(!model_request_from_json(with_unknown_part.value()).has_value());

    // Response side: unknown output item type fails closed.
    const auto response = sample_response(request);
    auto response_json = parse_json(to_json_string(model_response_to_json(response)));
    MIRA_CHECK(response_json.has_value());
    JsonValue::Array output_items = *response_json.value().find("output")->as_array();
    output_items[0] = JsonValue::Object{{"type", "brainwave"}, {"data", 1}};
    response_json.value().set("output", JsonValue(std::move(output_items)));
    MIRA_CHECK(!model_response_from_json(response_json.value()).has_value());

    auto restored_response = model_response_from_json(
        parse_json(to_json_string(model_response_to_json(response))).value());
    MIRA_CHECK(restored_response.has_value());
    MIRA_CHECK(restored_response.value().status == ModelCompletionStatus::Completed);
    MIRA_CHECK(restored_response.value().usage.input_tokens == 12);
    return 0;
}

int error_domain_mapping() {
    const auto auth = make_model_error(ModelDomainCode::AuthenticationFailed, "x");
    MIRA_CHECK(auth.domain == "mira.model");
    MIRA_CHECK(auth.code == ErrorCode::PermissionDenied);
    MIRA_CHECK(model_domain_code_name(ModelDomainCode::AuthenticationFailed) ==
               "AuthenticationFailed");

    const auto ambiguous = make_model_error(ModelDomainCode::AmbiguousCompletion, "x");
    MIRA_CHECK(ambiguous.code == ErrorCode::ExecutionUncertain);

    const auto filtered = make_model_error(ModelDomainCode::ContentFiltered, "x");
    MIRA_CHECK(filtered.code == ErrorCode::SafetyRejected);
    return 0;
}

int profile_manifest_and_routing() {
    ModelRouter router;
    auto profile = std::make_shared<ModelProfile>(
        testing::make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.example.test"));
    MIRA_CHECK(profile->validate().has_value());
    MIRA_CHECK(profile->request_path() == "/v1/responses");
    MIRA_CHECK(profile->endpoint_url() == "https://api.example.test/v1/responses");
    const auto digest = profile->profile_digest();
    router.register_profile(profile);

    RouteQuery query;
    query.needs_strict_schema = true;
    auto decision = router.route(query);
    MIRA_CHECK(decision.has_value());
    MIRA_CHECK(decision.value().selected());
    MIRA_CHECK(decision.value().profile_digest == digest);

    RouteQuery image_query;
    image_query.needs_image_input = true;
    image_query.min_evidence = CapabilityEvidence::InteropVerified;
    auto rejected = router.route(image_query);
    MIRA_CHECK(!rejected.has_value());

    // Sensitive content refuses profiles that keep remote storage on.
    ModelRouter storing_router;
    auto storing_profile = std::make_shared<ModelProfile>(
        testing::make_profile(ProtocolDialect::OpenAIResponsesV1, "https://api.example.test"));
    storing_profile->id = ModelProfileId::generate();
    storing_profile->default_data_policy.store = true;
    storing_profile->default_data_policy.remote_retention = std::chrono::seconds{3600};
    storing_router.register_profile(storing_profile);
    RouteQuery sensitive_query;
    sensitive_query.max_sensitivity = Sensitivity::Sensitive;
    MIRA_CHECK(!storing_router.route(sensitive_query).has_value());
    // The store=false profile accepts the same query.
    MIRA_CHECK(router.route(sensitive_query).has_value());

    RouteQuery secret_query;
    secret_query.max_sensitivity = Sensitivity::Secret;
    MIRA_CHECK(!router.route(secret_query).has_value());

    // Manifest digest covers identity; a changed model changes it.
    auto mutated = *profile;
    mutated.model_selector = "other-model";
    MIRA_CHECK(mutated.profile_digest() != digest);

    // Proxy policy and credential references are profile-bound, while secret
    // plaintext remains outside the manifest.
    mutated = *profile;
    mutated.proxy = ModelProxyConfig{"http://proxy.example.test:8080",
                                     SecretRef{"proxy-credential"},
                                     false,
                                     {"proxy.example.test"}};
    MIRA_CHECK(mutated.validate().has_value());
    MIRA_CHECK(mutated.profile_digest() != digest);
    const auto proxy_manifest = to_json_string(mutated.manifest_to_json());
    MIRA_CHECK(proxy_manifest.find("proxy-credential") != std::string::npos);
    MIRA_CHECK(proxy_manifest.find("Proxy-Authorization") == std::string::npos);
    mutated.proxy->url = "http://user:pass@proxy.example.test";
    MIRA_CHECK(!mutated.validate().has_value());

    // Generation gate rejects unsupported parameters.
    ModelGenerationOptions generation;
    generation.seed = 7;
    MIRA_CHECK(
        !unsupported_generation_parameters(profile->capabilities.generation, generation).empty());
    generation.seed.reset();
    MIRA_CHECK(
        unsupported_generation_parameters(profile->capabilities.generation, generation).empty());

    // Broken manifests never route.
    auto broken = std::make_shared<ModelProfile>(
        testing::make_profile(ProtocolDialect::OpenAIResponsesV1, "ftp://bad"));
    MIRA_CHECK(!broken->validate().has_value());
    return 0;
}

int wire_digest_excludes_secrets() {
    auto wire = parse_json(R"({"model":"m","input":[{"role":"user"}],"store":false})");
    MIRA_CHECK(wire.has_value());
    std::vector<std::pair<std::string, std::string>> headers = {
        {"content-type", "application/json"},
        {"Authorization", "Bearer sk-secret-value"},
        {"x-api-key", "sk-other-secret"},
    };
    const auto digest = wire_request_digest(wire.value(), headers);
    // Changing secrets must not change the digest.
    headers[1].second = "Bearer sk-different";
    headers[2].second = "changed";
    MIRA_CHECK(wire_request_digest(wire.value(), headers) == digest);
    // Changing content does.
    wire.value().set("model", "m2");
    MIRA_CHECK(wire_request_digest(wire.value(), headers) != digest);

    // Redaction for events keeps secrets out.
    const auto sanitized = sanitize_headers_for_events(
        {{"Authorization", "Bearer sk-secret"}, {"Accept", "application/json"}});
    MIRA_CHECK(sanitized[0].second == "[redacted]");
    MIRA_CHECK(sanitized[1].second == "application/json");

    MIRA_CHECK(redact_url_for_log("https://h.example/p?sig=abc&x=1") == "https://h.example/p");
    const std::vector<std::string> forbidden = {"sk-secret-value"};
    MIRA_CHECK(contains_none(to_json_string(sanitize_wire_for_events(wire.value())), forbidden));

    // Prompt digest covers artifact digests, not payload bytes.
    const auto request = sample_request();
    const auto first = prompt_digest(request.input);
    const auto second = prompt_digest(request.input);
    MIRA_CHECK(first == second);
    return 0;
}

} // namespace

int main() {
    if (const int status = validation_rejects_broken_requests(); status != 0) {
        return status;
    }
    if (const int status = response_validation_and_status_rules(); status != 0) {
        return status;
    }
    if (const int status = json_round_trip_and_unknown_enums(); status != 0) {
        return status;
    }
    if (const int status = error_domain_mapping(); status != 0) {
        return status;
    }
    if (const int status = profile_manifest_and_routing(); status != 0) {
        return status;
    }
    if (const int status = wire_digest_excludes_secrets(); status != 0) {
        return status;
    }
    return 0;
}
