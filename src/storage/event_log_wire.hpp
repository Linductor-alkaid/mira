#pragma once

#include <mira/event_store.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace mira::event_log_wire {
inline constexpr std::string_view header = "MIRA_EVENT_LOG 2";
inline constexpr std::string_view records = "MIRA_EVENT_RECORDS";
struct Record final {
    AppendRequest request;
    SessionSequence sequence = 0;
    std::optional<TaskSequence> task_sequence;
    Sha256Digest request_digest;
    Timestamp timestamp;
    bool v2 = false;
};
inline Error failure(ErrorCode code, const char *message) {
    Error result;
    result.code = code;
    result.domain = "mira.event_store";
    result.safe_message = message;
    return result;
}
inline JsonValue fields(const Record &r) {
    return JsonValue::Object{
        {"schema", "mira.file-event.v2"},
        {"event_id", r.request.event_id.to_string()},
        {"runtime_id", r.request.runtime_id.to_string()},
        {"session_id", r.request.session_id.to_string()},
        {"task_id", r.request.task_id ? JsonValue{r.request.task_id->to_string()} : JsonValue{}},
        {"session_sequence", std::to_string(r.sequence)},
        {"task_sequence",
         r.task_sequence ? JsonValue{std::to_string(*r.task_sequence)} : JsonValue{}},
        {"cause", std::to_string(r.request.cause)},
        {"classification", static_cast<std::int64_t>(r.request.payload.classification)},
        {"type", r.request.payload.type},
        {"data", r.request.payload.data},
        {"wall_ns", wall_nanos(r.timestamp)},
        {"integrity", r.request_digest.to_string()}};
}
inline std::string encode(const Record &r) {
    auto body = fields(r);
    body.set("record_digest", digest_string(canonical_json_string(body)).to_string());
    return "E2 " + canonical_json_string(body);
}
inline bool number(const JsonValue *value, std::uint64_t &out) {
    if (!value || !value->is_string() || value->as_string()->empty())
        return false;
    const auto &text = *value->as_string();
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
inline Result<Record> decode_v2(std::string_view line) {
    if (!line.starts_with("E2 ")) {
        if (std::string_view{"E2 "}.starts_with(line))
            return failure(ErrorCode::InvalidArgument, "event log record prefix is incomplete");
        if (line.starts_with("E") || (!line.empty() && line.front() >= '0' && line.front() <= '9'))
            return failure(ErrorCode::UnsupportedVersion,
                           "event log record version is unsupported");
        return failure(ErrorCode::InvalidArgument, "event log record is incomplete");
    }
    // Event payloads are opaque strings, not model JSON documents. Preserve
    // the store's existing byte surface instead of inheriting model defaults.
    JsonLimits limits;
    limits.max_document_bytes = line.size();
    limits.max_string_bytes = line.size();
    auto parsed = parse_json(line.substr(3), limits);
    if (!parsed)
        return failure(ErrorCode::InvalidArgument, "event log JSON is incomplete");
    if (!parsed.value().is_object())
        return failure(ErrorCode::DataLoss, "event log JSON must be an object");
    const auto &j = parsed.value();
    const auto *schema = j.find("schema");
    if (!schema || !schema->is_string() || *schema->as_string() != "mira.file-event.v2")
        return failure(ErrorCode::UnsupportedVersion, "event log schema is unsupported");
    constexpr std::string_view keys[] = {"schema",        "event_id",     "runtime_id",
                                         "session_id",    "task_id",      "session_sequence",
                                         "task_sequence", "cause",        "classification",
                                         "type",          "data",         "wall_ns",
                                         "integrity",     "record_digest"};
    if (j.as_object()->size() != std::size(keys) ||
        std::any_of(std::begin(keys), std::end(keys), [&](auto key) { return !j.find(key); }))
        return failure(ErrorCode::UnsupportedVersion, "event log field set is unsupported");
    for (auto key :
         {"event_id", "runtime_id", "session_id", "type", "data", "integrity", "record_digest"}) {
        if (!j.find(key)->is_string())
            return failure(ErrorCode::DataLoss, "event log member type is invalid");
    }
    const auto event = EventId::parse(*j.find("event_id")->as_string());
    const auto runtime = RuntimeId::parse(*j.find("runtime_id")->as_string());
    const auto session = SessionId::parse(*j.find("session_id")->as_string());
    auto digest = digest_from_hex(*j.find("integrity")->as_string());
    const auto record_digest = digest_from_hex(*j.find("record_digest")->as_string());
    if (!event || event->is_nil() || !runtime || runtime->is_nil() || !session ||
        session->is_nil() || !digest || !record_digest || !j.find("classification")->is_integer() ||
        !j.find("wall_ns")->is_integer())
        return failure(ErrorCode::DataLoss, "event log identity or time is invalid");
    const auto classification = *j.find("classification")->as_integer();
    if (classification < 0 || classification > 2)
        return failure(ErrorCode::DataLoss, "event log classification is invalid");
    Record r;
    r.v2 = true;
    r.request = {*event,
                 *runtime,
                 *session,
                 std::nullopt,
                 {*j.find("type")->as_string(), *j.find("data")->as_string(),
                  static_cast<EventClass>(classification)}};
    r.request_digest = *digest;
    r.timestamp.wall = WallTimePoint{std::chrono::nanoseconds(*j.find("wall_ns")->as_integer())};
    if (!number(j.find("session_sequence"), r.sequence) || r.sequence == 0 ||
        !number(j.find("cause"), r.request.cause))
        return failure(ErrorCode::DataLoss, "event log sequence or cause is invalid");
    if (!j.find("task_id")->is_null()) {
        if (!j.find("task_id")->is_string())
            return failure(ErrorCode::DataLoss, "event log task identity is invalid");
        auto task = TaskId::parse(*j.find("task_id")->as_string());
        std::uint64_t task_sequence = 0;
        if (!task || task->is_nil() || !number(j.find("task_sequence"), task_sequence) ||
            task_sequence == 0)
            return failure(ErrorCode::DataLoss, "event log task sequence is invalid");
        r.request.task_id = *task;
        r.task_sequence = task_sequence;
    } else if (!j.find("task_sequence")->is_null()) {
        return failure(ErrorCode::DataLoss, "event log task sequence has no task");
    }
    if (digest_string(canonical_json_string(fields(r))) != *record_digest)
        return failure(ErrorCode::DataLoss, "event record metadata digest mismatch");
    return r;
}
inline Result<Record> decode_legacy(const std::string &line) {
    std::istringstream stream(line);
    Record r;
    std::string event_text, runtime_text, session_text, task_text, integrity;
    unsigned int classification = 0;
    if (!(stream >> r.sequence >> event_text >> runtime_text >> session_text >> task_text >>
          r.request.cause >> classification >> std::quoted(r.request.payload.type) >>
          std::quoted(r.request.payload.data)))
        return failure(ErrorCode::InvalidArgument, "legacy event record is incomplete");
    const auto event = EventId::parse(event_text);
    const auto runtime = RuntimeId::parse(runtime_text);
    const auto session = SessionId::parse(session_text);
    if (!event || event->is_nil() || !runtime || runtime->is_nil() || !session ||
        session->is_nil() || classification > 2)
        return failure(ErrorCode::DataLoss, "legacy event identity or classification is invalid");
    r.request.event_id = *event;
    r.request.runtime_id = *runtime;
    r.request.session_id = *session;
    r.request.payload.classification = static_cast<EventClass>(classification);
    if (task_text != "-") {
        const auto task = TaskId::parse(task_text);
        if (!task || task->is_nil())
            return failure(ErrorCode::DataLoss, "legacy event task identity is invalid");
        r.request.task_id = *task;
    }
    if (!(stream >> integrity))
        return failure(ErrorCode::DataLoss, "legacy event digest is missing");
    const auto digest = digest_from_hex(integrity);
    if (!digest)
        return failure(ErrorCode::DataLoss, "legacy event digest is invalid");
    r.request_digest = *digest;
    std::string extra;
    if (stream >> extra)
        return failure(ErrorCode::DataLoss, "legacy event contains trailing fields");
    return r;
}
} // namespace mira::event_log_wire
