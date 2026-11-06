#include "support/test.hpp"

#include <mira/action_journal.hpp>
#include <mira/task_recovery.hpp>

#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace mira;

class FaultStore final : public IEventStore {
  public:
    MemoryEventStore backing;
    std::string affected;
    bool reject = false;
    Durability achieved = Durability::Buffered;
    bool throw_append = false;
    bool bad_ack = false;
    std::function<Result<EventPage>(const EventQuery &)> reader;

    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (throw_append) {
            throw std::runtime_error("private store diagnostic");
        }
        if (request.payload.type == affected && reject) {
            Error error;
            error.code = ErrorCode::ResourceExhausted;
            error.domain = "mira.test";
            return error;
        }
        auto result = backing.append(request);
        // Explicit protocol acknowledgement simulation, not storage durability.
        if (result)
            result.value().achieved = request.required;
        if (result && bad_ack) {
            result.value().event_id = EventId::generate();
        }
        if (result && request.payload.type == affected) {
            result.value().achieved = achieved;
        }
        return result;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return backing.append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override {
        return reader ? reader(query) : backing.read(query);
    }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return backing.recover(options);
    }
    Result<void> flush(Durability durability) override { return backing.flush(durability); }
};

ActionIntent intent() {
    return {ActionId::generate(),
            RuntimeId::generate(),
            SessionId::generate(),
            TaskId::generate(),
            2,
            5,
            digest_string("back"),
            {"input", "primary", "session"}};
}

int full_intent_binding_and_input_validation() {
    FaultStore store;
    const auto action = intent();
    ActionJournal journal(action.runtime_id, store);
    MIRA_CHECK(journal.prepare(action));
    MIRA_CHECK(journal.prepare(action));
    MIRA_CHECK(store.backing.size() == 1);
    for (int field = 0; field < 9; ++field) {
        auto changed = action;
        switch (field) {
        case 0:
            changed.runtime_id = RuntimeId::generate();
            break;
        case 1:
            changed.session_id = SessionId::generate();
            break;
        case 2:
            changed.task_id = TaskId::generate();
            break;
        case 3:
            ++changed.task_epoch;
            break;
        case 4:
            ++changed.environment_epoch;
            break;
        case 5:
            changed.action_digest = digest_string("other");
            break;
        case 6:
            changed.target.type = "other";
            break;
        case 7:
            changed.target.id = "other";
            break;
        case 8:
            changed.target.scope = "other";
            break;
        }
        MIRA_CHECK(!journal.prepare(changed));
        MIRA_CHECK(!journal.dispatch_started(changed));
        MIRA_CHECK(!journal.receipt(changed, {}));
        MIRA_CHECK(!journal.execution_uncertain(changed, "unknown"));
        MIRA_CHECK(!journal.recover(changed));
        MIRA_CHECK(store.backing.size() == 1);
    }
    for (int field = 0; field < 7; ++field) {
        auto changed = action;
        changed.action_id = ActionId::generate();
        switch (field) {
        case 0:
            changed.action_id = {};
            break;
        case 1:
            changed.task_id = {};
            break;
        case 2:
            changed.session_id = {};
            break;
        case 3:
            changed.target.type = "";
            break;
        case 4:
            changed.target.id = "injected|field";
            break;
        case 5:
            changed.target.scope = "new\nline";
            break;
        case 6:
            changed.target.id = std::string(1025, 'x');
            break;
        }
        MIRA_CHECK(!journal.prepare(changed));
        MIRA_CHECK(store.backing.size() == 1);
    }
    MIRA_CHECK(journal.dispatch_started(action));
    MIRA_CHECK(!journal.execution_uncertain(action, std::string(4097, 'x')));
    MIRA_CHECK(!journal.execution_uncertain(action, "unsafe\nmessage"));
    MIRA_CHECK(journal.execution_uncertain(action, "legacy|tail is preserved"));
    ActionJournal restarted(action.runtime_id, store);
    auto state = restarted.recover(action);
    MIRA_CHECK(state && state.value().phase == ActionJournalPhase::ExecutionUncertain);
    auto page = store.read({action.session_id, std::nullopt, 100});
    MIRA_CHECK(page);
    CheckpointBuilder reducer;
    const auto built = reducer.build(std::nullopt, page.value().events, Timestamp::now());
    MIRA_CHECK(built.stats.malformed == 0);
    MIRA_CHECK(built.checkpoint.uncertain_side_effects.size() == 1);
    MIRA_CHECK(built.checkpoint.uncertain_side_effects.front().reason ==
               "legacy|tail is preserved");
    return 0;
}

int paginated_recovery_is_complete_and_bounded() {
    FaultStore store;
    auto action = intent();
    action.task_epoch = std::numeric_limits<std::uint64_t>::max();
    ActionJournal writer(action.runtime_id, store);
    MIRA_CHECK(writer.prepare(action));
    // Unrelated tasks consume scan budget too; unknown event types sharing
    // the action prefix may not masquerade as journal results.
    auto initial = store.backing.read({action.session_id, std::nullopt, 10});
    MIRA_CHECK(initial);
    const auto data = initial.value().events.front().payload.data;
    MIRA_CHECK(store.backing.append({EventId::generate(),
                                     action.runtime_id,
                                     action.session_id,
                                     TaskId::generate(),
                                     {"diagnostic", data, EventClass::Diagnostic}}));
    MIRA_CHECK(writer.dispatch_started(action));
    MIRA_CHECK(writer.receipt(action, ExecutionReceipt{}));
    auto last = store.backing.read({action.session_id, std::nullopt, 10});
    MIRA_CHECK(last);
    const auto last_id = last.value().events.back().event_id;
    MIRA_CHECK(store.backing.append({EventId::generate(),
                                     action.runtime_id,
                                     action.session_id,
                                     action.task_id,
                                     {"unrelated", data, EventClass::Diagnostic}}));
    int pages = 0;
    store.reader = [&](const EventQuery &query) {
        ++pages;
        auto one = query;
        one.limit = 1;
        return store.backing.read(one);
    };
    ActionJournal reader(action.runtime_id, store, {2, 5});
    auto recovered = reader.recover(action);
    MIRA_CHECK(recovered && recovered.value().phase == ActionJournalPhase::Receipt);
    MIRA_CHECK(recovered.value().last_receipt->event_id == last_id && pages == 5);
    ActionJournal limited(action.runtime_id, store, {2, 4});
    auto exhausted = limited.recover(action);
    MIRA_CHECK(!exhausted && exhausted.error().code == ErrorCode::ResourceExhausted);
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(store, checkpoints, {checkpoint_schema_current(), 4});
    const auto plan = planner.plan(action.task_id, action.session_id, Timestamp::now());
    MIRA_CHECK(!plan && plan.error().code == ErrorCode::ResourceExhausted);
    ActionJournal invalid(action.runtime_id, store, {0, 5});
    MIRA_CHECK(!invalid.recover(action));
    // Duplicate/non-progressing, empty-with-more and foreign-session pages.
    for (int scenario = 0; scenario < 3; ++scenario) {
        store.reader = [&](const EventQuery &) -> Result<EventPage> {
            if (scenario == 1) {
                return EventPage{{}, true};
            }
            auto page = store.backing.read({action.session_id, std::nullopt, 1});
            if (page && scenario == 2) {
                page.value().events.front().session_id = SessionId::generate();
            }
            return page;
        };
        ActionJournal bad(action.runtime_id, store, {1, 10});
        const auto result = bad.recover(action);
        MIRA_CHECK(!result && result.error().code == ErrorCode::DataLoss);
        RecoveryPlanner bad_planner(store, checkpoints);
        const auto bad_plan = bad_planner.plan(action.task_id, action.session_id, Timestamp::now());
        MIRA_CHECK(!bad_plan && bad_plan.error().code == ErrorCode::DataLoss);
    }
    return 0;
}

int malformed_and_conflicting_history_is_rejected() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        FaultStore store;
        const auto action = intent();
        ActionJournal writer(action.runtime_id, store);
        MIRA_CHECK(writer.prepare(action));
        MIRA_CHECK(writer.dispatch_started(action));
        store.reader = [&](const EventQuery &query) {
            auto page = store.backing.read(query);
            for (auto &event : page.value().events) {
                if (event.payload.type != "ActionDispatchStarted") {
                    continue;
                }
                if (scenario == 0) {
                    event.task_id = TaskId::generate();
                } else if (scenario == 1) {
                    event.runtime_id = RuntimeId::generate();
                } else if (scenario == 2) {
                    auto &data = event.payload.data;
                    const auto begin = data.find('|', data.find('|') + 1) + 1;
                    const auto end = data.find('|', begin);
                    data.replace(begin, end - begin, "18446744073709551616");
                } else if (scenario == 3) {
                    event.payload.data += "\n";
                } else if (scenario == 4) {
                    const auto digest = event.payload.data.find(action.action_digest.to_string());
                    event.payload.data.replace(digest, 64, std::string(64, 'z'));
                } else {
                    event.payload.type = "ActionReceipt";
                }
            }
            return page;
        };
        ActionJournal reader(action.runtime_id, store);
        const auto result = reader.recover(action);
        MIRA_CHECK(!result && result.error().code == ErrorCode::DataLoss);
        if (scenario >= 2 && scenario <= 4) {
            auto page = store.read({action.session_id, std::nullopt, 10});
            CheckpointBuilder reducer;
            const auto built = reducer.build(std::nullopt, page.value().events, Timestamp::now());
            MIRA_CHECK(!built.checkpoint.verification_pending);
            MIRA_CHECK(built.stats.malformed > 0);
        }
    }
    return 0;
}

int store_exceptions_and_bad_receipts_are_results() {
    for (const bool throws : {false, true}) {
        FaultStore store;
        auto action = intent();
        ActionJournal journal(action.runtime_id, store);
        store.throw_append = throws;
        store.bad_ack = !throws;
        auto result = journal.prepare(action);
        MIRA_CHECK(!result);
        MIRA_CHECK(result.error().code == (throws ? ErrorCode::Internal : ErrorCode::DataLoss));
        MIRA_CHECK(result.error().safe_message.find("private") == std::string::npos);
        MIRA_CHECK(!journal.dispatch_started(action));
        store.reader = [](const EventQuery &) -> Result<EventPage> {
            throw std::runtime_error("private read");
        };
        auto recovered = journal.recover(action);
        MIRA_CHECK(!recovered && recovered.error().code == ErrorCode::Internal);
    }
    return 0;
}

int acknowledgements_gate_phase_changes() {
    for (const std::string type :
         {"ActionPrepared", "ActionDispatchStarted", "ActionReceipt", "ActionExecutionUncertain"}) {
        for (const bool reject : {false, true}) {
            FaultStore store;
            const auto action = intent();
            ActionJournal journal(action.runtime_id, store);
            if (type != "ActionPrepared") {
                MIRA_CHECK(journal.prepare(action));
            }
            if (type == "ActionReceipt" || type == "ActionExecutionUncertain") {
                MIRA_CHECK(journal.dispatch_started(action));
            }
            store.affected = type;
            store.reject = reject;
            const auto transition = [&]() -> Result<AppendReceipt> {
                if (type == "ActionPrepared") {
                    return journal.prepare(action);
                }
                if (type == "ActionDispatchStarted") {
                    return journal.dispatch_started(action);
                }
                if (type == "ActionReceipt") {
                    return journal.receipt(action, ExecutionReceipt{});
                }
                return journal.execution_uncertain(action, "unknown");
            };
            const auto failed = transition();
            MIRA_CHECK(!failed);
            MIRA_CHECK(failed.error().code ==
                       (reject ? ErrorCode::ResourceExhausted : ErrorCode::DataLoss));
            if (type == "ActionPrepared") {
                MIRA_CHECK(!journal.dispatch_started(action));
            } else {
                const auto local = journal.recover(action);
                MIRA_CHECK(local);
                MIRA_CHECK(local.value().phase == (type == "ActionDispatchStarted"
                                                       ? ActionJournalPhase::Prepared
                                                       : ActionJournalPhase::DispatchStarted));
            }
            if (!reject && type == "ActionDispatchStarted") {
                // A failed acknowledgement is not a rollback of the event.
                ActionJournal restarted(action.runtime_id, store);
                const auto stored = restarted.recover(action);
                MIRA_CHECK(stored && stored.value().phase == ActionJournalPhase::DispatchStarted);
            }
            // A retry must contact the store again, not return the failed ack
            // from a prematurely advanced phase. PowerLoss is also sufficient.
            store.reject = false;
            store.achieved = Durability::PowerLoss;
            MIRA_CHECK(transition());
        }
    }
    return 0;
}

int rejects_invalid_durability() {
    FaultStore store;
    const auto action = intent();
    ActionJournal journal(action.runtime_id, store);
    store.affected = "ActionPrepared";
    store.achieved = static_cast<Durability>(255);
    const auto result = journal.prepare(action);
    MIRA_CHECK(!result && result.error().code == ErrorCode::DataLoss);
    return 0;
}

int durable_start_survives_store_reopen() {
    const auto action = intent();
    const auto root =
        std::filesystem::temp_directory_path() / ("mira-journal-" + action.action_id.to_string());
    struct Cleanup final {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};
    {
        FileEventStore store(root);
        MIRA_CHECK(store.recover({}));
        ActionJournal journal(action.runtime_id, store);
        MIRA_CHECK(journal.prepare(action));
        MIRA_CHECK(journal.dispatch_started(action));
    }
    FileEventStore reopened(root);
    MIRA_CHECK(reopened.recover({}));
    ActionJournal journal(action.runtime_id, reopened);
    const auto recovered = journal.recover(action);
    MIRA_CHECK(recovered && recovered.value().phase == ActionJournalPhase::DispatchStarted);
    MemoryCheckpointStore checkpoints;
    RecoveryPlanner planner(reopened, checkpoints);
    const auto recovery = planner.plan(action.task_id, action.session_id, Timestamp::now());
    MIRA_CHECK(recovery && recovery.value().checkpoint);
    MIRA_CHECK(recovery.value().checkpoint->verification_pending);
    MIRA_CHECK(recovery.value().action == RecoveryAction::ResumeVerifying);
    // Recovery only reads facts; it cannot dispatch real input.
    const auto page = reopened.read({action.session_id, std::nullopt, 100});
    MIRA_CHECK(page && page.value().events.size() == 2);
    return 0;
}
} // namespace

int main() {
    for (auto test :
         {full_intent_binding_and_input_validation, paginated_recovery_is_complete_and_bounded,
          malformed_and_conflicting_history_is_rejected,
          store_exceptions_and_bad_receipts_are_results}) {
        if (const int status = test(); status != 0) {
            return status;
        }
    }
    if (const int status = acknowledgements_gate_phase_changes(); status != 0) {
        return status;
    }
    if (const int status = rejects_invalid_durability(); status != 0) {
        return status;
    }
    return durable_start_survives_store_reopen();
}
