#include <mira/action_journal.hpp>

#include "action_journal_internal.hpp"

#include <algorithm>

namespace mira {
namespace {
Error journal_error(ErrorCode code, std::string message) {
    Error result;
    result.code = code;
    result.domain = "mira.action_journal";
    result.safe_message = std::move(message);
    return result;
}

} // namespace

Result<AppendReceipt> ActionJournal::append(const ActionIntent &intent, std::string type,
                                            std::string data, EventClass classification,
                                            Durability durability) {
    const auto event_id = EventId::generate();
    Result<AppendReceipt> result(Error{});
    try {
        result = store_.append({event_id, runtime_id_, intent.session_id, intent.task_id,
                                EventPayload{std::move(type), std::move(data), classification}, 0,
                                durability});
    } catch (...) {
        return journal_error(ErrorCode::Internal,
                             "action journal store append raised an exception");
    }
    if (!result) {
        return result.error();
    }
    if (result.value().event_id != event_id || result.value().session_sequence == 0 ||
        !result.value().task_sequence || *result.value().task_sequence == 0) {
        return journal_error(ErrorCode::DataLoss,
                             "action journal append receipt identity is invalid");
    }
    if (static_cast<std::uint8_t>(result.value().achieved) <
            static_cast<std::uint8_t>(durability) ||
        static_cast<std::uint8_t>(result.value().achieved) >
            static_cast<std::uint8_t>(Durability::PowerLoss)) {
        return journal_error(ErrorCode::DataLoss,
                             "action journal append did not achieve required durability");
    }
    return result;
}

Result<AppendReceipt> ActionJournal::prepare(const ActionIntent &intent) {
    std::lock_guard lock(mutex_);
    if (auto valid = validate(intent); !valid) {
        return valid.error();
    }
    const auto found = states_.find(intent.action_id);
    if (found != states_.end())
        return found->second.state.last_receipt.value();
    auto result = append(intent, "ActionPrepared", journal_internal::encode(intent),
                         EventClass::State, Durability::ProcessCrash);
    if (!result)
        return result.error();
    states_.emplace(
        intent.action_id,
        Entry{journal_internal::binding(intent),
              ActionJournalState{intent.action_id, ActionJournalPhase::Prepared, result.value()}});
    return result;
}

Result<AppendReceipt> ActionJournal::dispatch_started(const ActionIntent &intent) {
    std::lock_guard lock(mutex_);
    if (auto valid = validate(intent); !valid) {
        return valid.error();
    }
    const auto found = states_.find(intent.action_id);
    if (found == states_.end())
        return journal_error(ErrorCode::InvalidState, "action must be prepared first");
    if (found->second.state.phase == ActionJournalPhase::DispatchStarted ||
        found->second.state.phase == ActionJournalPhase::Receipt ||
        found->second.state.phase == ActionJournalPhase::ExecutionUncertain) {
        return found->second.state.last_receipt.value();
    }
    auto result = append(intent, "ActionDispatchStarted", journal_internal::encode(intent),
                         EventClass::Critical, Durability::ProcessCrash);
    if (!result)
        return result.error();
    found->second.state.phase = ActionJournalPhase::DispatchStarted;
    found->second.state.last_receipt = result.value();
    return result;
}

Result<AppendReceipt> ActionJournal::receipt(const ActionIntent &intent,
                                             const ExecutionReceipt &execution) {
    std::lock_guard lock(mutex_);
    if (auto valid = validate(intent); !valid) {
        return valid.error();
    }
    const auto found = states_.find(intent.action_id);
    if (found == states_.end() || found->second.state.phase == ActionJournalPhase::Prepared) {
        return journal_error(ErrorCode::InvalidState, "dispatch must be durably started first");
    }
    if (found->second.state.phase == ActionJournalPhase::Receipt)
        return found->second.state.last_receipt.value();
    if (!journal_internal::safe_text(execution.safe_message, 4096, false)) {
        return journal_error(ErrorCode::InvalidArgument, "action receipt summary is invalid");
    }
    auto result =
        append(intent, "ActionReceipt", journal_internal::encode(intent, execution.safe_message),
               EventClass::State, Durability::ProcessCrash);
    if (!result)
        return result.error();
    found->second.state.phase = ActionJournalPhase::Receipt;
    found->second.state.last_receipt = result.value();
    return result;
}

Result<AppendReceipt> ActionJournal::execution_uncertain(const ActionIntent &intent,
                                                         std::string reason) {
    std::lock_guard lock(mutex_);
    if (auto valid = validate(intent); !valid) {
        return valid.error();
    }
    const auto found = states_.find(intent.action_id);
    if (found == states_.end() || found->second.state.phase == ActionJournalPhase::Prepared) {
        return journal_error(ErrorCode::InvalidState, "dispatch must be durably started first");
    }
    if (found->second.state.phase == ActionJournalPhase::ExecutionUncertain)
        return found->second.state.last_receipt.value();
    if (!journal_internal::safe_text(reason, 4096, false)) {
        return journal_error(ErrorCode::InvalidArgument, "action uncertainty summary is invalid");
    }
    auto result =
        append(intent, "ActionExecutionUncertain", journal_internal::encode(intent, reason),
               EventClass::Critical, Durability::ProcessCrash);
    if (!result)
        return result.error();
    found->second.state.phase = ActionJournalPhase::ExecutionUncertain;
    found->second.state.last_receipt = result.value();
    return result;
}

Result<void> ActionJournal::validate(const ActionIntent &intent) const {
    if (!journal_internal::valid_intent(intent, runtime_id_)) {
        return journal_error(ErrorCode::InvalidArgument,
                             "action intent identity or target is invalid");
    }
    const auto found = states_.find(intent.action_id);
    if (found != states_.end() && found->second.binding != journal_internal::binding(intent)) {
        return journal_error(ErrorCode::InvalidArgument,
                             "action ID is bound to a different intent");
    }
    return {};
}

Result<ActionJournalState> ActionJournal::recover(const ActionIntent &intent) const {
    std::lock_guard lock(mutex_);
    if (auto valid = validate(intent); !valid) {
        return valid.error();
    }
    const auto found = states_.find(intent.action_id);
    if (found != states_.end()) {
        return found->second.state;
    }
    if (recovery_policy_.page_size == 0 || recovery_policy_.max_events_scanned == 0) {
        return journal_error(ErrorCode::InvalidArgument,
                             "journal recovery bounds must be positive");
    }
    ActionJournalState state{intent.action_id, ActionJournalPhase::Prepared, std::nullopt};
    std::optional<SessionSequence> cursor;
    std::size_t scanned = 0;
    for (;;) {
        const auto limit =
            std::min(recovery_policy_.page_size, recovery_policy_.max_events_scanned - scanned);
        Result<EventPage> page(Error{});
        try {
            page = store_.read({intent.session_id, cursor, limit});
        } catch (...) {
            return journal_error(ErrorCode::Internal, "journal recovery read raised an exception");
        }
        if (!page) {
            return page.error();
        }
        if (page.value().events.size() > limit ||
            (page.value().events.empty() && page.value().has_more)) {
            return journal_error(ErrorCode::DataLoss,
                                 "journal recovery page made no valid progress");
        }
        for (const auto &event : page.value().events) {
            if (event.session_id != intent.session_id ||
                event.session_sequence <= cursor.value_or(0)) {
                return journal_error(ErrorCode::DataLoss,
                                     "journal recovery page identity or order is invalid");
            }
            cursor = event.session_sequence;
            ++scanned;
            const auto &type = event.payload.type;
            if (type != "ActionPrepared" && type != "ActionDispatchStarted" &&
                type != "ActionReceipt" && type != "ActionExecutionUncertain") {
                continue;
            }
            const auto payload = journal_internal::parse(event.payload.data);
            if (!payload || event.task_id != payload->task_id) {
                return journal_error(ErrorCode::DataLoss, "journal recovery payload is invalid");
            }
            if (payload->action_id != intent.action_id) {
                continue;
            }
            if (event.runtime_id != runtime_id_ || !journal_internal::matches(*payload, intent)) {
                return journal_error(ErrorCode::DataLoss,
                                     "journal action identity conflicts with intent");
            }
            if (type == "ActionPrepared") {
                if (state.phase != ActionJournalPhase::Prepared) {
                    return journal_error(ErrorCode::DataLoss, "journal prepare follows dispatch");
                }
            } else if (type == "ActionDispatchStarted") {
                if (state.phase == ActionJournalPhase::Prepared) {
                    state.phase = ActionJournalPhase::DispatchStarted;
                }
            } else {
                if (state.phase == ActionJournalPhase::Prepared) {
                    return journal_error(ErrorCode::DataLoss,
                                         "journal result has no dispatch start");
                }
                state.phase = type == "ActionReceipt" ? ActionJournalPhase::Receipt
                                                      : ActionJournalPhase::ExecutionUncertain;
            }
            state.last_receipt = AppendReceipt{event.event_id, event.session_sequence,
                                               event.task_sequence, 0, Durability::ProcessCrash};
        }
        if (!page.value().has_more) {
            return state;
        }
        if (scanned == recovery_policy_.max_events_scanned) {
            return journal_error(ErrorCode::ResourceExhausted,
                                 "journal recovery scan exceeded its bound");
        }
    }
}

} // namespace mira
