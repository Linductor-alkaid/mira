#include "memory_agent_support.hpp"

#include <algorithm>
#include <charconv>

namespace mira {
namespace {
constexpr std::string_view kRecordPage = "mira.memory-agent.record-page.v1";

Id128 digest_id(std::string_view text) {
    const auto digest = digest_string(text);
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id128{bytes};
}

SessionId receipt_partition(const MemoryWorkBinding &binding, const MemoryScope &scope) {
    return SessionId{digest_id(
        "mira.memory-agent.record-partition.v1:" + binding.principal.tenant_id.to_string() + ':' +
        binding.principal.user_id.to_string() + ':' + binding.work.session_id.to_string() + ':' +
        binding.work.task_id.to_string() + ':' + memory_agent_support::scope_key(scope))};
}

EventId receipt_id(SessionId partition, SessionSequence through) {
    return EventId{digest_id(std::string(kRecordPage) + ':' + partition.to_string() + ':' +
                             std::to_string(through))};
}

std::optional<SessionSequence> read_sequence(const JsonValue &value, std::string_view name) {
    const auto *field = value.find(name);
    if (!field || !field->is_string())
        return {};
    const auto &text = *field->as_string();
    SessionSequence sequence = 0;
    const auto *end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return {};
    return sequence;
}

Error invalid_receipt() {
    return memory_agent_support::error(ErrorCode::DataLoss,
                                       "memory record page receipt is invalid");
}
} // namespace

Result<void> MemoryAgentService::State::restore_record_cursor(Job &job) {
    EventSliceQuery query;
    query.session_id = receipt_partition(job.binding, *job.request.write_scope);
    query.limit = 1;
    query.scan_limit = 1;
    query.byte_limit = 512;
    const auto first = events->read_slice(query);
    if (!first)
        return first.error();
    const auto head = first.value().snapshot_through;
    if (head == 0) {
        if (job.after_sequence != 0 || !first.value().events.empty() || first.value().has_more ||
            first.value().scanned_through != 0)
            return invalid_receipt();
        return events->flush(Durability::ProcessCrash);
    }
    query.after_sequence = head - 1;
    query.through_sequence = head;
    const auto latest = events->read_slice(query);
    if (!latest)
        return latest.error();
    const auto &page = latest.value();
    if (page.snapshot_through != head || page.scanned_through != head || page.has_more ||
        page.events.size() != 1)
        return invalid_receipt();
    const auto &event = page.events.front();
    if (event.session_id != query.session_id || event.session_sequence != head || event.task_id ||
        event.task_sequence || event.runtime_id.is_nil() || event.payload.type != kRecordPage ||
        event.payload.classification != EventClass::Critical || event.payload.data.size() > 384)
        return invalid_receipt();
    const auto decoded = parse_json(event.payload.data);
    if (!decoded)
        return invalid_receipt();
    const auto after = read_sequence(decoded.value(), "after");
    const auto through = read_sequence(decoded.value(), "through");
    const auto snapshot = read_sequence(decoded.value(), "snapshot");
    if (!after || !through || *after >= *through || *through < job.after_sequence || !snapshot ||
        (*snapshot != 0 && *snapshot <= *through) ||
        event.event_id != receipt_id(query.session_id, *through))
        return invalid_receipt();
    if (const auto durable = events->flush(Durability::ProcessCrash); !durable)
        return durable.error();
    job.after_sequence = *through;
    job.record_snapshot = *snapshot == 0 ? std::nullopt : snapshot;
    return {};
}

Result<void> MemoryAgentService::State::save_record_cursor(const Job &job,
                                                           const Work::RecordCursor &cursor,
                                                           const ConsolidationReport &report) {
    const auto partition = receipt_partition(job.binding, *job.request.write_scope);
    const auto applied = report.count_of(CandidateDisposition::Applied);
    const auto payload = to_json_string(JsonValue::Object{
        {"after", std::to_string(job.after_sequence)},
        {"through", std::to_string(cursor.through)},
        {"snapshot", std::to_string(cursor.snapshot.value_or(0))},
        {"examined", static_cast<std::int64_t>(report.candidates_examined)},
        {"applied", static_cast<std::int64_t>(applied)},
        {"rejected", static_cast<std::int64_t>(report.entries.size() - applied)},
    });
    const AppendRequest request{receipt_id(partition, cursor.through),
                                runtime,
                                partition,
                                {},
                                {std::string(kRecordPage), payload, EventClass::Critical},
                                0,
                                Durability::ProcessCrash};
    const auto result = events->append(request);
    if (!result)
        return result.error();
    if (result.value().event_id != request.event_id || result.value().session_sequence == 0 ||
        result.value().task_sequence || result.value().achieved < Durability::ProcessCrash)
        return invalid_receipt();
    return {};
}
} // namespace mira
