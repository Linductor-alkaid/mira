#include "sqlite_goal_store_internal.hpp"

namespace mira {
namespace {
using storage::bind_key;
using storage::bind_revision;
using storage::bind_text;
using storage::bind_time;
using storage::column_text;
using storage::column_time;
using storage::goal_error;
using storage::kMaxSqlInteger;
using storage::read_goal_plan;
using storage::require_goal_capacity;
using storage::Statement;
using storage::Transaction;
using storage::valid_key;
using storage::valid_time;

bool valid_occurrence(const GoalOccurrenceKey &key) {
    return valid_key(key.plan) && key.plan_revision > 0 && key.plan_revision < kMaxSqlInteger &&
           valid_time(key.scheduled_at);
}

bool bind_occurrence(Statement &statement, const GoalOccurrenceKey &key) {
    return bind_key(statement, key.plan) && bind_revision(statement, 4, key.plan_revision) &&
           bind_time(statement, 5, key.scheduled_at);
}

struct OccurrenceRow final {
    // Legacy receipts store the record time here; dispatched rows store preparation time.
    GoalTime recorded_at;
    std::optional<GoalOccurrenceDispatchReceipt> dispatch;
};

Result<std::optional<OccurrenceRow>> read_occurrence_row(sqlite3 *db,
                                                         const GoalOccurrenceKey &key) {
    Statement row(db, "SELECT recorded_at,dispatch_operation,dispatch_session,dispatch_task,"
                      " dispatched_at FROM recurring_occurrences WHERE tenant=?1 AND owner=?2"
                      " AND plan_id=?3 AND revision=?4 AND scheduled_at=?5");
    if (!row.valid() || !bind_occurrence(row, key))
        return goal_error(ErrorCode::Internal, "occurrence read preparation failed");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<OccurrenceRow>{};
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "occurrence read failed");
    const auto invalid_receipt = [] {
        return goal_error(ErrorCode::DataLoss, "occurrence receipt is invalid");
    };
    const auto recorded_at = column_time(row, 0);
    if (!recorded_at || *recorded_at < key.scheduled_at)
        return invalid_receipt();
    OccurrenceRow value{*recorded_at, {}};
    const bool has_operation = sqlite3_column_type(row.get(), 1) != SQLITE_NULL;
    const bool has_submission = sqlite3_column_type(row.get(), 2) != SQLITE_NULL;
    if (has_submission != (sqlite3_column_type(row.get(), 3) != SQLITE_NULL) ||
        has_submission != (sqlite3_column_type(row.get(), 4) != SQLITE_NULL) ||
        (has_submission && !has_operation))
        return invalid_receipt();
    if (has_operation) {
        const auto operation = OperationId::parse(column_text(row, 1));
        if (!operation || operation->is_nil())
            return invalid_receipt();
        value.dispatch = GoalOccurrenceDispatchReceipt{{key, *operation}, *recorded_at, {}};
    }
    if (has_submission) {
        const auto session = SessionId::parse(column_text(row, 2));
        const auto task = TaskId::parse(column_text(row, 3));
        const auto at = column_time(row, 4);
        if (!session || session->is_nil() || !task || task->is_nil() || !at || *at < *recorded_at)
            return invalid_receipt();
        value.dispatch->submission = GoalDispatchSubmission{*session, *task, *at};
    }
    return std::optional{value};
}

std::optional<GoalOccurrenceReceipt> completed(const OccurrenceRow &row,
                                               const GoalOccurrenceKey &key, bool replay) {
    if (!row.dispatch)
        return GoalOccurrenceReceipt{key, row.recorded_at, replay};
    if (!row.dispatch->submission)
        return {};
    return GoalOccurrenceReceipt{key, row.dispatch->submission->submitted_at, replay};
}

bool is_current(const std::optional<RecurringPlanRecord> &plan, const GoalOccurrenceKey &key) {
    return plan && plan->state == RecurringPlanState::Active &&
           plan->revision == key.plan_revision && plan->next_at == key.scheduled_at;
}

Result<std::optional<GoalTime>> next_occurrence(const GoalOccurrenceKey &key,
                                                const RecurringPlanDefinition &definition) {
    if (definition.calendar) {
        if (key.scheduled_at.time_since_epoch().count() == kMaxSqlInteger)
            return goal_error(ErrorCode::ResourceExhausted, "recurring plan time range reached");
        return next_goal_calendar_occurrence(*definition.calendar,
                                             key.scheduled_at + std::chrono::milliseconds{1});
    }
    if (key.scheduled_at.time_since_epoch().count() > kMaxSqlInteger - definition.interval.count())
        return goal_error(ErrorCode::ResourceExhausted, "recurring plan time range reached");
    return std::optional{key.scheduled_at + definition.interval};
}

Result<void> advance_occurrence(sqlite3 *db, const GoalOccurrenceKey &key,
                                const RecurringPlanDefinition &definition) {
    const auto next = next_occurrence(key, definition);
    if (!next)
        return next.error();
    Statement advance(db, "UPDATE recurring_plans SET next_at=?6 WHERE tenant=?1 AND owner=?2"
                          " AND id=?3 AND revision=?4 AND next_at=?5 AND state=0");
    if (!advance.valid() || !bind_occurrence(advance, key) ||
        !bind_time(advance, 6, next.value()) || sqlite3_step(advance.get()) != SQLITE_DONE ||
        sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "recurring plan advance failed");
    return {};
}

Result<void> insert_occurrence(sqlite3 *db, std::size_t capacity, const GoalOccurrenceKey &key,
                               GoalTime recorded_at, std::optional<OperationId> operation = {}) {
    if (auto room =
            require_goal_capacity(db, "SELECT count(*) FROM recurring_occurrences", capacity);
        !room)
        return room.error();
    Statement insert(db,
                     "INSERT INTO recurring_occurrences(tenant,owner,plan_id,revision,"
                     " scheduled_at,recorded_at,dispatch_operation) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    if (!insert.valid() || !bind_occurrence(insert, key) || !bind_time(insert, 6, recorded_at) ||
        !(operation ? bind_text(insert, 7, operation->to_string())
                    : sqlite3_bind_null(insert.get(), 7) == SQLITE_OK) ||
        sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "occurrence write failed");
    return {};
}

Result<GoalOccurrenceReceipt> record_occurrence_row(sqlite3 *db, std::size_t capacity,
                                                    const GoalOccurrenceKey &key,
                                                    GoalTime recorded_at) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "occurrence transaction unavailable");
    const auto stored = read_occurrence_row(db, key);
    if (!stored)
        return stored.error();
    if (stored.value()) {
        if (auto receipt = completed(*stored.value(), key, true))
            return *receipt;
        return goal_error(ErrorCode::InvalidState, "occurrence dispatch is already prepared");
    }
    const auto plan = read_goal_plan(db, key.plan);
    if (!plan)
        return plan.error();
    if (!is_current(plan.value(), key))
        return goal_error(ErrorCode::InvalidState,
                          "occurrence does not match the current due plan");
    if (auto inserted = insert_occurrence(db, capacity, key, recorded_at); !inserted)
        return inserted.error();
    if (auto advanced = advance_occurrence(db, key, *plan.value()->definition); !advanced)
        return advanced.error();
    if (auto saved = transaction.commit(); !saved)
        return saved.error();
    return GoalOccurrenceReceipt{key, recorded_at, false};
}

Result<GoalOccurrenceDispatchReceipt> prepare_occurrence(sqlite3 *db, std::size_t capacity,
                                                         const GoalOccurrenceKey &key,
                                                         GoalTime cutoff) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "occurrence dispatch transaction unavailable");
    const auto plan = read_goal_plan(db, key.plan);
    if (!plan)
        return plan.error();
    if (!is_current(plan.value(), key))
        return goal_error(ErrorCode::InvalidState, "occurrence no longer matches the due plan");
    const auto stored = read_occurrence_row(db, key);
    if (!stored)
        return stored.error();
    if (stored.value()) {
        if (stored.value()->dispatch && !stored.value()->dispatch->submission)
            return *stored.value()->dispatch;
        return goal_error(ErrorCode::InvalidState, "occurrence is already recorded");
    }
    if (auto next = next_occurrence(key, *plan.value()->definition); !next)
        return next.error();
    const auto operation = OperationId::generate();
    if (auto inserted = insert_occurrence(db, capacity, key, cutoff, operation); !inserted)
        return inserted.error();
    if (auto saved = transaction.commit(); !saved)
        return saved.error();
    return GoalOccurrenceDispatchReceipt{{key, operation}, cutoff, {}};
}

Result<GoalOccurrenceDispatchReceipt> record_submission(sqlite3 *db,
                                                        const GoalOccurrenceDispatchKey &key,
                                                        const GoalDispatchSubmission &submission) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "occurrence submission transaction unavailable");
    const auto stored = read_occurrence_row(db, key.occurrence);
    if (!stored)
        return stored.error();
    if (!stored.value() || !stored.value()->dispatch || stored.value()->dispatch->key != key)
        return goal_error(ErrorCode::NotFound, "occurrence dispatch identity is unavailable");
    auto receipt = *stored.value()->dispatch;
    if (receipt.submission) {
        if (*receipt.submission != submission)
            return goal_error(ErrorCode::AlreadyExists, "occurrence submission conflicts");
        return receipt;
    }
    if (submission.submitted_at < receipt.prepared_at)
        return goal_error(ErrorCode::InvalidArgument, "occurrence submission predates preparation");
    const auto plan = read_goal_plan(db, key.occurrence.plan);
    if (!plan)
        return plan.error();
    if (is_current(plan.value(), key.occurrence)) {
        if (auto advanced = advance_occurrence(db, key.occurrence, *plan.value()->definition);
            !advanced)
            return advanced.error();
    }
    Statement update(db, "UPDATE recurring_occurrences SET dispatch_session=?6,dispatch_task=?7,"
                         " dispatched_at=?8 WHERE tenant=?1 AND owner=?2 AND plan_id=?3"
                         " AND revision=?4 AND scheduled_at=?5 AND dispatched_at IS NULL");
    if (!update.valid() || !bind_occurrence(update, key.occurrence) ||
        !bind_text(update, 6, submission.session.to_string()) ||
        !bind_text(update, 7, submission.task.to_string()) ||
        !bind_time(update, 8, submission.submitted_at) ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "occurrence submission write failed");
    if (auto saved = transaction.commit(); !saved)
        return saved.error();
    receipt.submission = submission;
    return receipt;
}
} // namespace

Result<GoalOccurrenceReceipt> SqliteGoalStore::record_occurrence(const GoalOccurrenceKey &key,
                                                                 GoalTime recorded_at) {
    if (!valid_occurrence(key) || recorded_at < key.scheduled_at)
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence recording");
    return impl_->run<GoalOccurrenceReceipt>(
        {key.plan.owner, GoalStoreResource::RecurringPlan, GoalStoreOperation::RecordOccurrence,
         key.plan.id},
        [key, recorded_at, capacity = impl_->options.max_occurrences](sqlite3 *db) {
            return record_occurrence_row(db, capacity, key, recorded_at);
        });
}

Result<std::optional<GoalOccurrenceReceipt>>
SqliteGoalStore::read_occurrence(const GoalOccurrenceKey &key) const {
    if (!valid_occurrence(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence key");
    return impl_->run<std::optional<GoalOccurrenceReceipt>>(
        {key.plan.owner, GoalStoreResource::RecurringPlan, GoalStoreOperation::ReadOccurrence,
         key.plan.id},
        [key](sqlite3 *db) -> Result<std::optional<GoalOccurrenceReceipt>> {
            const auto stored = read_occurrence_row(db, key);
            if (!stored)
                return stored.error();
            return stored.value() ? completed(*stored.value(), key, false) : std::nullopt;
        });
}

Result<GoalOccurrenceDispatchReceipt>
SqliteGoalStore::prepare_occurrence_dispatch(const GoalOccurrenceKey &key, GoalTime cutoff) {
    if (!valid_occurrence(key) || cutoff < key.scheduled_at)
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence dispatch preparation");
    return impl_->run<GoalOccurrenceDispatchReceipt>(
        {key.plan.owner, GoalStoreResource::RecurringPlan, GoalStoreOperation::PrepareDispatch,
         key.plan.id},
        [key, cutoff, capacity = impl_->options.max_occurrences](sqlite3 *db) {
            return prepare_occurrence(db, capacity, key, cutoff);
        });
}

Result<std::optional<GoalOccurrenceDispatchReceipt>>
SqliteGoalStore::read_occurrence_dispatch(const GoalOccurrenceKey &key) const {
    if (!valid_occurrence(key))
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence dispatch key");
    return impl_->run<std::optional<GoalOccurrenceDispatchReceipt>>(
        {key.plan.owner, GoalStoreResource::RecurringPlan, GoalStoreOperation::ReadDispatch,
         key.plan.id},
        [key](sqlite3 *db) -> Result<std::optional<GoalOccurrenceDispatchReceipt>> {
            const auto stored = read_occurrence_row(db, key);
            if (!stored)
                return stored.error();
            return stored.value() ? stored.value()->dispatch : std::nullopt;
        });
}

Result<GoalOccurrenceDispatchReceipt>
SqliteGoalStore::record_occurrence_submission(const GoalOccurrenceDispatchKey &key,
                                              GoalDispatchSubmission submission) {
    if (!valid_occurrence(key.occurrence) || key.operation.is_nil() ||
        submission.session.is_nil() || submission.task.is_nil() ||
        !valid_time(submission.submitted_at))
        return goal_error(ErrorCode::InvalidArgument, "invalid occurrence submission");
    return impl_->run<GoalOccurrenceDispatchReceipt>(
        {key.occurrence.plan.owner, GoalStoreResource::RecurringPlan,
         GoalStoreOperation::RecordDispatchSubmission, key.occurrence.plan.id},
        [key, submission](sqlite3 *db) { return record_submission(db, key, submission); });
}
} // namespace mira
