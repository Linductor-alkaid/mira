#include <mira/verified_fact.hpp>

#include "../support/ascii_text.hpp"
#include <mira/content_markers.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace mira {
namespace {
Error fact_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.verified_fact";
    error.safe_message = std::move(message);
    return error;
}
bool identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == ':' || c == '-';
           });
}
bool safe_text(std::string_view text, std::size_t bound) {
    return !text.empty() && text.size() <= bound &&
           std::none_of(text.begin(), text.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
Result<void> validate(const PublishedTaskFact &fact) {
    const auto &c = fact.claim;
    if (fact.runtime.is_nil() || fact.session.is_nil() || c.publication_id.is_nil() ||
        c.task.is_nil() || c.evidence_event.is_nil() || c.publication_id == c.evidence_event ||
        !identifier(c.key) || !identifier(c.verifier) || !safe_text(c.value, 1024) ||
        c.task_epoch > static_cast<std::uint64_t>(INT64_MAX) ||
        c.environment_epoch > static_cast<std::uint64_t>(INT64_MAX)) {
        return fact_error(ErrorCode::InvalidArgument,
                          "verified fact identity or content is invalid");
    }
    const auto &s = fact.scope;
    if ((s.kind != MemoryScopeKind::Environment && s.kind != MemoryScopeKind::Application &&
         s.kind != MemoryScopeKind::User) ||
        !safe_text(s.subject_id, 256) || (s.tenant_id && !safe_text(*s.tenant_id, 256))) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact scope is invalid");
    }
    if (c.valid_from.time_since_epoch().count() <= 0 || c.valid_until <= c.valid_from ||
        c.valid_until - c.valid_from > std::chrono::hours(24)) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact validity is invalid");
    }
    const auto forbidden = default_forbidden_content_markers();
    const auto injection = default_injection_content_markers();
    if (text_support::carries_marker(c.key, forbidden) ||
        text_support::carries_marker(c.value, forbidden) ||
        text_support::carries_marker(c.value, injection)) {
        return fact_error(ErrorCode::SafetyRejected, "verified fact content is forbidden");
    }
    return {};
}
EventPayload payload_for(const PublishedTaskFact &fact) {
    const auto &c = fact.claim;
    return {"TaskFactVerified",
            canonical_json_string(JsonValue::Object{
                {"schema", "mira.task-fact-verified.v1"},
                {"runtime_id", fact.runtime.to_string()},
                {"session_id", fact.session.to_string()},
                {"task_id", c.task.to_string()},
                {"task_epoch", static_cast<std::int64_t>(c.task_epoch)},
                {"environment_epoch", static_cast<std::int64_t>(c.environment_epoch)},
                {"scope", memory_scope_to_json(fact.scope)},
                {"claim", JsonValue::Object{{"key", c.key}, {"value", c.value}}},
                {"evidence_event", c.evidence_event.to_string()},
                {"evidence_digest", c.evidence_digest.to_string()},
                {"verifier", c.verifier},
                {"valid_from", wall_nanos(c.valid_from)},
                {"valid_until", wall_nanos(c.valid_until)}}),
            EventClass::Critical};
}
Result<void> ready(const OperationContext &context) {
    if (context.cancelled())
        return fact_error(ErrorCode::Cancelled, "fact publication cancelled");
    if (context.expired(Timestamp::now())) {
        return fact_error(ErrorCode::DeadlineExceeded, "fact publication deadline expired");
    }
    return {};
}
} // namespace

Result<PublishedTaskFact> parse_published_task_fact(const EventEnvelope &event) {
    if (event.payload.type != "TaskFactVerified" || event.payload.data.size() > 8192 ||
        event.payload.classification != EventClass::Critical) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact envelope is invalid");
    }
    auto parsed = parse_json(event.payload.data);
    if (!parsed || !parsed.value().is_object()) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact payload is invalid");
    }
    const auto &j = parsed.value();
    const auto *schema = j.find("schema");
    if (!schema || !schema->is_string() || *schema->as_string() != "mira.task-fact-verified.v1") {
        return fact_error(ErrorCode::UnsupportedVersion, "verified fact schema is unsupported");
    }
    constexpr std::string_view keys[] = {"schema",          "runtime_id", "session_id",
                                         "task_id",         "task_epoch", "environment_epoch",
                                         "scope",           "claim",      "evidence_event",
                                         "evidence_digest", "verifier",   "valid_from",
                                         "valid_until"};
    if (j.as_object()->size() != std::size(keys) ||
        std::any_of(std::begin(keys), std::end(keys), [&](auto key) { return !j.find(key); })) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact field set is invalid");
    }
    auto string = [&](const char *key) -> const std::string * { return j.find(key)->as_string(); };
    for (auto key :
         {"runtime_id", "session_id", "task_id", "evidence_event", "evidence_digest", "verifier"}) {
        if (!string(key))
            return fact_error(ErrorCode::InvalidArgument, "verified fact member type is invalid");
    }
    for (auto key : {"task_epoch", "environment_epoch", "valid_from", "valid_until"}) {
        if (!j.find(key)->is_integer())
            return fact_error(ErrorCode::InvalidArgument, "verified fact number type is invalid");
    }
    auto runtime = RuntimeId::parse(*string("runtime_id"));
    auto session = SessionId::parse(*string("session_id"));
    auto task = TaskId::parse(*string("task_id"));
    auto evidence = EventId::parse(*string("evidence_event"));
    auto digest = digest_from_hex(*string("evidence_digest"));
    const auto *scope_json = j.find("scope");
    if (!scope_json->is_object() || !scope_json->find("kind") ||
        !scope_json->find("kind")->is_string() || !scope_json->find("subject_id") ||
        !scope_json->find("subject_id")->is_string() ||
        (scope_json->find("tenant_id") && !scope_json->find("tenant_id")->is_string()) ||
        std::any_of(scope_json->as_object()->begin(), scope_json->as_object()->end(),
                    [](const auto &member) {
                        return member.first != "kind" && member.first != "subject_id" &&
                               member.first != "tenant_id";
                    })) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact scope fields are invalid");
    }
    auto scope = memory_scope_from_json(*scope_json);
    const auto *claim = j.find("claim");
    if (!runtime || !session || !task || !evidence || !digest || !scope || !claim->is_object() ||
        claim->as_object()->size() != 2 || !claim->find("key") || !claim->find("value") ||
        !claim->find("key")->is_string() || !claim->find("value")->is_string() ||
        *j.find("task_epoch")->as_integer() < 0 || *j.find("environment_epoch")->as_integer() < 0) {
        return fact_error(ErrorCode::InvalidArgument, "verified fact members are invalid");
    }
    PublishedTaskFact fact{*runtime, *session, scope.value(), {}};
    auto &c = fact.claim;
    c.publication_id = event.event_id;
    c.task = *task;
    c.task_epoch = static_cast<std::uint64_t>(*j.find("task_epoch")->as_integer());
    c.environment_epoch = static_cast<std::uint64_t>(*j.find("environment_epoch")->as_integer());
    c.key = *claim->find("key")->as_string();
    c.value = *claim->find("value")->as_string();
    c.evidence_event = *evidence;
    c.evidence_digest = *digest;
    c.verifier = *string("verifier");
    c.valid_from = WallTimePoint{std::chrono::nanoseconds(*j.find("valid_from")->as_integer())};
    c.valid_until = WallTimePoint{std::chrono::nanoseconds(*j.find("valid_until")->as_integer())};
    if (auto valid = validate(fact); !valid)
        return valid.error();
    if (event.runtime_id != fact.runtime || event.session_id != fact.session || !event.task_id ||
        *event.task_id != c.task) {
        return fact_error(ErrorCode::DataLoss, "verified fact envelope identity disagrees");
    }
    return fact;
}

VerifiedFactPublisher::VerifiedFactPublisher(RuntimeId runtime, SessionId session,
                                             MemoryScope scope, std::shared_ptr<IEventStore> events,
                                             VerifiedFactLeaseAcquirer acquire,
                                             VerifiedFactLimits limits)
    : runtime_(runtime), session_(session), scope_(std::move(scope)), events_(std::move(events)),
      acquire_(std::move(acquire)), limits_(limits) {}

Result<AppendReceipt> VerifiedFactPublisher::publish(const VerifiedFactClaim &claim,
                                                     const OperationContext &context) {
    PublishedTaskFact fact{runtime_, session_, scope_, claim};
    if (!events_ || !acquire_ || limits_.page_size == 0 || limits_.page_size > 4096 ||
        limits_.max_events_scanned == 0) {
        return fact_error(ErrorCode::InvalidState,
                          "fact publisher requires a bounded store and authority");
    }
    if (auto valid = validate(fact); !valid)
        return valid.error();
    if (context.session != session_ || context.task != claim.task ||
        context.task_epoch != claim.task_epoch || context.operation.is_nil()) {
        return fact_error(ErrorCode::InvalidArgument, "fact publication frame disagrees");
    }
    if (auto active = ready(context); !active)
        return active.error();
    if (!fact.valid_at(Timestamp::now().wall)) {
        return fact_error(ErrorCode::DeadlineExceeded, "fact validity interval is not current");
    }
    try {
        std::optional<EventEnvelope> source;
        SessionSequence after = 0;
        std::size_t scanned = 0;
        while (!source && scanned < limits_.max_events_scanned) {
            if (auto active = ready(context); !active)
                return active.error();
            const auto limit = std::min(limits_.page_size, limits_.max_events_scanned - scanned);
            auto page =
                events_->read({session_, after ? std::optional(after) : std::nullopt, limit});
            if (!page)
                return page.error();
            if (page.value().events.size() > limit ||
                (page.value().has_more && page.value().events.empty())) {
                return fact_error(ErrorCode::DataLoss, "fact evidence pagination is invalid");
            }
            for (const auto &event : page.value().events) {
                ++scanned;
                if (event.session_id != session_ || event.session_sequence <= after) {
                    return fact_error(ErrorCode::DataLoss, "fact evidence stream is invalid");
                }
                after = event.session_sequence;
                if (event.event_id == claim.evidence_event)
                    source = event;
            }
            if (!page.value().has_more)
                break;
        }
        if (!source)
            return fact_error(scanned == limits_.max_events_scanned ? ErrorCode::ResourceExhausted
                                                                    : ErrorCode::NotFound,
                              "fact evidence was not found within the scan budget");
        if (source->runtime_id != runtime_ || source->task_id != claim.task ||
            source->payload.type != "ObservationRecorded" ||
            digest_string(source->payload.data) != claim.evidence_digest ||
            source->payload.data.size() > 8192) {
            return fact_error(ErrorCode::InvalidArgument,
                              "fact evidence identity, digest or time disagrees");
        }
        const auto body = parse_json(source->payload.data);
        if (!body || !body.value().is_object())
            return fact_error(ErrorCode::InvalidArgument, "fact observation evidence is invalid");
        const auto *id = body.value().find("observation_id");
        const auto *epoch = body.value().find("task_epoch");
        const auto *environment = body.value().find("environment_epoch");
        const auto *observed_at = body.value().find("observed_at");
        const auto observation =
            id && id->is_string() ? ObservationId::parse(*id->as_string()) : std::nullopt;
        if (!observed_at || !observed_at->is_integer() || *observed_at->as_integer() <= 0 ||
            WallTimePoint{std::chrono::nanoseconds(*observed_at->as_integer())} >
                claim.valid_from ||
            claim.valid_until -
                    WallTimePoint{std::chrono::nanoseconds(*observed_at->as_integer())} >
                std::chrono::hours(24) ||
            !observation || observation->is_nil() || !epoch || !epoch->is_integer() ||
            !environment || !environment->is_integer() || *epoch->as_integer() < 0 ||
            *environment->as_integer() < 0 ||
            static_cast<std::uint64_t>(*epoch->as_integer()) != claim.task_epoch ||
            static_cast<std::uint64_t>(*environment->as_integer()) != claim.environment_epoch) {
            return fact_error(ErrorCode::InvalidArgument, "fact observation frame disagrees");
        }
        auto acquired = acquire_(fact, *source, context);
        if (!acquired)
            return acquired.error();
        auto lease = std::move(acquired).value();
        if (!lease)
            return fact_error(ErrorCode::InvalidState, "fact authority returned no lease");
        if (auto admitted = lease->validate(fact, *source, context); !admitted)
            return admitted.error();
        if (auto active = ready(context); !active)
            return active.error();
        if (!fact.valid_at(Timestamp::now().wall))
            return fact_error(ErrorCode::DeadlineExceeded, "fact expired before publication");
        auto receipt = events_->append({claim.publication_id, runtime_, session_, claim.task,
                                        payload_for(fact), 0, Durability::ProcessCrash});
        if (!receipt)
            return receipt.error();
        const auto &r = receipt.value();
        if (r.event_id != claim.publication_id || r.session_sequence == 0 || !r.task_sequence ||
            *r.task_sequence == 0 ||
            (r.achieved != Durability::ProcessCrash && r.achieved != Durability::PowerLoss)) {
            return fact_error(ErrorCode::DataLoss, "fact publication acknowledgement is invalid");
        }
        return receipt;
    } catch (...) {
        return fact_error(ErrorCode::Internal, "fact evidence, authority or store operation threw");
    }
}
} // namespace mira
