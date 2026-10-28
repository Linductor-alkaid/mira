#include <mira/interaction_coordinator.hpp>

#include <mira/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mira {
namespace {

// The Mira-owned partition of the interaction ledger stream. The bytes are
// fixed so a restarted process resolves the same durable partition without
// persisting an id (DEC-068 §8).
constexpr std::array<std::uint8_t, 16> kLedgerPartitionBytes{
    0x6d, 0x69, 0x72, 0x61, 0x2e, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x61, 0x63, 0x74, 0x2e, 0x6c, 0x31};

// The wall instant of the ledger. It is pinned to nanosecond precision
// (WallTimePoint) for the same reason the rest of the runtime pins it: the
// default system_clock duration cannot represent nanoseconds on every
// supported toolchain (MSVC ticks at 100 ns, libc++ differs again), so a
// functional cast from nanoseconds is not portable.
[[nodiscard]] WallTimePoint now_wall() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

[[nodiscard]] std::int64_t to_nanos(WallTimePoint point) {
    return static_cast<std::int64_t>(point.time_since_epoch().count());
}

[[nodiscard]] WallTimePoint from_nanos(std::int64_t nanos) {
    return WallTimePoint(std::chrono::nanoseconds(nanos));
}

// The principal reference of an event payload: the irreversible internal
// identifiers of the tenant and the user. Never an external account, a
// credential or a display name (AGENTS.md log redaction).
[[nodiscard]] std::string principal_ref(const PrincipalContext &principal) {
    return principal.tenant_id.to_string() + ":" + principal.user_id.to_string();
}

[[nodiscard]] JsonValue::Object accepted_payload(const LedgerEntry &entry,
                                                 const PrincipalContext &principal) {
    JsonValue::Object object;
    object.emplace_back("schema",
                        std::string(interaction_event_schema(InteractionEventType::InputAccepted)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    object.emplace_back("principal_ref", principal_ref(principal));
    object.emplace_back("admitted_at", wall_nanos(entry.admitted_at));
    object.emplace_back("window_expires_at", to_nanos(entry.window_expires_at));
    return object;
}

[[nodiscard]] JsonValue::Object routed_payload(const LedgerEntry &entry,
                                               const LedgerTransition &transition) {
    JsonValue::Object object;
    object.emplace_back("schema",
                        std::string(interaction_event_schema(InteractionEventType::InputRouted)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    object.emplace_back("intent", std::string(transition.intent.has_value()
                                                  ? route_intent_name(*transition.intent)
                                                  : std::string_view("")));
    object.emplace_back("session_id", transition.session_id ? transition.session_id->to_string()
                                                            : std::string(""));
    object.emplace_back("task_id",
                        transition.task_id ? transition.task_id->to_string() : std::string(""));
    object.emplace_back("rationale", transition.note);
    return object;
}

[[nodiscard]] JsonValue::Object clarified_payload(const LedgerEntry &entry,
                                                  const LedgerTransition &transition) {
    JsonValue::Object object;
    object.emplace_back(
        "schema", std::string(interaction_event_schema(InteractionEventType::InputClarified)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    object.emplace_back("clarification", transition.clarification.value_or(std::string{}));
    return object;
}

[[nodiscard]] JsonValue::Object rejected_payload(const LedgerEntry &entry,
                                                 const LedgerTransition &transition) {
    JsonValue::Object object;
    object.emplace_back("schema",
                        std::string(interaction_event_schema(InteractionEventType::InputRejected)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    const auto code = transition.error ? transition.error->domain_code
                                       : static_cast<std::int32_t>(InteractionDomainCode::Unknown);
    object.emplace_back("reason_code", static_cast<std::int64_t>(code));
    object.emplace_back("reason",
                        transition.error ? transition.error->safe_message : transition.note);
    return object;
}

[[nodiscard]] JsonValue::Object answered_payload(const LedgerEntry &entry,
                                                 const LedgerTransition &transition) {
    JsonValue::Object object;
    object.emplace_back("schema",
                        std::string(interaction_event_schema(InteractionEventType::InputAnswered)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    object.emplace_back("session_id", transition.session_id ? transition.session_id->to_string()
                                                            : std::string(""));
    object.emplace_back("task_id",
                        transition.task_id ? transition.task_id->to_string() : std::string(""));
    object.emplace_back("summary", transition.note);
    return object;
}

[[nodiscard]] JsonValue::Object settled_payload(const LedgerEntry &entry,
                                                const LedgerTransition &transition) {
    JsonValue::Object object;
    object.emplace_back(
        "schema", std::string(interaction_event_schema(InteractionEventType::InputTaskSettled)));
    object.emplace_back("input_id", entry.input_id);
    object.emplace_back("ingress_id", entry.ingress_id.to_string());
    object.emplace_back("task_id",
                        transition.task_id ? transition.task_id->to_string() : std::string(""));
    object.emplace_back("terminal", transition.note);
    return object;
}

// The stage order of the closed stage set. Rejected is terminal and may be
// reached from any stage; the other stages never regress.
[[nodiscard]] int stage_rank(InputStage stage) {
    switch (stage) {
    case InputStage::Accepted:
        return 0;
    case InputStage::Routed:
        return 1;
    case InputStage::Answered:
        return 2;
    case InputStage::TaskSettled:
        return 3;
    case InputStage::Rejected:
        return 4;
    }
    return 0;
}

} // namespace

SessionId interaction_ledger_partition() { return SessionId{Id128{kLedgerPartitionBytes}}; }

class EventStoreInputLedger::Impl final {
  public:
    Impl(std::shared_ptr<IEventStore> store, RuntimeId runtime_id,
         InputLedgerOptions ledger_options)
        : events(std::move(store)), runtime(runtime_id), options(ledger_options) {
        if (!this->options.partition.has_value()) {
            this->options.partition = interaction_ledger_partition();
        }
        partition = *this->options.partition;
    }

    [[nodiscard]] Result<void> append(const std::string &type, JsonValue::Object object) const {
        if (events == nullptr) {
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::LedgerUnavailable,
                                     "the interaction ledger has no event store");
        }
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = runtime;
        request.session_id = partition;
        request.payload =
            EventPayload{type, to_json_string(JsonValue(std::move(object))), EventClass::State};
        const auto receipt = events->append(request);
        if (!receipt) {
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::LedgerUnavailable,
                                     "the interaction ledger could not append its record");
        }
        return Result<void>{};
    }

    std::shared_ptr<IEventStore> events;
    RuntimeId runtime;
    InputLedgerOptions options;
    SessionId partition;
    mutable std::mutex mutex;
    std::unordered_map<std::string, LedgerEntry> index;
};

EventStoreInputLedger::EventStoreInputLedger(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                                             InputLedgerOptions options)
    : impl_(std::make_unique<Impl>(std::move(events), runtime, options)) {}

EventStoreInputLedger::~EventStoreInputLedger() = default;

SessionId EventStoreInputLedger::partition() const noexcept { return impl_->partition; }

std::size_t EventStoreInputLedger::size() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->index.size();
}

Result<LedgerEntry> EventStoreInputLedger::admit(const InputEnvelope &envelope) {
    if (envelope.input_id.empty()) {
        return interaction_error(ErrorCode::InvalidArgument, InteractionDomainCode::InputInvalid,
                                 "the input id must not be empty");
    }
    std::lock_guard lock(impl_->mutex);
    const auto now = now_wall();
    const auto found = impl_->index.find(envelope.input_id);
    if (found != impl_->index.end()) {
        if (now <= found->second.window_expires_at) {
            auto entry = found->second;
            entry.duplicate = true;
            return entry;
        }
        // The window expired: the repeat is an explicit rejection. It is
        // never executed as a new input and the expired record is never
        // silently dropped to make room for it.
        return interaction_error(
            ErrorCode::InvalidState, InteractionDomainCode::InputExpired,
            "the input id was already used and its idempotency window expired");
    }
    if (impl_->index.size() >= impl_->options.max_entries) {
        // Reclaim the expired records first; a full ledger is an explicit
        // error, never a silent drop.
        for (auto it = impl_->index.begin(); it != impl_->index.end();) {
            if (now > it->second.window_expires_at) {
                it = impl_->index.erase(it);
            } else {
                ++it;
            }
        }
        if (impl_->index.size() >= impl_->options.max_entries) {
            return interaction_error(ErrorCode::ResourceExhausted,
                                     InteractionDomainCode::LedgerFull,
                                     "the interaction ledger index is full");
        }
    }
    LedgerEntry entry;
    entry.input_id = envelope.input_id;
    entry.ingress_id = ControlIngressId::generate();
    entry.admitted_at = Timestamp::now();
    entry.stage = InputStage::Accepted;
    entry.window_expires_at = now + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        impl_->options.idempotency_window);
    const auto appended =
        impl_->append("InputAccepted", accepted_payload(entry, envelope.principal));
    if (!appended) {
        return appended.error();
    }
    impl_->index.emplace(entry.input_id, entry);
    return entry;
}

Result<LedgerEntry> EventStoreInputLedger::lookup(std::string_view input_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->index.find(std::string(input_id));
    if (found == impl_->index.end()) {
        return interaction_error(ErrorCode::NotFound, InteractionDomainCode::InputInvalid,
                                 "the input id is not recorded in the interaction ledger");
    }
    return found->second;
}

Result<LedgerEntry> EventStoreInputLedger::advance(std::string_view input_id,
                                                   const LedgerTransition &transition) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->index.find(std::string(input_id));
    if (found == impl_->index.end()) {
        return interaction_error(ErrorCode::NotFound, InteractionDomainCode::InputInvalid,
                                 "the input id is not recorded in the interaction ledger");
    }
    auto &entry = found->second;
    const bool clarifying = transition.clarification.has_value();
    if (!clarifying && transition.stage != InputStage::Rejected &&
        stage_rank(transition.stage) < stage_rank(entry.stage)) {
        // The stage never regresses; a late report of an earlier stage is
        // ignored instead of rewinding the record.
        return entry;
    }
    if (entry.stage == InputStage::Rejected && transition.stage == InputStage::Rejected) {
        // A rejection is terminal and idempotent.
        return entry;
    }
    if (transition.stage != InputStage::Accepted || clarifying) {
        entry.stage = transition.stage;
    }
    if (transition.session_id.has_value()) {
        entry.session_id = transition.session_id;
    }
    if (transition.task_id.has_value()) {
        entry.task_id = transition.task_id;
    }
    if (!transition.note.empty()) {
        entry.note = transition.note;
    }
    if (transition.clarification.has_value()) {
        entry.clarification = transition.clarification;
    }
    if (transition.error.has_value()) {
        entry.error = transition.error;
    }

    JsonValue::Object object;
    std::string type;
    if (transition.stage == InputStage::Rejected) {
        type = "InputRejected";
        object = rejected_payload(entry, transition);
    } else if (transition.stage == InputStage::Routed) {
        type = "InputRouted";
        object = routed_payload(entry, transition);
    } else if (transition.stage == InputStage::Answered) {
        type = "InputAnswered";
        object = answered_payload(entry, transition);
    } else if (transition.stage == InputStage::TaskSettled) {
        type = "InputTaskSettled";
        object = settled_payload(entry, transition);
    } else if (transition.clarification.has_value()) {
        type = "InputClarified";
        object = clarified_payload(entry, transition);
    } else {
        // No visible transition: the record keeps its stage.
        return entry;
    }
    const auto appended = impl_->append(type, std::move(object));
    if (!appended) {
        return appended.error();
    }
    return entry;
}

Result<std::size_t> EventStoreInputLedger::reclaim_expired() {
    std::lock_guard lock(impl_->mutex);
    const auto now = now_wall();
    std::size_t reclaimed = 0;
    for (auto it = impl_->index.begin(); it != impl_->index.end();) {
        if (now > it->second.window_expires_at) {
            it = impl_->index.erase(it);
            ++reclaimed;
        } else {
            ++it;
        }
    }
    return reclaimed;
}

Result<void> EventStoreInputLedger::rebuild() {
    if (impl_->events == nullptr) {
        return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::LedgerUnavailable,
                                 "the interaction ledger has no event store");
    }
    std::unordered_map<std::string, LedgerEntry> rebuilt;
    EventQuery query;
    query.session_id = impl_->partition;
    query.limit = 1024;
    while (true) {
        const auto page = impl_->events->read(query);
        if (!page) {
            return interaction_error(ErrorCode::Unavailable,
                                     InteractionDomainCode::LedgerUnavailable,
                                     "the interaction ledger stream could not be read");
        }
        for (const auto &envelope : page.value().events) {
            const auto parsed = parse_json(envelope.payload.data);
            if (!parsed || !parsed.value().is_object()) {
                continue;
            }
            const auto *schema_member = parsed.value().find("schema");
            const auto *input_member = parsed.value().find("input_id");
            if (schema_member == nullptr || !schema_member->is_string() ||
                input_member == nullptr || !input_member->is_string()) {
                continue;
            }
            const auto schema = *schema_member->as_string();
            const auto input_id = *input_member->as_string();
            const auto *ingress_member = parsed.value().find("ingress_id");
            if (ingress_member == nullptr || !ingress_member->is_string()) {
                continue;
            }
            const auto ingress = ControlIngressId::parse(*ingress_member->as_string());
            if (!ingress) {
                continue;
            }
            if (parse_interaction_event_schema(schema) == InteractionEventType::InputAccepted) {
                const auto *admitted_member = parsed.value().find("admitted_at");
                const auto *expires_member = parsed.value().find("window_expires_at");
                LedgerEntry entry;
                entry.input_id = input_id;
                entry.ingress_id = *ingress;
                entry.stage = InputStage::Accepted;
                if (admitted_member != nullptr && admitted_member->is_integer()) {
                    entry.admitted_at =
                        timestamp_from_nanos(admitted_member->as_integer().value(), 0);
                }
                if (expires_member != nullptr && expires_member->is_integer()) {
                    entry.window_expires_at = from_nanos(expires_member->as_integer().value());
                }
                rebuilt[input_id] = entry;
                continue;
            }
            const auto found = rebuilt.find(input_id);
            if (found == rebuilt.end()) {
                // A transition without its acceptance record: the acceptance
                // event was lost, so the record cannot be reconstructed.
                continue;
            }
            auto &entry = found->second;
            const auto type = parse_interaction_event_schema(schema);
            if (type == InteractionEventType::InputRouted) {
                entry.stage = InputStage::Routed;
                const auto *session_member = parsed.value().find("session_id");
                const auto *task_member = parsed.value().find("task_id");
                const auto *rationale_member = parsed.value().find("rationale");
                if (session_member != nullptr && session_member->is_string()) {
                    const auto session = SessionId::parse(*session_member->as_string());
                    if (session) {
                        entry.session_id = *session;
                    }
                }
                if (task_member != nullptr && task_member->is_string()) {
                    const auto task = TaskId::parse(*task_member->as_string());
                    if (task) {
                        entry.task_id = *task;
                    }
                }
                if (rationale_member != nullptr && rationale_member->is_string()) {
                    entry.note = *rationale_member->as_string();
                }
            } else if (type == InteractionEventType::InputClarified) {
                const auto *clarification_member = parsed.value().find("clarification");
                if (clarification_member != nullptr && clarification_member->is_string()) {
                    entry.clarification = *clarification_member->as_string();
                }
            } else if (type == InteractionEventType::InputRejected) {
                entry.stage = InputStage::Rejected;
                const auto *reason_member = parsed.value().find("reason");
                const auto *code_member = parsed.value().find("reason_code");
                Error error;
                error.code = ErrorCode::InvalidState;
                error.domain = std::string(kInteractionErrorDomain);
                error.domain_code =
                    code_member != nullptr && code_member->is_integer()
                        ? static_cast<std::int32_t>(code_member->as_integer().value())
                        : static_cast<std::int32_t>(InteractionDomainCode::Unknown);
                error.safe_message = reason_member != nullptr && reason_member->is_string()
                                         ? *reason_member->as_string()
                                         : "the input was rejected";
                entry.error = error;
            } else if (type == InteractionEventType::InputAnswered) {
                entry.stage = InputStage::Answered;
                const auto *summary_member = parsed.value().find("summary");
                if (summary_member != nullptr && summary_member->is_string()) {
                    entry.note = *summary_member->as_string();
                }
            } else if (type == InteractionEventType::InputTaskSettled) {
                entry.stage = InputStage::TaskSettled;
                const auto *task_member = parsed.value().find("task_id");
                if (task_member != nullptr && task_member->is_string()) {
                    const auto task = TaskId::parse(*task_member->as_string());
                    if (task) {
                        entry.task_id = *task;
                    }
                }
            }
        }
        if (!page.value().has_more || page.value().events.empty()) {
            break;
        }
        query.after_sequence = page.value().events.back().session_sequence;
    }
    std::lock_guard lock(impl_->mutex);
    impl_->index = std::move(rebuilt);
    return Result<void>{};
}

} // namespace mira
