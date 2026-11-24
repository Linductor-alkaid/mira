#include "support/test.hpp"

#include <mira/content_markers.hpp>
#include <mira/sqlite_goal_store.hpp>

#include <kairo/executor.hpp>
#include <sqlite3.h>

#include <cstdlib>

namespace {
using namespace mira;
using namespace std::chrono_literals;

struct GoalFixture final {
    GoalFixture(std::size_t total, std::size_t per_owner) {
        kairo::ExecutorConfig executor_options;
        executor_options.min_threads = 2;
        executor_options.max_threads = 2;
        if (!executor.initialize(executor_options))
            std::abort();
        root =
            std::filesystem::temp_directory_path() / ("mira-goal-" + Id128::generate().to_string());
        std::filesystem::create_directories(root);
        options.path = root / "goals.db";
        options.max_goals = total;
        options.max_pending_per_owner = per_owner;
    }

    ~GoalFixture() {
        store.reset();
        (void)executor.shutdown(true);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    bool reopen() {
        if (store && !store->close())
            return false;
        store.reset();
        auto opened = SqliteGoalStore::open(executor, options);
        if (!opened)
            return false;
        store = std::move(opened).value();
        return true;
    }

    bool update_database(const char *sql) {
        if (store && !store->close())
            return false;
        store.reset();
        sqlite3 *raw = nullptr;
        const bool opened = sqlite3_open(options.path.string().c_str(), &raw) == SQLITE_OK;
        const bool changed =
            opened && sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(raw);
        return changed;
    }

    kairo::Executor executor;
    std::filesystem::path root;
    SqliteGoalStoreOptions options;
    std::unique_ptr<SqliteGoalStore> store;
    const GoalOwner owner{TenantId::generate(), UserId::generate()};
    const GoalTime now =
        std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
};

GoalDefinition commitment(GoalTime cutoff, std::string title) {
    return {GoalKind::Commitment, std::move(title), EventId::generate(),
            cutoff - 2h,          cutoff,           cutoff + 2h};
}

int bounded_due_pages_and_exact_owner() {
    GoalFixture fixture(9, 5);
    MIRA_CHECK(fixture.reopen());
    const auto &owner = fixture.owner;
    auto &store = *fixture.store;
    const GoalKey first{owner, Id128::generate()};
    const GoalKey second{owner, Id128::generate()};
    const GoalKey later{owner, Id128::generate()};
    const GoalKey without_date{owner, Id128::generate()};
    const GoalKey past_expiry{owner, Id128::generate()};
    const GoalKey other_user{{owner.tenant, UserId::generate()}, first.id};
    const GoalKey other_tenant{{TenantId::generate(), owner.user}, first.id};
    const auto same_instant = fixture.now - 15min;
    const auto report = commitment(same_instant, "Review the draft");
    MIRA_CHECK(store.create(first, report));
    const auto replay = store.create(first, report);
    MIRA_CHECK(replay && replay.value().revision == 1 && replay.value().definition == report);
    auto different = report;
    different.title = "Different draft";
    MIRA_CHECK(!store.create(first, different));
    MIRA_CHECK(store.create(second, commitment(same_instant, "Send the notes")));
    MIRA_CHECK(store.create(later, commitment(fixture.now + 15min, "Check the final")));
    const GoalDefinition task{
        GoalKind::Task, "Audit references", EventId::generate(), fixture.now, {}, {}};
    MIRA_CHECK(store.create(without_date, task));
    MIRA_CHECK(store.create(past_expiry, commitment(fixture.now - 3h, "Was due yesterday")));
    MIRA_CHECK(!store.read(other_user).value() && !store.read(other_tenant).value());
    MIRA_CHECK(store.create(other_user, commitment(same_instant, "Other user's task")));
    MIRA_CHECK(store.create(other_tenant, commitment(same_instant, "Other tenant's task")));

    GoalDueQuery query{owner, fixture.now, 1, {}};
    auto page = store.due(query);
    const bool first_by_id = first.id.to_string() < second.id.to_string();
    MIRA_CHECK(page && page.value().records.size() == 1 && page.value().next);
    MIRA_CHECK(page.value().records.front().key == (first_by_id ? first : second));
    query.after = page.value().next;
    page = store.due(query);
    MIRA_CHECK(page && page.value().records.size() == 1 && !page.value().next);
    MIRA_CHECK(page.value().records.front().key == (first_by_id ? second : first));
    query.owner = other_user.owner;
    query.after.reset();
    MIRA_CHECK(store.due(query).value().records.front().key == other_user);
    query.owner = other_tenant.owner;
    MIRA_CHECK(store.due(query).value().records.front().key == other_tenant);
    query.owner = owner;
    query.cutoff = fixture.now + 15min;
    query.limit = 16;
    const auto extended = store.due(query);
    MIRA_CHECK(extended && extended.value().records.size() == 3 &&
               extended.value().records.back().key == later);
    auto pending = store.list({owner, GoalState::Pending, 4, {}});
    MIRA_CHECK(pending && pending.value().records.size() == 4 && pending.value().next);
    const auto last = store.list({owner, GoalState::Pending, 4, pending.value().next});
    MIRA_CHECK(last && last.value().records.size() == 1 && !last.value().next);
    const auto foreign = store.list({other_tenant.owner, GoalState::Pending, 8, {}});
    MIRA_CHECK(foreign && foreign.value().records.size() == 1 &&
               foreign.value().records.front().key == other_tenant);
    return 0;
}

int recover_states_quota_and_erasure() {
    GoalFixture fixture(4, 2);
    MIRA_CHECK(fixture.reopen());
    const GoalKey done{fixture.owner, Id128::generate()};
    const GoalKey retiring{fixture.owner, Id128::generate()};
    const GoalKey later{fixture.owner, Id128::generate()};
    const GoalKey preempted{fixture.owner, Id128::generate()};
    const auto goal = commitment(fixture.now, "Send the report");
    auto forbidden = goal;
    forbidden.title = default_forbidden_content_markers().front();
    const auto refused = fixture.store->create(done, forbidden);
    MIRA_CHECK(!refused && refused.error().code == ErrorCode::SafetyRejected);
    auto missing_due = goal;
    missing_due.due_at.reset();
    MIRA_CHECK(!fixture.store->create(done, missing_due));
    MIRA_CHECK(fixture.store->create(done, goal) && fixture.store->create(retiring, goal));
    const auto full = fixture.store->create(later, goal);
    MIRA_CHECK(!full && full.error().code == ErrorCode::ResourceExhausted);

    const GoalResolution fulfilled{GoalState::Fulfilled, EventId::generate(), fixture.now};
    const GoalResolution cancelled{GoalState::Cancelled, EventId::generate(), fixture.now};
    auto winner =
        fixture.executor.submit_auto([&] { return fixture.store->settle(done, 1, fulfilled); });
    auto contender =
        fixture.executor.submit_auto([&] { return fixture.store->settle(done, 1, cancelled); });
    const auto first = winner.get();
    const auto second = contender.get();
    MIRA_CHECK(first.has_value() != second.has_value());
    const auto resolution = first ? fulfilled : cancelled;
    MIRA_CHECK(fixture.store->create(later, goal));

    GoalResolution expired{GoalState::Expired, EventId::generate(), fixture.now};
    MIRA_CHECK(!fixture.store->settle(retiring, 1, expired));
    expired.at = *goal.expires_at;
    auto terminal = fixture.store->settle(retiring, 1, expired);
    MIRA_CHECK(terminal && terminal.value().state == GoalState::Expired &&
               terminal.value().resolution == expired);
    MIRA_CHECK(fixture.store->erase(preempted));
    MIRA_CHECK(fixture.store->erase(retiring) && fixture.store->erase(retiring));
    MIRA_CHECK(fixture.reopen());

    auto read = fixture.store->read(done);
    MIRA_CHECK(read && read.value() && read.value()->revision == 2 &&
               read.value()->resolution == resolution);
    const auto replay = fixture.store->settle(done, 1, resolution);
    MIRA_CHECK(replay && replay.value().revision == 2);
    MIRA_CHECK(!fixture.store->settle(done, 2, resolution));
    for (const auto &key : {retiring, preempted}) {
        auto erased = fixture.store->read(key);
        MIRA_CHECK(erased && erased.value() && erased.value()->state == GoalState::Erased &&
                   !erased.value()->definition && !erased.value()->resolution);
        MIRA_CHECK(!fixture.store->create(key, goal));
        MIRA_CHECK(!fixture.store->settle(key, 1, cancelled));
    }
    const auto next = fixture.store->due({fixture.owner, fixture.now, 8, {}});
    MIRA_CHECK(next && next.value().records.size() == 1 &&
               next.value().records.front().key == later);
    const auto forgotten = fixture.store->list({fixture.owner, GoalState::Erased, 8, {}});
    MIRA_CHECK(forgotten && forgotten.value().records.size() == 2);
    const GoalKey blocked{fixture.owner, Id128::generate()};
    MIRA_CHECK(!fixture.store->create(blocked, goal) && !fixture.store->erase(blocked));
    MIRA_CHECK(fixture.store->close());
    MIRA_CHECK(!fixture.store->read(done));
    return 0;
}

int recurring_plan_versions_and_restart() {
    GoalFixture fixture(6, 3);
    fixture.options.max_plans = 3;
    fixture.options.max_active_plans_per_owner = 2;
    MIRA_CHECK(fixture.reopen());
    const GoalKey plan{fixture.owner, Id128::generate()};
    const GoalKey peer{fixture.owner, Id128::generate()};
    const GoalKey foreign{{TenantId::generate(), fixture.owner.user}, plan.id};
    const auto task = commitment(fixture.now, "A one-time goal with the same ID");
    MIRA_CHECK(fixture.store->create(plan, task));
    MIRA_CHECK(
        fixture.update_database("DROP TABLE recurring_occurrences; DROP TABLE recurring_plans;"
                                " DROP TABLE goal_corrections; DROP TABLE goal_reminders;"
                                " UPDATE store_meta SET value='1.0' WHERE key='schema_version';"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(fixture.store->read(plan).value()->definition == task);
    const auto first_at = fixture.now - 2h;
    const RecurringPlanDefinition definition{"Send weekly notes", EventId::generate(), first_at,
                                             15min};
    auto invalid = definition;
    invalid.interval = 0ms;
    MIRA_CHECK(!fixture.store->create_plan(plan, invalid));
    const auto initial = fixture.store->create_plan(plan, definition);
    MIRA_CHECK(initial && initial.value().revision == 1 && initial.value().next_at == first_at);
    MIRA_CHECK(fixture.store->create_plan(peer, definition));
    MIRA_CHECK(!fixture.store->read_plan(foreign).value());
    MIRA_CHECK(fixture.store->create_plan(foreign, definition));
    RecurringDueQuery due{fixture.owner, fixture.now, 1, {}};
    const auto page = fixture.store->due_plans(due);
    MIRA_CHECK(page && page.value().records.size() == 1 && page.value().next);
    due.after = page.value().next;
    const auto next_page = fixture.store->due_plans(due);
    MIRA_CHECK(next_page && next_page.value().records.size() == 1 && !next_page.value().next &&
               next_page.value().records.front().key != page.value().records.front().key);

    const GoalOccurrenceKey occurrence{plan, 1, first_at};
    MIRA_CHECK(!fixture.store->record_occurrence(occurrence, first_at - 1ms));
    const auto same_time = [&] {
        return fixture.store->record_occurrence(occurrence, fixture.now);
    };
    auto first = fixture.executor.submit_auto(same_time);
    auto second = fixture.executor.submit_auto(same_time);
    const auto a = first.get();
    const auto b = second.get();
    MIRA_CHECK(a && b && a.value().idempotent_replay != b.value().idempotent_replay);
    MIRA_CHECK(fixture.reopen());
    const auto replay = fixture.store->record_occurrence(occurrence, fixture.now + 1min);
    MIRA_CHECK(replay && replay.value().idempotent_replay &&
               replay.value().recorded_at == fixture.now);
    auto advanced = fixture.store->read_plan(plan);
    MIRA_CHECK(advanced && advanced.value() && advanced.value()->next_at == first_at + 15min);
    const GoalOccurrenceKey missed_step{plan, 1, first_at + 30min};
    MIRA_CHECK(!fixture.store->record_occurrence(missed_step, fixture.now));
    const GoalOccurrenceKey pending_old{plan, 1, first_at + 15min};
    MIRA_CHECK(fixture.store->record_occurrence(pending_old, fixture.now));

    auto new_definition = definition;
    new_definition.source_event = EventId::generate();
    new_definition.interval = 1h;
    const auto revised = fixture.store->replace_plan(plan, 1, new_definition);
    MIRA_CHECK(revised && revised.value().revision == 2 && revised.value().next_at == first_at);
    MIRA_CHECK(!fixture.store->record_occurrence(missed_step, fixture.now));
    const GoalOccurrenceKey new_occurrence{plan, 2, first_at};
    const auto registered = fixture.store->record_occurrence(new_occurrence, fixture.now);
    MIRA_CHECK(registered && !registered.value().idempotent_replay);
    const auto revise_replay = fixture.store->replace_plan(plan, 1, new_definition);
    MIRA_CHECK(revise_replay && revise_replay.value().next_at == first_at + 1h);

    const RecurringPlanCancellation cancellation{EventId::generate(), fixture.now};
    const auto cancelled = fixture.store->cancel_plan(plan, 2, cancellation);
    MIRA_CHECK(cancelled && cancelled.value().state == RecurringPlanState::Cancelled &&
               !cancelled.value().next_at && cancelled.value().cancellation == cancellation);
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(fixture.store->cancel_plan(plan, 2, cancellation).value().revision == 3);
    MIRA_CHECK(
        fixture.store->record_occurrence(new_occurrence, fixture.now).value().idempotent_replay);
    const GoalOccurrenceKey cancelled_occurrence{plan, 2, first_at + 1h};
    MIRA_CHECK(!fixture.store->record_occurrence(cancelled_occurrence, fixture.now));
    const auto remaining = fixture.store->due_plans({fixture.owner, fixture.now, 32, {}});
    MIRA_CHECK(remaining && remaining.value().records.size() == 1 &&
               remaining.value().records.front().key == peer);

    MIRA_CHECK(fixture.store->erase_plan(plan) && fixture.store->erase_plan(plan));
    MIRA_CHECK(fixture.reopen());
    const auto erased = fixture.store->read_plan(plan);
    MIRA_CHECK(erased && erased.value() && erased.value()->state == RecurringPlanState::Erased &&
               !erased.value()->definition && !erased.value()->cancellation);
    MIRA_CHECK(!fixture.store->read_occurrence(occurrence).value() &&
               !fixture.store->read_occurrence(new_occurrence).value());
    MIRA_CHECK(!fixture.store->record_occurrence(new_occurrence, fixture.now));
    MIRA_CHECK(!fixture.store->create_plan(plan, definition));
    MIRA_CHECK(fixture.store->read_plan(foreign).value()->next_at == first_at);
    MIRA_CHECK(fixture.store->read(plan).value()->definition == task);
    return 0;
}

int occurrence_rollback_and_quota() {
    GoalFixture fixture(4, 2);
    fixture.options.max_plans = 2;
    fixture.options.max_active_plans_per_owner = 1;
    fixture.options.max_occurrences = 1;
    MIRA_CHECK(fixture.reopen());
    const GoalKey plan{fixture.owner, Id128::generate()};
    const GoalKey next_plan{fixture.owner, Id128::generate()};
    const auto first_at = fixture.now - 2h;
    const RecurringPlanDefinition definition{"Check status", EventId::generate(), first_at, 1h};
    MIRA_CHECK(fixture.store->create_plan(plan, definition));
    const auto denied = fixture.store->create_plan(next_plan, definition);
    MIRA_CHECK(!denied && denied.error().code == ErrorCode::ResourceExhausted);
    const GoalOccurrenceKey occurrence{plan, 1, first_at};
    MIRA_CHECK(fixture.update_database(
        "CREATE TRIGGER fail_advance BEFORE UPDATE OF next_at ON recurring_plans"
        " BEGIN SELECT RAISE(ABORT, 'fixture rejects advance'); END;"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(!fixture.store->record_occurrence(occurrence, fixture.now));
    MIRA_CHECK(!fixture.store->read_occurrence(occurrence).value());
    MIRA_CHECK(fixture.store->read_plan(plan).value()->next_at == first_at);
    MIRA_CHECK(fixture.update_database("DROP TRIGGER fail_advance;"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(fixture.store->record_occurrence(occurrence, fixture.now));
    const GoalOccurrenceKey after{plan, 1, first_at + 1h};
    const auto full = fixture.store->record_occurrence(after, fixture.now);
    MIRA_CHECK(!full && full.error().code == ErrorCode::ResourceExhausted);
    MIRA_CHECK(fixture.store->read_plan(plan).value()->next_at == after.scheduled_at);
    MIRA_CHECK(fixture.store->record_occurrence(occurrence, fixture.now).value().idempotent_replay);
    MIRA_CHECK(fixture.store->erase_plan(plan));
    MIRA_CHECK(fixture.store->create_plan(next_plan, definition));
    MIRA_CHECK(fixture.store->record_occurrence({next_plan, 1, first_at}, fixture.now));
    return 0;
}

int corrections_and_reminders_survive_restart() {
    GoalFixture fixture(4, 3);
    fixture.options.max_corrections_per_goal = 2;
    fixture.options.max_reminders_per_goal = 2;
    MIRA_CHECK(fixture.reopen());
    const GoalKey goal{fixture.owner, Id128::generate()};
    const GoalKey foreign{{TenantId::generate(), fixture.owner.user}, goal.id};
    const auto original = commitment(fixture.now, "Original deadline");
    MIRA_CHECK(fixture.store->create(goal, original));
    MIRA_CHECK(fixture.store->create(foreign, original));
    const RecurringPlanDefinition plan{"Keep the plan", EventId::generate(), fixture.now, 1h};
    MIRA_CHECK(fixture.store->create_plan(goal, plan));
    MIRA_CHECK(fixture.update_database(
        "DROP TABLE goal_corrections; DROP TABLE goal_reminders;"
        " ALTER TABLE goals RENAME TO fixture_goal_source;"
        " CREATE TABLE goals(tenant TEXT NOT NULL,owner TEXT NOT NULL,id TEXT NOT NULL,"
        " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
        " revision INTEGER NOT NULL CHECK(revision BETWEEN 1 AND 3),"
        " kind INTEGER,title TEXT,source_event TEXT,created_at INTEGER,due_at INTEGER,"
        " expires_at INTEGER,resolution_event TEXT,resolved_at INTEGER,PRIMARY "
        "KEY(tenant,owner,id));"
        " INSERT INTO goals SELECT * FROM fixture_goal_source; DROP TABLE fixture_goal_source;"
        " CREATE INDEX goals_due ON goals(tenant,owner,due_at,id) WHERE state=0;"
        " CREATE INDEX goals_by_owner_state ON goals(tenant,owner,state,id);"
        " UPDATE store_meta SET value='1.1' WHERE key='schema_version';"
        " CREATE TRIGGER block_schema_version BEFORE UPDATE ON store_meta"
        " WHEN NEW.key='schema_version' BEGIN SELECT RAISE(ABORT, 'fixture rejects migration'); "
        "END;"));
    MIRA_CHECK(!fixture.reopen());
    MIRA_CHECK(fixture.update_database("DROP TRIGGER block_schema_version;"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(fixture.store->read_plan(goal).value()->definition == plan);
    MIRA_CHECK(fixture.store->read(goal).value()->definition == original);

    const GoalReminder reminder{EventId::generate(), fixture.now};
    const auto notify = [&] { return fixture.store->record_reminder(goal, 1, reminder); };
    auto one = fixture.executor.submit_auto(notify);
    auto two = fixture.executor.submit_auto(notify);
    const auto first = one.get();
    const auto repeated = two.get();
    MIRA_CHECK(first && repeated &&
               first.value().idempotent_replay != repeated.value().idempotent_replay);
    MIRA_CHECK(fixture.store->read(goal).value()->reminder_count == 1);
    MIRA_CHECK(fixture.store->read(foreign).value()->reminder_count == 0);
    MIRA_CHECK(!fixture.store->record_reminder(goal, 1, {reminder.evidence, fixture.now + 1ms}));

    auto modified = original;
    modified.title = "Revised deadline";
    modified.due_at = fixture.now + 15min;
    modified.source_event = EventId::generate();
    MIRA_CHECK(
        fixture.update_database("CREATE TRIGGER block_correction BEFORE UPDATE OF title ON goals"
                                " BEGIN SELECT RAISE(ABORT, 'fixture rejects correction'); END;"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(!fixture.store->correct(goal, 1, modified));
    MIRA_CHECK(fixture.store->correction_history(goal).value().empty());
    MIRA_CHECK(fixture.store->read(goal).value()->definition == original);
    MIRA_CHECK(fixture.update_database("DROP TRIGGER block_correction;"));
    MIRA_CHECK(fixture.reopen());
    const auto changed = fixture.store->correct(goal, 1, modified);
    MIRA_CHECK(changed && changed.value().revision == 2 && changed.value().reminder_count == 1);
    MIRA_CHECK(fixture.store->correct(goal, 1, modified).value().revision == 2);
    MIRA_CHECK(fixture.store->due({fixture.owner, fixture.now, 8, {}}).value().records.empty());
    MIRA_CHECK(!fixture.store->correct(goal, 2, original));
    MIRA_CHECK(fixture.store->correction_history(foreign).value().empty());
    auto history = fixture.store->correction_history(goal);
    MIRA_CHECK(history && history.value().size() == 1 && history.value().front().revision == 1 &&
               history.value().front().definition == original);

    const GoalReminder new_reminder{EventId::generate(), fixture.now + 5min};
    MIRA_CHECK(!fixture.store->record_reminder(goal, 1, new_reminder));
    MIRA_CHECK(fixture.store->record_reminder(goal, 1, reminder).value().idempotent_replay);
    MIRA_CHECK(!fixture.store->record_reminder(goal, 2, reminder));
    MIRA_CHECK(fixture.store->record_reminder(goal, 2, new_reminder));
    const auto full = fixture.store->record_reminder(goal, 2, {EventId::generate(), fixture.now});
    MIRA_CHECK(!full && full.error().code == ErrorCode::ResourceExhausted);

    auto latest = modified;
    latest.title = "Final deadline";
    latest.source_event = EventId::generate();
    MIRA_CHECK(fixture.store->correct(goal, 2, latest).value().revision == 3);
    auto more = latest;
    more.source_event = EventId::generate();
    const auto quota = fixture.store->correct(goal, 3, more);
    MIRA_CHECK(!quota && quota.error().code == ErrorCode::ResourceExhausted);
    const GoalResolution complete{GoalState::Fulfilled, EventId::generate(), fixture.now + 30min};
    MIRA_CHECK(!fixture.store->settle(goal, 1, complete));
    MIRA_CHECK(fixture.store->settle(goal, 3, complete).value().revision == 4);
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(fixture.store->settle(goal, 3, complete).value().reminder_count == 2);
    MIRA_CHECK(fixture.store->record_reminder(goal, 2, new_reminder).value().idempotent_replay);
    MIRA_CHECK(!fixture.store->correct(goal, 4, more));

    MIRA_CHECK(fixture.update_database(
        "CREATE TRIGGER block_reminder_erasure BEFORE DELETE ON goal_reminders"
        " BEGIN SELECT RAISE(ABORT, 'fixture rejects erasure'); END;"));
    MIRA_CHECK(fixture.reopen());
    MIRA_CHECK(!fixture.store->erase(goal));
    MIRA_CHECK(fixture.store->read(goal).value()->reminder_count == 2 &&
               fixture.store->correction_history(goal).value().size() == 2);
    MIRA_CHECK(fixture.update_database("DROP TRIGGER block_reminder_erasure;"));
    MIRA_CHECK(fixture.reopen() && fixture.store->erase(goal) && fixture.reopen());
    MIRA_CHECK(fixture.store->read(goal).value()->state == GoalState::Erased &&
               fixture.store->read(goal).value()->reminder_count == 0);
    MIRA_CHECK(fixture.store->correction_history(goal).value().empty());
    MIRA_CHECK(!fixture.store->record_reminder(goal, 1, reminder));
    MIRA_CHECK(!fixture.store->create(goal, original));
    MIRA_CHECK(fixture.store->read(foreign).value()->definition == original);
    MIRA_CHECK(fixture.store->read_plan(goal).value()->definition == plan);
    return 0;
}
} // namespace

int main() {
    if (const int result = bounded_due_pages_and_exact_owner(); result != 0)
        return result;
    if (const int result = recover_states_quota_and_erasure(); result != 0)
        return result;
    if (const int result = recurring_plan_versions_and_restart(); result != 0)
        return result;
    if (const int result = occurrence_rollback_and_quota(); result != 0)
        return result;
    return corrections_and_reminders_survive_restart();
}
