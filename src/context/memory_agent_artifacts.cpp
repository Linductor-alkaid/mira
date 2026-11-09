#include "memory_agent_support.hpp"

#include <algorithm>

namespace mira {
using memory_agent_support::error;
namespace {
Result<void> erase_artifact(IArtifactStore &store, const ArtifactDescriptor &artifact,
                            const char *reason) {
    const auto erased = store.erase({artifact.id, reason});
    if (erased && erased.value().payload_erased)
        return {};
    // A recovered store may omit an already-tombstoned object from its live
    // index. Confirm it is unavailable, rather than retrying forever or
    // treating an arbitrary storage failure as successful erasure.
    if (!erased && erased.error().code == ErrorCode::NotFound) {
        const auto remaining = store.open(artifact);
        if (!remaining && remaining.error().code == ErrorCode::NotFound)
            return {};
    }
    return error(ErrorCode::DataLoss, "memory projection erasure is pending");
}
} // namespace

Result<ArtifactDescriptor>
MemoryAgentService::State::store_projection(const Job &job, const MemoryAgentInput &input,
                                            const MemoryAgentProposal &proposal) {
    if (!config.artifacts)
        return error(ErrorCode::InvalidState, "memory compression requires a host artifact store");
    if (projections.size() >= config.max_projection_artifacts)
        return error(ErrorCode::ResourceExhausted, "memory projection artifact capacity exceeded");
    JsonValue::Array notes;
    for (const auto &note : proposal.summary) {
        JsonValue::Array citations;
        for (const auto citation : note.citations)
            citations.emplace_back(static_cast<std::int64_t>(citation));
        notes.emplace_back(
            JsonValue::Object{{"text", note.text}, {"citations", std::move(citations)}});
    }
    const auto content = to_json_string(
        JsonValue::Object{{"schema", "mira.memory-agent.projection.v1"},
                          {"source", memory_agent_support::history_json(input.history)},
                          {"summary", std::move(notes)}});
    ArtifactWriteSpec spec;
    spec.media_type = "application/json";
    spec.encoding = ArtifactEncoding::Utf8;
    spec.sensitivity = Sensitivity::Sensitive;
    spec.content_schema = {1, 0};
    spec.max_bytes = config.max_source_bytes + config.max_result_bytes * 8 + 4096;
    auto writer = config.artifacts->begin(spec);
    if (!writer)
        return writer.error();
    if (auto wrote = writer.value().write(content.data(), content.size()); !wrote)
        return wrote.error();
    auto stored = config.artifacts->commit(writer.value());
    if (!stored)
        return stored.error();
    Projection projection;
    projection.artifact = stored.value();
    JsonValue::Array scopes;
    for (const auto &scope : job.binding.scopes) {
        projection.scopes.push_back(memory_agent_support::scope_key(scope));
        scopes.emplace_back(projection.scopes.back());
    }
    const auto &artifact = projection.artifact;
    projections.emplace(artifact.id, projection);
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime;
    append.session_id = memory_agent_support::control_partition();
    append.payload = {"mira.memory-agent.projection-stored.v1",
                      to_json_string(JsonValue::Object{{"artifact_id", artifact.id.to_string()},
                                                       {"digest", artifact.digest.to_string()},
                                                       {"bytes", artifact.byte_size},
                                                       {"scope_digests", std::move(scopes)}}),
                      EventClass::State};
    append.required = Durability::ProcessCrash;
    auto registered = events->append(append);
    if (!registered || registered.value().achieved < Durability::ProcessCrash) {
        const auto erased =
            config.artifacts->erase({artifact.id, "projection-registration-failed"});
        if (erased && erased.value().payload_erased)
            projections.erase(artifact.id);
        return error(ErrorCode::DataLoss,
                     "memory projection registration failed; cleanup may be pending");
    }
    return artifact;
}

Result<void> MemoryAgentService::State::restore_projection(const JsonValue &payload) {
    const auto *id = payload.find("artifact_id");
    const auto *digest = payload.find("digest");
    const auto *bytes = payload.find("bytes");
    const auto *scopes = payload.find("scope_digests");
    const auto parsed_id =
        id && id->is_string() ? ArtifactId::parse(*id->as_string()) : std::nullopt;
    const auto parsed_digest =
        digest && digest->is_string() ? digest_from_hex(*digest->as_string()) : std::nullopt;
    if (!parsed_id || !parsed_digest || !bytes || !bytes->is_integer() ||
        *bytes->as_integer() < 0 || !scopes || !scopes->is_array() || scopes->as_array()->empty() ||
        scopes->as_array()->size() > 16)
        return error(ErrorCode::DataLoss, "memory projection reference is malformed");
    Projection projection;
    projection.artifact = {*parsed_id,
                           *parsed_digest,
                           static_cast<std::uint64_t>(*bytes->as_integer()),
                           "application/json",
                           ArtifactEncoding::Utf8,
                           Sensitivity::Sensitive,
                           {1, 0}};
    for (const auto &scope : *scopes->as_array()) {
        if (!scope.is_string() || !digest_from_hex(*scope.as_string()))
            return error(ErrorCode::DataLoss, "memory projection scope is malformed");
        projection.scopes.push_back(*scope.as_string());
    }
    if (std::any_of(projection.scopes.begin(), projection.scopes.end(),
                    [&](const auto &scope) { return blocked_scopes.contains(scope); })) {
        if (!config.artifacts)
            return error(ErrorCode::InvalidState,
                         "projection recovery requires its artifact store");
        return erase_artifact(*config.artifacts, projection.artifact,
                              "memory-scope-erasure-recovery");
    }
    if (projections.size() >= config.max_projection_artifacts && !projections.contains(*parsed_id))
        return error(ErrorCode::ResourceExhausted, "memory projection recovery exceeds capacity");
    projections[*parsed_id] = std::move(projection);
    return {};
}

Result<void> MemoryAgentService::State::erase_projections(const std::string &scope) {
    for (auto entry = projections.begin(); entry != projections.end();) {
        if (std::find(entry->second.scopes.begin(), entry->second.scopes.end(), scope) ==
            entry->second.scopes.end()) {
            ++entry;
            continue;
        }
        if (!config.artifacts)
            return error(ErrorCode::InvalidState,
                         "projection erasure requires its host artifact store");
        if (auto erased =
                erase_artifact(*config.artifacts, entry->second.artifact, "memory-scope-erasure");
            !erased)
            return erased;
        entry = projections.erase(entry);
    }
    return {};
}
} // namespace mira
