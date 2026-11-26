#include <mira/agent_persona_suggestion.hpp>

#include <mira/content_markers.hpp>
#include <mira/json.hpp>

#include <algorithm>

namespace mira {
namespace {

Error suggestion_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.persona.suggestion";
    error.safe_message = message;
    return error;
}

Result<void> check_operation(const OperationContext &operation) {
    if (operation.cancelled())
        return suggestion_error(ErrorCode::Cancelled, "persona suggestion cancelled");
    if (operation.expired(Timestamp::now()))
        return suggestion_error(ErrorCode::DeadlineExceeded, "persona suggestion expired");
    return {};
}

bool binding_current(const AgentPersonaBinding &binding) {
    try {
        return binding.snapshot.current() && binding.current && binding.current() &&
               binding.snapshot.current();
    } catch (...) {
        return false;
    }
}

Result<std::string> read_user_message(const IEventStore &events,
                                      const AgentPersonaSuggestionSource &source,
                                      const OperationContext &operation) {
    EventSliceQuery query;
    query.session_id = operation.session;
    query.after_sequence = source.sequence - 1;
    query.through_sequence = source.sequence;
    query.limit = 1;
    query.scan_limit = 1;
    const auto page = events.read_slice(query);
    if (!page)
        return page.error();
    const auto &window = page.value();
    if (window.events.size() != 1 || window.has_more ||
        window.snapshot_through != source.sequence || window.scanned_through != source.sequence)
        return suggestion_error(ErrorCode::DataLoss, "persona suggestion evidence is unavailable");
    const auto &event = window.events.front();
    if (event.payload.type != "UserMessageInjected" ||
        event.payload.classification != EventClass::State || event.event_id != source.event ||
        event.session_sequence != source.sequence || event.runtime_id != source.runtime ||
        event.session_id != operation.session || event.task_id != operation.task)
        return suggestion_error(ErrorCode::InvalidArgument,
                                "persona suggestion evidence disagrees");
    const auto body = parse_json(event.payload.data);
    if (!body)
        return suggestion_error(ErrorCode::DataLoss, "persona suggestion evidence cannot be read");
    const auto *task = body.value().find("task_id");
    const auto *epoch = body.value().find("task_epoch");
    const auto *detail = body.value().find("detail");
    if (!task || !task->is_string() || TaskId::parse(*task->as_string()) != operation.task ||
        !epoch || !epoch->is_integer() || *epoch->as_integer() < 0 ||
        static_cast<std::uint64_t>(*epoch->as_integer()) != operation.task_epoch || !detail ||
        !detail->is_object())
        return suggestion_error(ErrorCode::InvalidArgument, "persona suggestion frame disagrees");
    const auto *text = detail->find("text");
    const auto *bytes = detail->find("bytes");
    const auto *digest = detail->find("digest");
    if (!text || !text->is_string() || !bytes || !bytes->is_integer() || *bytes->as_integer() < 0 ||
        static_cast<std::uint64_t>(*bytes->as_integer()) != text->as_string()->size() || !digest ||
        !digest->is_string() ||
        digest_from_hex(*digest->as_string()) != digest_string(*text->as_string()))
        return suggestion_error(ErrorCode::DataLoss,
                                "persona suggestion message integrity disagrees");
    return *text->as_string();
}

bool utf8_boundary(std::string_view text, std::size_t offset) {
    return offset == text.size() || (static_cast<unsigned char>(text[offset]) & 0xc0U) != 0x80U;
}

Id128 suggestion_id(const AgentPersonaSuggestion &suggestion) {
    const auto digest = digest_string(
        "mira.persona.suggestion.v1/" + suggestion.key.tenant.to_string() + "/" +
        suggestion.key.agent.to_string() + "/" + std::to_string(suggestion.base_version) + "/" +
        suggestion.base_digest.to_string() + "/" + suggestion.source.runtime.to_string() + "/" +
        suggestion.session.to_string() + "/" + suggestion.task.to_string() + "/" +
        std::to_string(suggestion.task_epoch) + "/" + suggestion.source.event.to_string() + "/" +
        std::to_string(suggestion.source.sequence) + "/" + std::to_string(suggestion.quote_offset) +
        "/" + std::to_string(suggestion.quote.size()));
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id128{bytes};
}

} // namespace

Result<AgentPersonaSuggestionReceipt> submit_agent_persona_suggestion(
    const IEventStore &events, const AgentPersonaSuggestionSource &source, std::string_view quote,
    std::optional<std::size_t> quote_offset, const OperationContext &operation,
    const AgentPersonaSupplier &bind, const AgentPersonaSuggestionSink &stage) {
    if (source.runtime.is_nil() || source.event.is_nil() || source.sequence == 0 ||
        operation.session.is_nil() || operation.task.is_nil() || operation.operation.is_nil() ||
        !bind || !stage || quote.size() > 512 ||
        quote.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos ||
        quote.find('\0') != std::string_view::npos)
        return suggestion_error(ErrorCode::InvalidArgument, "persona suggestion is incomplete");
    if (auto active = check_operation(operation); !active)
        return active.error();

    try {
        const auto selected = bind(operation);
        if (!selected)
            return selected.error();
        if (!selected.value() || !binding_current(*selected.value()))
            return suggestion_error(ErrorCode::PermissionDenied,
                                    "persona suggestion binding unavailable");
        if (auto active = check_operation(operation); !active)
            return active.error();
        const auto &binding = *selected.value();
        const auto message = read_user_message(events, source, operation);
        if (!message)
            return message.error();
        const std::string_view text = message.value();
        const auto offset = quote_offset ? *quote_offset : text.find(quote);
        if (offset > text.size() || !text.substr(offset).starts_with(quote) ||
            (!quote_offset && text.find(quote, offset + 1) != std::string_view::npos))
            return suggestion_error(ErrorCode::InvalidArgument,
                                    "persona suggestion quote is ambiguous or missing");
        if (!utf8_boundary(text, offset) || !utf8_boundary(text, offset + quote.size()))
            return suggestion_error(ErrorCode::InvalidArgument,
                                    "persona suggestion quote splits UTF-8");
        if (matches_content_marker(quote, default_forbidden_content_markers()) ||
            matches_content_marker(quote, default_injection_content_markers()))
            return suggestion_error(ErrorCode::SafetyRejected,
                                    "persona suggestion content was rejected");

        const auto &revision = binding.snapshot.revision();
        AgentPersonaSuggestion suggestion{
            {},     binding.snapshot.key(), revision.version, digest_string(revision.instructions),
            source, operation.session,      operation.task,   operation.task_epoch,
            offset, std::string(quote)};
        suggestion.id = suggestion_id(suggestion);
        if (!binding_current(binding))
            return suggestion_error(ErrorCode::PermissionDenied,
                                    "persona suggestion binding changed");
        if (auto active = check_operation(operation); !active)
            return active.error();
        auto receipt = stage(suggestion, operation);
        if (receipt && receipt.value().id != suggestion.id)
            return suggestion_error(ErrorCode::DataLoss, "persona suggestion receipt disagrees");
        return receipt;
    } catch (...) {
        return suggestion_error(ErrorCode::Internal, "persona suggestion submission failed");
    }
}

} // namespace mira
