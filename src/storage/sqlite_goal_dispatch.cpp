#include "sqlite_goal_store_internal.hpp"

#include <limits>

namespace mira {
namespace {
using storage::bind_key;
using storage::bind_text;
using storage::bind_time;
using storage::column_text;
using storage::column_time;
using storage::goal_error;
using storage::Statement;
using storage::Transaction;
using storage::valid_key;
using storage::valid_time;

bool valid_dispatch_revision(std::uint64_t revision) {
    return revision > 0 &&
           revision <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

Result<std::optional<GoalDispatchReceipt>> read_dispatch_row(sqlite3 *db, const GoalKey &goal) {
    Statement row(db, "SELECT revision,operation,prepared_at,session,task,submitted_at"
                      " FROM goal_dispatches WHERE tenant=?1 AND owner=?2 AND goal_id=?3");
    if (!row.valid() || !bind_key(row, goal))
        return goal_error(ErrorCode::Internal, "goal dispatch lookup preparation failed");
    const auto step = sqlite3_step(row.get());
    if (step == SQLITE_DONE)
        return std::optional<GoalDispatchReceipt>{};
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "goal dispatch lookup failed");
    const auto revision = sqlite3_column_int64(row.get(), 0);
    const auto operation = OperationId::parse(column_text(row, 1));
    const auto prepared = column_time(row, 2);
    if (sqlite3_column_type(row.get(), 0) != SQLITE_INTEGER || revision <= 0 || !operation ||
        operation->is_nil() || !prepared || !valid_time(*prepared))
        return goal_error(ErrorCode::DataLoss, "goal dispatch identity is malformed");
    GoalDispatchReceipt receipt{
        {goal, static_cast<std::uint64_t>(revision), *operation}, *prepared, {}};
    if (sqlite3_column_type(row.get(), 3) != SQLITE_NULL) {
        const auto session = SessionId::parse(column_text(row, 3));
        const auto task = TaskId::parse(column_text(row, 4));
        const auto at = column_time(row, 5);
        if (!session || session->is_nil() || !task || task->is_nil() || !at || *at < *prepared)
            return goal_error(ErrorCode::DataLoss, "goal dispatch submission is malformed");
        receipt.submission = GoalDispatchSubmission{*session, *task, *at};
    }
    return std::optional{receipt};
}

Result<GoalDispatchReceipt> prepare_dispatch_row(sqlite3 *db, const GoalKey &goal,
                                                 std::uint64_t revision, GoalTime cutoff) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal dispatch transaction unavailable");
    Statement eligible(db, "SELECT 1 FROM goals WHERE tenant=?1 AND owner=?2 AND id=?3"
                           " AND state=0 AND revision=?4 AND due_at<=?5"
                           " AND (expires_at IS NULL OR expires_at>?5)");
    if (!eligible.valid() || !bind_key(eligible, goal) ||
        sqlite3_bind_int64(eligible.get(), 4, static_cast<std::int64_t>(revision)) != SQLITE_OK ||
        !bind_time(eligible, 5, cutoff))
        return goal_error(ErrorCode::Internal, "goal dispatch eligibility preparation failed");
    const auto step = sqlite3_step(eligible.get());
    if (step == SQLITE_DONE)
        return goal_error(ErrorCode::InvalidState, "goal version is not due for dispatch");
    if (step != SQLITE_ROW)
        return goal_error(ErrorCode::Internal, "goal dispatch eligibility check failed");
    eligible.reset();
    auto stored = read_dispatch_row(db, goal);
    if (!stored)
        return stored.error();
    if (stored.value()) {
        if (stored.value()->key.revision != revision)
            return goal_error(ErrorCode::InvalidState,
                              "goal dispatch belongs to a previous version");
        return *stored.value();
    }
    GoalDispatchReceipt receipt{{goal, revision, OperationId::generate()}, cutoff, {}};
    Statement insert(
        db, "INSERT INTO goal_dispatches(tenant,owner,goal_id,revision,operation,prepared_at)"
            " VALUES(?1,?2,?3,?4,?5,?6)");
    if (!insert.valid() || !bind_key(insert, goal) ||
        sqlite3_bind_int64(insert.get(), 4, static_cast<std::int64_t>(revision)) != SQLITE_OK ||
        !bind_text(insert, 5, receipt.key.operation.to_string()) || !bind_time(insert, 6, cutoff) ||
        sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal dispatch preparation failed");
    if (auto saved = transaction.commit(); !saved)
        return saved.error();
    return receipt;
}

Result<GoalDispatchReceipt> record_dispatch_row(sqlite3 *db, const GoalDispatchKey &key,
                                                const GoalDispatchSubmission &submission) {
    Transaction transaction(db);
    if (!transaction.valid())
        return goal_error(ErrorCode::Unavailable, "goal dispatch receipt transaction unavailable");
    auto stored = read_dispatch_row(db, key.goal);
    if (!stored)
        return stored.error();
    if (!stored.value() || stored.value()->key != key)
        return goal_error(ErrorCode::NotFound, "goal dispatch identity is unavailable");
    auto receipt = *stored.value();
    if (receipt.submission) {
        if (*receipt.submission != submission)
            return goal_error(ErrorCode::AlreadyExists, "goal dispatch receipt conflicts");
        return receipt;
    }
    if (submission.submitted_at < receipt.prepared_at)
        return goal_error(ErrorCode::InvalidArgument,
                          "goal dispatch submission predates preparation");
    Statement update(db, "UPDATE goal_dispatches SET session=?4,task=?5,submitted_at=?6"
                         " WHERE tenant=?1 AND owner=?2 AND goal_id=?3 AND session IS NULL");
    if (!update.valid() || !bind_key(update, key.goal) ||
        !bind_text(update, 4, submission.session.to_string()) ||
        !bind_text(update, 5, submission.task.to_string()) ||
        !bind_time(update, 6, submission.submitted_at) ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db) != 1)
        return goal_error(ErrorCode::Internal, "goal dispatch receipt write failed");
    if (auto saved = transaction.commit(); !saved)
        return saved.error();
    receipt.submission = submission;
    return receipt;
}
} // namespace

Result<GoalDispatchReceipt> SqliteGoalStore::prepare_dispatch(const GoalKey &goal,
                                                              std::uint64_t expected_revision,
                                                              GoalTime cutoff) {
    if (!valid_key(goal) || !valid_dispatch_revision(expected_revision) || !valid_time(cutoff))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal dispatch preparation");
    return impl_->run<GoalDispatchReceipt>(
        {goal.owner, GoalStoreResource::Goal, GoalStoreOperation::PrepareDispatch, goal.id},
        [goal, expected_revision, cutoff](sqlite3 *db) {
            return prepare_dispatch_row(db, goal, expected_revision, cutoff);
        });
}

Result<std::optional<GoalDispatchReceipt>>
SqliteGoalStore::read_dispatch(const GoalKey &goal) const {
    if (!valid_key(goal))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal dispatch lookup");
    return impl_->run<std::optional<GoalDispatchReceipt>>(
        {goal.owner, GoalStoreResource::Goal, GoalStoreOperation::ReadDispatch, goal.id},
        [goal](sqlite3 *db) { return read_dispatch_row(db, goal); });
}

Result<GoalDispatchReceipt>
SqliteGoalStore::record_dispatch_submission(const GoalDispatchKey &key,
                                            GoalDispatchSubmission submission) {
    if (!valid_key(key.goal) || !valid_dispatch_revision(key.revision) || key.operation.is_nil() ||
        submission.session.is_nil() || submission.task.is_nil() ||
        !valid_time(submission.submitted_at))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal dispatch submission");
    return impl_->run<GoalDispatchReceipt>(
        {key.goal.owner, GoalStoreResource::Goal, GoalStoreOperation::RecordDispatchSubmission,
         key.goal.id},
        [key, submission](sqlite3 *db) { return record_dispatch_row(db, key, submission); });
}

} // namespace mira
