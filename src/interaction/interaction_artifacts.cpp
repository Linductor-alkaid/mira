#include "interaction_coordinator_support.hpp"

#include <algorithm>

namespace mira {
using interaction_support::recovery_error;
namespace {
const std::string *text_field(const JsonValue &json, const char *name) {
    const auto *value = json.find(name);
    return value ? value->as_string() : nullptr;
}
bool bound_to(const JsonValue &json, const LedgerEntry &entry, std::string_view schema) {
    const auto *version = text_field(json, "schema");
    const auto *tenant = text_field(json, "tenant");
    const auto *user = text_field(json, "user");
    return version && *version == schema && tenant && *tenant == entry.tenant_id.to_string() &&
           user && *user == entry.user_id.to_string();
}
} // namespace

void InteractionCoordinator::set_recovery_stores(std::shared_ptr<IArtifactStore> artifacts,
                                                 std::shared_ptr<ICheckpointStore> checkpoints) {
    impl_->artifacts = std::move(artifacts);
    impl_->checkpoints = std::move(checkpoints);
}

Result<ArtifactDescriptor> InteractionCoordinator::Impl::store_json(const JsonValue &json) {
    if (!artifacts)
        return recovery_error(ErrorCode::Unavailable,
                              "recovery artifact storage is not configured");
    const auto bytes = to_json_string(json);
    if (bytes.size() > config.max_recovery_artifact_bytes)
        return recovery_error(ErrorCode::ResourceExhausted,
                              "interaction artifact exceeds its byte bound");
    ArtifactWriteSpec spec;
    spec.media_type = "application/json";
    spec.encoding = ArtifactEncoding::Utf8;
    spec.sensitivity = Sensitivity::Sensitive;
    spec.content_schema = {1, 0};
    spec.max_bytes = config.max_recovery_artifact_bytes;
    auto writer = artifacts->begin(spec);
    if (!writer)
        return writer.error();
    if (auto wrote = writer.value().write(bytes.data(), bytes.size()); !wrote)
        return wrote.error();
    return artifacts->commit(writer.value());
}

Result<JsonValue> InteractionCoordinator::Impl::read_json(const ArtifactDescriptor &ref) const {
    if (!artifacts || ref.byte_size > config.max_recovery_artifact_bytes)
        return recovery_error(ErrorCode::Unavailable,
                              "recovery artifact is unavailable or exceeds its bound");
    auto reader = artifacts->open(ref);
    if (!reader)
        return reader.error();
    if (reader.value().size() != ref.byte_size ||
        digest_bytes(reader.value().bytes()) != ref.digest)
        return recovery_error(ErrorCode::DataLoss, "recovery artifact size mismatch");
    const auto &bytes = reader.value().bytes();
    const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const auto json = parse_json(text);
    if (!json || !json.value().is_object())
        return recovery_error(ErrorCode::DataLoss, "recovery artifact is invalid JSON");
    return json.value();
}

Result<LedgerEntry> InteractionCoordinator::Impl::admit_input(const InputEnvelope &envelope) {
    if (!artifacts || !interaction_support::permitted(envelope.principal, config))
        return ledger->admit(envelope);
    if (auto writable = ledger->check_writable(); !writable)
        return writable.error();
    const auto previous = ledger->lookup(envelope.input_id);
    if (previous)
        return ledger->admit(envelope); // No orphan artifact on a duplicate.
    if (previous.error().code != ErrorCode::NotFound)
        return previous.error();
    if (envelope.content.size() > config.max_recovery_artifact_bytes ||
        (envelope.reply_to && envelope.reply_to->size() > 256) ||
        (envelope.clarification_for && envelope.clarification_for->size() > 256) ||
        envelope.input_id.size() > 256 || envelope.attachment_refs.size() > 64)
        return recovery_error(ErrorCode::ResourceExhausted,
                              "input exceeds recovery storage bounds");
    JsonValue::Array attachments;
    for (const auto &ref : envelope.attachment_refs) {
        if (ref.size() > 4096)
            return recovery_error(ErrorCode::ResourceExhausted,
                                  "attachment reference exceeds its bound");
        attachments.emplace_back(ref);
    }
    JsonValue::Object json{{"schema", "mira.interaction.input.v1"},
                           {"tenant", envelope.principal.tenant_id.to_string()},
                           {"user", envelope.principal.user_id.to_string()},
                           {"input_id", envelope.input_id},
                           {"content", envelope.content},
                           {"attachments", std::move(attachments)}};
    if (envelope.reply_to)
        json.emplace_back("reply_to", *envelope.reply_to);
    if (envelope.target_task)
        json.emplace_back("target_task", envelope.target_task->to_string());
    if (envelope.requested_intent)
        json.emplace_back("intent", std::string(route_intent_name(*envelope.requested_intent)));
    if (envelope.clarification_for)
        json.emplace_back("clarification_for", *envelope.clarification_for);
    const auto stored = store_json(json);
    if (!stored)
        return stored.error();
    auto admitted = ledger->admit_recoverable(envelope, stored.value());
    if (!admitted) {
        // An append failure can be a committed acceptance with a lost ack.
        // Keep its body available for recovery; only erase on known pre-write
        // rejection. The write fence stops further speculative artifacts.
        const auto code = admitted.error().domain_code;
        const bool rejected_before_write =
            admitted.error().domain == kInteractionErrorDomain &&
            (code == static_cast<std::int32_t>(InteractionDomainCode::LedgerFull) ||
             code == static_cast<std::int32_t>(InteractionDomainCode::InputInvalid) ||
             code == static_cast<std::int32_t>(InteractionDomainCode::InputConflict) ||
             code == static_cast<std::int32_t>(InteractionDomainCode::PermissionDenied));
        if (rejected_before_write) {
            const auto erased = artifacts->erase({stored.value().id, "input admission rejected"});
            if (!erased || !erased.value().payload_erased)
                return recovery_error(ErrorCode::DataLoss,
                                      "input rejection and artifact cleanup failed");
        }
    }
    return admitted;
}

Result<InputEnvelope> InteractionCoordinator::Impl::load_input(const LedgerEntry &entry) const {
    if (!entry.input_ref) {
        const auto found = clarification_inputs.find(entry.input_id);
        if (found != clarification_inputs.end())
            return found->second;
        return recovery_error(ErrorCode::NotFound, "input body is missing or was erased");
    }
    const auto json = read_json(*entry.input_ref);
    if (!json)
        return json.error();
    const auto *id = text_field(json.value(), "input_id");
    const auto *content = text_field(json.value(), "content");
    const auto *attachments = json.value().find("attachments");
    if (!bound_to(json.value(), entry, "mira.interaction.input.v1") || !id ||
        *id != entry.input_id || !content || !attachments || !attachments->is_array() ||
        attachments->as_array()->size() > 64)
        return recovery_error(ErrorCode::DataLoss, "input artifact binding or schema is invalid");
    InputEnvelope result;
    result.principal.tenant_id = entry.tenant_id;
    result.principal.user_id = entry.user_id;
    result.input_id = entry.input_id;
    result.content = *content;
    for (const auto &ref : *attachments->as_array()) {
        if (!ref.is_string())
            return recovery_error(ErrorCode::DataLoss, "invalid stored attachment reference");
        result.attachment_refs.push_back(*ref.as_string());
    }
    if (const auto *reply = text_field(json.value(), "reply_to"))
        result.reply_to = *reply;
    if (const auto *target = text_field(json.value(), "target_task")) {
        result.target_task = TaskId::parse(*target);
        if (!result.target_task)
            return recovery_error(ErrorCode::DataLoss, "invalid stored task reference");
    }
    if (const auto *intent = text_field(json.value(), "intent")) {
        result.requested_intent = interaction_support::parse_intent(*intent);
        if (!result.requested_intent)
            return recovery_error(ErrorCode::DataLoss, "invalid stored intent");
    }
    if (const auto *original = text_field(json.value(), "clarification_for"))
        result.clarification_for = *original;
    if (result.clarification_for != entry.clarification_for)
        return recovery_error(ErrorCode::DataLoss,
                              "stored clarification metadata does not match its body");
    return result;
}

Result<ArtifactDescriptor>
InteractionCoordinator::Impl::store_context(const PrincipalTaskEntry &task,
                                            const PrincipalContext &principal) {
    JsonValue::Array constraints;
    for (const auto &constraint : task.constraints)
        constraints.emplace_back(constraint);
    JsonValue::Array sources;
    if (task.source_artifacts.empty() || task.source_artifacts.size() > config.max_inputs_per_task)
        return recovery_error(ErrorCode::ResourceExhausted, "work provenance exceeds its bound");
    for (const auto &ref : task.source_artifacts)
        sources.emplace_back(interaction_support::artifact_json(ref));
    JsonValue::Object json{{"schema", "mira.interaction.work.v1"},
                           {"tenant", principal.tenant_id.to_string()},
                           {"user", principal.user_id.to_string()},
                           {"task", task.task_id.to_string()},
                           {"session", task.session_id.to_string()},
                           {"goal", task.goal},
                           {"constraints", std::move(constraints)},
                           {"state", std::string(task_state_name(task.state))},
                           {"sources", std::move(sources)}};
    if (task.resumed_from)
        json.emplace_back("resumed_from", task.resumed_from->to_string());
    if (task.recovered_checkpoint)
        json.emplace_back("checkpoint", checkpoint_to_json(*task.recovered_checkpoint));
    return store_json(json);
}

Result<PrincipalTaskEntry>
InteractionCoordinator::Impl::load_context(const LedgerEntry &entry) const {
    if (!entry.context_ref || !entry.task_id || !entry.session_id)
        return recovery_error(ErrorCode::NotFound, "work context is missing or was erased");
    const auto json = read_json(*entry.context_ref);
    if (!json)
        return json.error();
    const auto *task = text_field(json.value(), "task");
    const auto *session = text_field(json.value(), "session");
    const auto *goal = text_field(json.value(), "goal");
    const auto *state = text_field(json.value(), "state");
    const auto *constraints = json.value().find("constraints");
    if (!bound_to(json.value(), entry, "mira.interaction.work.v1") || !task ||
        *task != entry.task_id->to_string() || !session ||
        *session != entry.session_id->to_string() || !goal || !state ||
        !task_state_from_name(*state) || !constraints || !constraints->is_array() ||
        constraints->as_array()->size() > interaction_support::kMaxConstraintsPerTask)
        return recovery_error(ErrorCode::DataLoss, "work context binding or schema is invalid");
    PrincipalTaskEntry result;
    result.task_id = *entry.task_id;
    result.session_id = *entry.session_id;
    result.goal = *goal;
    result.state = *task_state_from_name(*state);
    result.historical = true;
    for (const auto &value : *constraints->as_array()) {
        if (!value.is_string())
            return recovery_error(ErrorCode::DataLoss, "invalid stored constraint");
        result.constraints.push_back(*value.as_string());
    }
    if (const auto *previous = text_field(json.value(), "resumed_from")) {
        result.resumed_from = TaskId::parse(*previous);
        if (!result.resumed_from)
            return recovery_error(ErrorCode::DataLoss, "invalid resumed work reference");
    }
    if (const auto *checkpoint = json.value().find("checkpoint")) {
        const auto parsed = checkpoint_from_json(*checkpoint);
        if (!parsed || !result.resumed_from || parsed.value().task_id != *result.resumed_from)
            return recovery_error(ErrorCode::DataLoss, "invalid recovered checkpoint binding");
        result.recovered_checkpoint = parsed.value();
    }
    const auto *sources = json.value().find("sources");
    if (!sources || !sources->is_array() || sources->as_array()->empty() ||
        sources->as_array()->size() > config.max_inputs_per_task)
        return recovery_error(ErrorCode::DataLoss,
                              "work source provenance is missing or exceeds its bound");
    for (const auto &value : *sources->as_array()) {
        const auto ref = interaction_support::artifact_from_json(value);
        if (!ref)
            return ref.error();
        if (ref.value().byte_size > config.max_recovery_artifact_bytes)
            return recovery_error(ErrorCode::ResourceExhausted,
                                  "work source artifact exceeds its bound");
        const auto available = artifacts->open(ref.value());
        if (!available)
            return available.error();
        if (available.value().size() != ref.value().byte_size ||
            digest_bytes(available.value().bytes()) != ref.value().digest)
            return recovery_error(ErrorCode::DataLoss, "work source artifact integrity mismatch");
        result.source_artifacts.push_back(ref.value());
    }
    result.context_block = assemble_context(result);
    return result;
}

Result<SessionSequence> InteractionCoordinator::Impl::append_task_event(TaskId task,
                                                                        SessionId session,
                                                                        std::string type,
                                                                        JsonValue payload) {
    AppendRequest request;
    request.event_id = EventId::generate();
    request.runtime_id = runtime_id;
    request.session_id = session;
    request.task_id = task;
    request.payload = {std::move(type), to_json_string(payload), EventClass::State};
    const auto receipt = events->append(request);
    if (!receipt || receipt.value().achieved < Durability::ProcessCrash)
        return recovery_error(ErrorCode::DataLoss,
                              "recovery metadata did not receive a durable acknowledgement");
    return receipt.value().session_sequence;
}
} // namespace mira
