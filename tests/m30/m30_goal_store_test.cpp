#include "support/test.hpp"

#include <mira/content_markers.hpp>
#include <mira/sqlite_goal_store.hpp>

#include <kairo/executor.hpp>

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
} // namespace

int main() {
    if (const int result = bounded_due_pages_and_exact_owner(); result != 0)
        return result;
    return recover_states_quota_and_erasure();
}
