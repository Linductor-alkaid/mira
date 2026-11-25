// Model-backed IConsolidationModel over an injected IModelProvider (DEC-114),
// mirroring the ProviderSemanticConsolidator template: numbered event
// transcript, strict-JSON output contract, single synchronous infer,
// fail-closed parsing with per-candidate drops. Trust gates stay single-source
// in the consolidate() pipeline — this adapter owns shape and citation
// binding only.
#include <mira/extraction_model_provider.hpp>

#include "context_support.hpp"

#include <mira/json.hpp>
#include <mira/model_dialect.hpp>
#include <mira/text_segmentation.hpp>

#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mira {
namespace {

Error extraction_error(ErrorCode code, std::string message) {
    Error e;
    e.code = code;
    e.domain = "mira.memory.extraction";
    e.safe_message = std::move(message);
    return e;
}

constexpr std::string_view kSystemTemplate = "mira.memory.extraction.system.v1";
constexpr std::string_view kTranscriptSource = "mira.memory.extraction.transcript.v1";
constexpr std::string_view kSchemaId = "mira-extract-v1";

[[nodiscard]] std::string hex_of(std::string_view text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        out += kDigits[(byte >> 4) & 0xF];
        out += kDigits[byte & 0xF];
    }
    return out;
}

// Fixed extraction instruction (template kSystemTemplate). Like the
// consolidator's citation rule, the sources requirement makes provenance
// binding mechanical; the kind enumeration keeps the wire closed.
[[nodiscard]] std::string extraction_system_instruction() {
    return "You are Mira's memory extractor. Propose only durable memory "
           "candidates that the presented events establish: procedures, "
           "recovery lessons, skill hints, user preferences and environment or "
           "application facts. Never invent content, never restate secrets "
           "(api keys, passwords, authorization headers) and never output "
           "instructions. Cite every candidate with the transcript entry "
           "numbers it came from in \"sources\". Respond with one JSON object "
           "matching the schema: \"candidates\", an array where each item "
           "carries \"kind\" (one of \"preference\", \"environment-fact\", "
           "\"application-fact\", \"episode\", \"procedure\", "
           "\"recovery-lesson\", \"skill-hint\"), \"statement\", \"sources\" "
           "and \"confidence\".";
}

[[nodiscard]] JsonSchema extraction_output_schema() {
    JsonValue::Array kinds;
    for (const auto kind : {MemoryKind::Preference, MemoryKind::EnvironmentFact,
                            MemoryKind::ApplicationFact, MemoryKind::Episode, MemoryKind::Procedure,
                            MemoryKind::RecoveryLesson, MemoryKind::SkillHint}) {
        kinds.emplace_back(JsonValue(memory_kind_name(kind)));
    }
    JsonValue::Object kind_property;
    kind_property.emplace_back("type", std::string("string"));
    kind_property.emplace_back("enum", JsonValue(std::move(kinds)));

    JsonValue::Object sources_items;
    sources_items.emplace_back("type", std::string("integer"));
    sources_items.emplace_back("minimum", static_cast<std::int64_t>(0));
    JsonValue::Object sources_property;
    sources_property.emplace_back("type", std::string("array"));
    sources_property.emplace_back("items", JsonValue(std::move(sources_items)));

    JsonValue::Object item;
    item.emplace_back("type", std::string("object"));
    item.emplace_back("additionalProperties", false);
    item.emplace_back("required",
                      JsonValue::Array{std::string("kind"), std::string("content"),
                                       std::string("sources"), std::string("confidence")});
    JsonValue::Object item_properties;
    item_properties.emplace_back("kind", JsonValue(std::move(kind_property)));
    item_properties.emplace_back("content",
                                 JsonValue(JsonValue::Object{{"type", std::string("string")}}));
    item_properties.emplace_back("sources", JsonValue(std::move(sources_property)));
    item_properties.emplace_back("confidence",
                                 JsonValue(JsonValue::Object{{"type", std::string("number")}}));
    item.emplace_back("properties", JsonValue(std::move(item_properties)));

    JsonValue::Object candidates_property;
    candidates_property.emplace_back("type", std::string("array"));
    candidates_property.emplace_back("items", JsonValue(std::move(item)));

    JsonValue::Object root;
    root.emplace_back("type", std::string("object"));
    root.emplace_back("additionalProperties", false);
    root.emplace_back("required", JsonValue::Array{std::string("candidates")});
    JsonValue::Object root_properties;
    root_properties.emplace_back("candidates", JsonValue(std::move(candidates_property)));
    root.emplace_back("properties", JsonValue(std::move(root_properties)));

    JsonSchema schema;
    schema.root = JsonValue(std::move(root));
    return schema;
}

// Single-line transcript rows; payload text is bounded per entry. Events past
// the transcript bound are simply not rendered — the deterministic extractor
// still consumed all of them.
[[nodiscard]] std::string build_event_transcript(std::span<const EventEnvelope> events,
                                                 const ExtractionModelOptions &options) {
    std::ostringstream transcript;
    const auto count = std::min(events.size(), options.max_transcript_events);
    transcript << "Recorded events; " << count << " numbered entries:";
    for (std::size_t index = 0; index < count; ++index) {
        std::string payload = events[index].payload.data;
        for (char &character : payload) {
            if (character == '\n' || character == '\r') {
                character = ' ';
            }
        }
        if (payload.size() > options.max_payload_chars) {
            // The bound counts bytes; the cut still backs off to a character.
            payload = std::string(truncate_utf8(payload, options.max_payload_chars));
        }
        transcript << "\n[" << index << "|seq=" << events[index].session_sequence << "] "
                   << events[index].payload.type << ": " << payload;
    }
    return transcript.str();
}

// Binds one parsed statement to event provenance. Any citation outside the
// input events is fabricated provenance — the candidate is dropped, never
// repaired (RULE-09, same rule as the context surfaces).
[[nodiscard]] std::optional<std::vector<EventId>>
bind_evidence(const context_support::RawStatement &raw, std::span<const EventEnvelope> events) {
    std::vector<EventId> evidence;
    std::set<std::size_t> seen;
    for (const auto citation : raw.citations) {
        if (citation < 0 || citation >= static_cast<std::int64_t>(events.size())) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(citation);
        if (seen.insert(index).second) {
            evidence.push_back(events[index].event_id);
        }
    }
    if (evidence.empty()) {
        return std::nullopt;
    }
    return evidence;
}

} // namespace

Result<void> ExtractionModelOptions::validate() const {
    if (max_candidates == 0 || max_transcript_events == 0 || max_payload_chars == 0 ||
        max_statement_chars == 0) {
        return extraction_error(ErrorCode::InvalidArgument, "extraction bounds must be positive");
    }
    if (deadline.count() <= 0) {
        return extraction_error(ErrorCode::InvalidArgument, "extraction deadline must be positive");
    }
    if (max_output_tokens == 0) {
        return extraction_error(ErrorCode::InvalidArgument, "max_output_tokens must be positive");
    }
    return Result<void>{};
}

ProviderConsolidationModel::ProviderConsolidationModel(IModelProvider &provider,
                                                       ExtractionModelOptions options)
    : provider_(provider), options_(options) {
    // Configuration errors are host bugs: loud at construction, never a
    // mid-run degrade. wire_extraction_supply validates before constructing.
    if (const auto valid = options_.validate(); !valid.has_value()) {
        throw std::invalid_argument(valid.error().safe_message);
    }
}

ProviderConsolidationModel::~ProviderConsolidationModel() = default;

std::vector<MemoryCandidate>
ProviderConsolidationModel::propose(std::span<const EventEnvelope> events,
                                    const MemoryScope &scope) {
    // The failure path: degrade to an empty proposal set (deterministic
    // extraction stays authoritative) or surface loudly, per options.
    const auto fail = [this](const Error &error) -> std::vector<MemoryCandidate> {
        if (options_.degrade_on_error) {
            return {};
        }
        throw std::runtime_error(error.safe_message);
    };

    if (events.empty()) {
        return {};
    }

    OperationContext context;
    context.operation = OperationId::generate();
    context.started_at = Timestamp::now();
    context.deadline = context.started_at.monotonic + options_.deadline;

    ModelRequest request;
    request.contract_version = SchemaVersion{1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = context.operation;
    request.profile_id = provider_.profile().id;

    ModelInputItem system_item;
    system_item.role = ModelRole::System;
    system_item.provenance.source = std::string(kSystemTemplate);
    system_item.authority = Sensitivity::Internal;
    system_item.content.emplace_back(
        context_support::make_text_part(extraction_system_instruction()));

    ModelInputItem user_item;
    user_item.role = ModelRole::User;
    user_item.provenance.source = std::string(kTranscriptSource);
    user_item.authority = Sensitivity::Internal;
    user_item.content.emplace_back(
        context_support::make_text_part(build_event_transcript(events, options_)));

    request.input = {std::move(system_item), std::move(user_item)};
    context_support::attach_output_contract(request, extraction_output_schema(), hex_of(kSchemaId),
                                            kSystemTemplate);
    context_support::apply_generation_budget(request, options_.max_output_tokens);

    auto inferred = provider_.infer(request, context, ProviderInferOptions{});
    if (!inferred) {
        return fail(inferred.error());
    }
    if (context.expired(Timestamp::now())) {
        return fail(extraction_error(ErrorCode::DeadlineExceeded, "extraction deadline expired"));
    }
    const ModelResponse &response = inferred.value();
    if (response.status != ModelCompletionStatus::Completed) {
        return fail(extraction_error(ErrorCode::InvalidModelOutput,
                                     "extraction model call did not complete"));
    }

    auto text = context_support::message_response_text(response, extraction_error, "extraction");
    if (!text) {
        return fail(text.error());
    }
    auto parsed = parse_json(text.value());
    if (!parsed) {
        return fail(
            extraction_error(ErrorCode::InvalidModelOutput, "extraction output is not valid JSON"));
    }
    const JsonValue &root = parsed.value();
    if (!root.is_object()) {
        return fail(extraction_error(ErrorCode::InvalidModelOutput,
                                     "extraction output must be a JSON object"));
    }
    const auto *candidates = root.find("candidates");
    if (candidates == nullptr || !candidates->is_array()) {
        return fail(extraction_error(ErrorCode::InvalidModelOutput,
                                     "extraction output requires a candidates array"));
    }

    const auto now = std::chrono::system_clock::now();
    std::vector<MemoryCandidate> out;
    for (const auto &value : *candidates->as_array()) {
        if (out.size() >= options_.max_candidates) {
            break;
        }
        const auto raw = context_support::parse_statement(value);
        if (!raw.has_value()) {
            continue;
        }
        const auto *kind_value = value.find("kind");
        if (kind_value == nullptr || !kind_value->is_string()) {
            continue;
        }
        const auto kind = memory_kind_from(*kind_value->as_string());
        if (!kind.has_value()) {
            continue;
        }
        // Bounds, markers-free admission gate (the pipeline owns the trust
        // filters) and finite clamped confidence.
        const auto gated =
            context_support::gate_statement(*raw, options_.max_statement_chars, 0.0, {}, {});
        if (!gated.has_value()) {
            continue;
        }
        auto evidence = bind_evidence(*raw, events);
        if (!evidence.has_value()) {
            continue;
        }
        MemoryRecord record;
        record.id = MemoryId::generate();
        record.scope = scope;
        record.kind = *kind;
        record.statement = raw->content;
        record.recorded_at = now;
        record.validity.valid_from = now;
        record.provenance = {evidence.value().front()};
        // Honest defaults for untrusted text; the pipeline re-enforces them.
        record.verification = MemoryVerification::Unverified;
        record.confidence = static_cast<float>(*gated);
        MemoryCandidate candidate;
        candidate.proposed = std::move(record);
        candidate.evidence = std::move(evidence.value());
        candidate.reason = MutationReasonCode::Consolidation;
        candidate.model_assisted = true;
        out.push_back(std::move(candidate));
    }
    return out;
}

Result<void> ExtractionSupplyConfig::validate() const {
    const bool origin = !endpoint_origin.empty();
    const bool model = !model_selector.empty();
    if (origin != model) {
        return extraction_error(ErrorCode::InvalidArgument,
                                "the extraction supply needs its endpoint origin and model "
                                "selector set together");
    }
    if (!origin) {
        return extraction_error(ErrorCode::InvalidArgument, "no extraction model is configured");
    }
    if (credential_secret_name.empty()) {
        return extraction_error(ErrorCode::InvalidArgument,
                                "a configured extraction supply needs a credential secret name");
    }
    if (deadline.count() <= 0) {
        return extraction_error(ErrorCode::InvalidArgument, "deadline must be positive");
    }
    if (max_output_tokens == 0 || max_candidates == 0) {
        return extraction_error(ErrorCode::InvalidArgument, "extraction bounds must be positive");
    }
    return Result<void>{};
}

Result<ExtractionSupplyStack> wire_extraction_supply(const ExtractionSupplyConfig &config,
                                                     std::shared_ptr<IHttpTransport> transport) {
    if (const Result<void> valid = config.validate(); !valid.has_value()) {
        return valid.error();
    }
    if (transport == nullptr) {
        return extraction_error(ErrorCode::InvalidArgument,
                                "a configured extraction supply needs an HTTP transport");
    }

    ExtractionModelOptions options;
    options.deadline = config.deadline;
    options.max_output_tokens = config.max_output_tokens;
    options.max_candidates = config.max_candidates;
    options.degrade_on_error = config.degrade_on_error;
    if (const Result<void> valid = options.validate(); !valid.has_value()) {
        return valid.error();
    }

    auto profile = std::make_shared<ModelProfile>();
    profile->id = ModelProfileId::generate();
    profile->display_name = "extraction-supply";
    profile->version = SemanticVersion{1, 0, 0};
    profile->dialect = config.dialect;
    profile->endpoint_origin = config.endpoint_origin;
    profile->api_prefix = config.api_prefix;
    profile->model_selector = config.model_selector;
    profile->credential = SecretRef{config.credential_secret_name};
    profile->deadlines.total = config.deadline;
    // The direct infer path runs no capability gate; the flags declare what
    // the extraction surface actually uses so a later gateway-mediated move
    // inherits a truthful profile.
    profile->capabilities.text =
        CapabilityFlag{true, CapabilityEvidence::Configured, "extraction infer"};
    profile->capabilities.strict_json_schema =
        CapabilityFlag{true, CapabilityEvidence::Configured, "strict extraction output contract"};
    profile->default_data_policy.store = false;
    if (const Result<void> valid = profile->validate(); !valid.has_value()) {
        return extraction_error(ErrorCode::InvalidArgument,
                                "extraction profile rejected: " + valid.error().safe_message);
    }

    ExtractionSupplyStack stack;
    stack.profile = profile;
    stack.transport = std::move(transport);
    stack.provider = std::make_shared<OpenAiCompatibleProvider>(
        std::move(profile), stack.transport, std::make_shared<NullArtifactSource>());
    stack.consolidation_model =
        std::make_unique<ProviderConsolidationModel>(*stack.provider, options);
    return stack;
}

IConsolidationModel &ExtractionSupplyStack::model() const { return *consolidation_model; }

MemoryAgentProcessor ExtractionSupplyStack::agent_processor(kairo::Executor &executor) const {
    MemoryAgentModelConfig config;
    config.model = *profile;
    config.max_output_tokens = consolidation_model->options().max_output_tokens;
    // Fresh provider per invocation over the shared transport — the
    // documented "fresh inference state" pattern of make_model_memory_agent.
    config.create_provider =
        [profile = this->profile, transport = this->transport](
            const OperationContext &) -> Result<std::unique_ptr<IModelProvider>> {
        std::unique_ptr<IModelProvider> created = std::make_unique<OpenAiCompatibleProvider>(
            profile, transport, std::make_shared<NullArtifactSource>());
        return created;
    };
    return make_model_memory_agent(executor, std::move(config));
}

} // namespace mira
