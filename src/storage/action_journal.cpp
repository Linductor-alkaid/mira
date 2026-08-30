#include <mira/action_journal.hpp>

#include <sstream>

namespace mira {
namespace {
Error journal_error(ErrorCode code, std::string message) {
    Error result;
    result.code = code;
    result.domain = "mira.action_journal";
    result.safe_message = std::move(message);
    return result;
}

std::string intent_data(const ActionIntent &intent, std::string_view suffix = {}) {
    std::ostringstream data;
    data << intent.action_id.to_string() << '|' << intent.task_id.to_string() << '|'
         << intent.task_epoch << '|' << intent.environment_epoch << '|' << intent.action_digest.to_string()
         << '|' << intent.target.type << '|' << intent.target.id << '|' << intent.target.scope;
    if (!suffix.empty()) data << '|' << suffix;
    return data.str();
}
} // namespace

Result<AppendReceipt> ActionJournal::append(const ActionIntent &intent, std::string type,
                                            std::string data, EventClass classification,
                                            Durability durability) {
    if (intent.action_id.is_nil() || intent.runtime_id.is_nil() || intent.session_id.is_nil() ||
        intent.task_id.is_nil()) {
        return journal_error(ErrorCode::InvalidArgument, "action intent IDs must be non-nil");
    }
    return store_.append({EventId::generate(), runtime_id_, intent.session_id, intent.task_id,
                          EventPayload{std::move(type), std::move(data), classification}, 0, durability});
}

Result<AppendReceipt> ActionJournal::prepare(const ActionIntent &intent) {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(intent.action_id);
    if (found != states_.end()) return found->second.last_receipt.value();
    auto result = append(intent, "ActionPrepared", intent_data(intent), EventClass::State,
                         Durability::ProcessCrash);
    if (!result) return result.error();
    states_.emplace(intent.action_id,
                    ActionJournalState{intent.action_id, ActionJournalPhase::Prepared, result.value()});
    return result;
}

Result<AppendReceipt> ActionJournal::dispatch_started(const ActionIntent &intent) {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(intent.action_id);
    if (found == states_.end()) return journal_error(ErrorCode::InvalidState, "action must be prepared first");
    if (found->second.phase == ActionJournalPhase::DispatchStarted ||
        found->second.phase == ActionJournalPhase::Receipt ||
        found->second.phase == ActionJournalPhase::ExecutionUncertain) {
        return found->second.last_receipt.value();
    }
    auto result = append(intent, "ActionDispatchStarted", intent_data(intent), EventClass::Critical,
                         Durability::ProcessCrash);
    if (!result) return result.error();
    found->second.phase = ActionJournalPhase::DispatchStarted;
    found->second.last_receipt = result.value();
    return result;
}

Result<AppendReceipt> ActionJournal::receipt(const ActionIntent &intent,
                                             const ExecutionReceipt &execution) {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(intent.action_id);
    if (found == states_.end() || found->second.phase == ActionJournalPhase::Prepared) {
        return journal_error(ErrorCode::InvalidState, "dispatch must be durably started first");
    }
    if (found->second.phase == ActionJournalPhase::Receipt) return found->second.last_receipt.value();
    auto result = append(intent, "ActionReceipt", intent_data(intent, execution.safe_message), EventClass::State,
                         Durability::ProcessCrash);
    if (!result) return result.error();
    found->second.phase = ActionJournalPhase::Receipt;
    found->second.last_receipt = result.value();
    return result;
}

Result<AppendReceipt> ActionJournal::execution_uncertain(const ActionIntent &intent, std::string reason) {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(intent.action_id);
    if (found == states_.end() || found->second.phase == ActionJournalPhase::Prepared) {
        return journal_error(ErrorCode::InvalidState, "dispatch must be durably started first");
    }
    if (found->second.phase == ActionJournalPhase::ExecutionUncertain) return found->second.last_receipt.value();
    auto result = append(intent, "ActionExecutionUncertain", intent_data(intent, reason), EventClass::Critical,
                         Durability::ProcessCrash);
    if (!result) return result.error();
    found->second.phase = ActionJournalPhase::ExecutionUncertain;
    found->second.last_receipt = result.value();
    return result;
}

Result<ActionJournalState> ActionJournal::recover(const ActionIntent &intent) const {
    std::lock_guard lock(mutex_);
    const auto found = states_.find(intent.action_id);
    if (found != states_.end()) return found->second;
    auto page = store_.read({intent.session_id, std::nullopt, 100000});
    if (!page) return page.error();
    ActionJournalState state{intent.action_id, ActionJournalPhase::Prepared, std::nullopt};
    const auto marker = intent.action_id.to_string() + "|";
    for (const auto &event : page.value().events) {
        if (event.task_id != intent.task_id || event.payload.data.rfind(marker, 0) != 0) continue;
        if (event.payload.type == "ActionDispatchStarted") state.phase = ActionJournalPhase::DispatchStarted;
        if (event.payload.type == "ActionReceipt") state.phase = ActionJournalPhase::Receipt;
        if (event.payload.type == "ActionExecutionUncertain") state.phase = ActionJournalPhase::ExecutionUncertain;
        state.last_receipt = AppendReceipt{event.event_id, event.session_sequence, event.task_sequence, 0,
                                           Durability::ProcessCrash};
    }
    return state;
}

} // namespace mira
