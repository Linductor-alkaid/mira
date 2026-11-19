#include <mira/model_context_admission.hpp>
#include <mira/model_digest.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>

namespace mira {
namespace {
ContextItemId item_id(const std::string &seed) {
    const auto digest = digest_string(seed);
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return ContextItemId{Id128{bytes}};
}
Error invalid(const char *message) {
    return make_context_error(ContextDomainCode::InvalidItem, message);
}
} // namespace

Result<AdmittedModelRequest> admit_model_request(const ModelRequest &request,
                                                 const ModelProfile &profile, SessionId session,
                                                 const ModelContextSnapshot &snapshot,
                                                 const ModelContextAdmissionConfig &config) try {
    if (config.max_candidate_items == 0 || config.max_candidate_items > 256 ||
        config.max_candidate_bytes == 0 || snapshot.candidates.size() > config.max_candidate_items)
        return invalid("invalid context candidate bounds");
    auto counter = config.counter;
    if (!counter) {
        ConservativeTokenConfig conservative;
        conservative.bytes_per_token = 1;
        counter = std::make_shared<ConservativeTokenCounter>(conservative);
    }
    ContextRequest build;
    build.task_id = request.task_id;
    // Stateless gateway consumers may not carry a Session; this identity is
    // accounting-only and is never persisted as a real runtime Session.
    build.session_id = session.is_nil() ? SessionId{request.task_id.value} : session;
    build.task_epoch = request.task_epoch;
    build.environment_epoch = snapshot.environment_epoch;
    build.through_event_sequence = snapshot.through_event_sequence;
    build.profile_id = profile.id;
    build.limits.context_window_tokens = profile.capabilities.limits.max_context_tokens;
    build.limits.reserved_output_tokens = request.generation.max_output_tokens.value_or(
        profile.capabilities.limits.max_output_tokens);
    if (build.limits.reserved_output_tokens > profile.capabilities.limits.max_output_tokens)
        return invalid("requested output exceeds profile limit");
    build.limits.safety_margin_tokens = config.safety_margin_tokens;
    build.limits.provider_overhead_tokens = counter->estimate_assembly_overhead(0, 0);
    build.limits.max_image_tokens = build.limits.context_window_tokens;
    build.limits.max_tool_schema_tokens = build.limits.context_window_tokens;
    build.tools = request.tools;
    if (auto valid = validate_model_tool_limit(request, profile); !valid)
        return valid.error();
    std::size_t images = 0;
    for (const auto &item : request.input)
        for (const auto &part : item.content)
            if (const auto *image = std::get_if<ImagePart>(&part))
                if (++images > profile.capabilities.limits.max_images_per_request ||
                    image->source.byte_size > profile.capabilities.limits.max_image_bytes)
                    return invalid("complete request exceeds profile image capacity");
    const auto seed = model_request_canonical_digest(request).to_string();
    for (std::size_t i = 0; i < request.input.size(); ++i) {
        ContextItem item;
        item.id = item_id(seed + "/input/" + std::to_string(i));
        item.kind = request.input[i].role == ModelRole::System ||
                            request.input[i].role == ModelRole::Developer
                        ? ContextItemKind::SystemPolicy
                        : ContextItemKind::HistoricalPayload;
        item.authority = item.kind == ContextItemKind::SystemPolicy
                             ? ContextAuthority::SystemPolicy
                             : ContextAuthority::UntrustedExternalData;
        item.content = request.input[i].content;
        item.pinned = true;
        item.sequence = i;
        build.items.push_back(std::move(item));
    }
    // These bytes affect inference but are outside input messages. This
    // accounting-only item is never emitted as a user/system message.
    ContextItem framing;
    framing.id = item_id(seed + "/framing");
    framing.kind = ContextItemKind::TaskLimits;
    framing.authority = ContextAuthority::VerifiedState;
    framing.pinned = true;
    framing.content.emplace_back(TextPart{to_json_string(request.output_contract.schema.root) +
                                          to_json_string(JsonValue{profile.extra_body}) +
                                          std::string(64, ' ')});
    build.items.push_back(std::move(framing));
    const auto original_count = build.items.size();
    std::set<ContextItemId> ids;
    for (const auto &item : build.items)
        ids.insert(item.id);
    std::size_t bytes = 0;
    for (const auto &candidate : snapshot.candidates) {
        const bool memory = candidate.kind == ContextItemKind::RetrievedMemory &&
                            candidate.authority == ContextAuthority::RetrievedMemory;
        const bool summary = candidate.kind == ContextItemKind::CheckpointSummary &&
                             candidate.authority == ContextAuthority::UntrustedExternalData;
        if ((!memory && !summary) || candidate.pinned || candidate.replaceable_by_reference ||
            candidate.payload || !ids.insert(candidate.id).second || !candidate.validate())
            return invalid("context supplier returned an elevated or invalid candidate");
        for (const auto &part : candidate.content) {
            const auto *text = std::get_if<TextPart>(&part);
            if (!text || text->text.size() > config.max_candidate_bytes - bytes)
                return invalid("context candidates exceed text capacity");
            bytes += text->text.size();
        }
        build.items.push_back(candidate);
    }
    const StandardContextManager manager{counter};
    auto selected = manager.prepare(build);
    if (!selected)
        return selected.error();
    std::vector<std::pair<std::size_t, ContextItemAudit>> omitted_history;
    if (snapshot.prepend_history) {
        for (std::size_t i = original_count; i < build.items.size(); ++i)
            if (build.items[i].wire_role)
                omitted_history.emplace_back(i, selected.value().item_audit[i]);
        const auto dropped =
            std::count_if(omitted_history.begin(), omitted_history.end(), [](const auto &entry) {
                return entry.second.disposition == ContextItemDisposition::Dropped;
            });
        if (dropped != 0 && dropped != std::ssize(omitted_history)) {
            // A partial native transcript can replay an answer without its
            // question. Give the freed budget to the other candidates.
            std::erase_if(build.items, [](const auto &item) { return item.wire_role.has_value(); });
            selected = manager.prepare(build);
            if (!selected)
                return selected.error();
        } else {
            omitted_history.clear();
        }
    }
    if (selected.value().tools.size() != request.tools.size())
        return make_context_error(ContextDomainCode::MinimumSetTooLarge,
                                  "selected tool schemas exceed the complete request budget");

    AdmittedModelRequest admitted{request, std::move(selected).value()};
    admitted.request.profile_id = profile.id;
    // Match Layer 0 assembly order to its per-item audit. Reuse only optional
    // data from that assembly; original protocol envelopes remain untouched.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < build.items.size(); ++i)
        if (admitted.context.item_audit[i].disposition != ContextItemDisposition::Dropped)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return std::tuple{partition_of(build.items[a].kind), build.items[a].sequence, a} <
               std::tuple{partition_of(build.items[b].kind), build.items[b].sequence, b};
    });
    std::vector<ModelInputItem> prefix;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] < original_count)
            continue;
        auto &destination = snapshot.prepend_history && build.items[order[i]].wire_role
                                ? prefix
                                : admitted.request.input;
        destination.push_back(admitted.context.input[i]);
    }
    if (!prefix.empty()) {
        // Work with the validated original envelope, never with the compacted
        // Layer 0 copy. Insertion cannot split an existing tool round trip.
        const auto current =
            std::find_if(request.input.begin(), request.input.end(), [](const auto &item) {
                return item.role != ModelRole::System && item.role != ModelRole::Developer;
            });
        if (current == request.input.end() || current->role != ModelRole::User ||
            std::any_of(current->content.begin(), current->content.end(), [](const auto &part) {
                return std::holds_alternative<ToolResultPart>(part);
            }))
            return invalid("recalled history requires an original leading user message");
        admitted.request.input.insert(
            admitted.request.input.begin() + std::distance(request.input.begin(), current),
            std::make_move_iterator(prefix.begin()), std::make_move_iterator(prefix.end()));
    }
    if (admitted.request.input.size() > profile.capabilities.limits.max_input_items)
        return invalid("complete request exceeds profile input item capacity");
    if (auto valid = validate_model_request(admitted.request); !valid)
        return valid.error();
    for (auto &[index, audit] : omitted_history) {
        audit.disposition = ContextItemDisposition::Dropped;
        audit.reason = "native_history_did_not_fit";
        const auto offset = static_cast<std::ptrdiff_t>(index);
        admitted.context.item_audit.insert(admitted.context.item_audit.begin() + offset,
                                           std::move(audit));
        ++admitted.context.budget.dropped_items;
    }
    admitted.context.selection_digest =
        digest_string(admitted.context.selection_digest.to_string() +
                      model_request_canonical_digest(admitted.request).to_string());
    admitted.request.prompt_provenance.context_selection_digest = admitted.context.selection_digest;
    admitted.context.input = admitted.request.input;
    return admitted;
} catch (...) {
    return make_context_error(ContextDomainCode::TokenCountUnavailable,
                              "complete request admission raised an exception");
}
} // namespace mira
