#include "sqlite_goal_store_internal.hpp"

#include <mira/interaction_coordinator.hpp>

#include <limits>

namespace mira {
namespace {
using storage::bind_key;
using storage::bind_owner;
using storage::bind_text;
using storage::bind_time;
using storage::column_text;
using storage::column_time;
using storage::goal_error;
using storage::Statement;
using storage::Transaction;
using storage::valid_key;
using storage::valid_owner;
using storage::valid_time;

bool valid_dispatch_revision(std::uint64_t revision) {
    return revision > 0 &&
           revision <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

Result<GoalPendingFulfillmentPage>
read_pending_fulfillments(sqlite3 *db, const GoalPendingFulfillmentQuery &query) {
    Statement rows(db, "SELECT g.id,g.revision,d.operation FROM goals g JOIN goal_dispatches d"
                       " ON d.tenant=g.tenant AND d.owner=g.owner AND d.goal_id=g.id"
                       " AND d.revision=g.revision WHERE g.tenant=?1 AND g.owner=?2 AND g.state=0"
                       " AND g.id>?3 AND d.submitted_at IS NOT NULL ORDER BY g.id LIMIT ?4");
    const auto after = query.after ? query.after->to_string() : std::string{};
    if (!rows.valid() || !bind_owner(rows, query.owner) || !bind_text(rows, 3, after) ||
        sqlite3_bind_int64(rows.get(), 4, static_cast<std::int64_t>(query.limit + 1)) != SQLITE_OK)
        return goal_error(ErrorCode::Internal, "pending goal fulfillment query preparation failed");
    GoalPendingFulfillmentPage page;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        const auto id = Id128::parse(column_text(rows, 0));
        const auto revision = sqlite3_column_int64(rows.get(), 1);
        const auto operation = OperationId::parse(column_text(rows, 2));
        if (!id || id->is_nil() || sqlite3_column_type(rows.get(), 1) != SQLITE_INTEGER ||
            revision <= 0 || !operation || operation->is_nil())
            return goal_error(ErrorCode::DataLoss, "a pending goal fulfillment is malformed");
        page.records.push_back(
            {{query.owner, *id}, static_cast<std::uint64_t>(revision), *operation});
    }
    if (step != SQLITE_DONE)
        return goal_error(ErrorCode::Internal, "pending goal fulfillment query failed");
    if (page.records.size() > query.limit) {
        page.records.pop_back();
        page.next = page.records.back().goal.id;
    }
    return page;
}

Result<void> check_reconciliation_context(const OperationContext &context) {
    if (context.cancelled())
        return goal_error(ErrorCode::Cancelled, "goal reconciliation was cancelled");
    if (context.expired(Timestamp::now()))
        return goal_error(ErrorCode::DeadlineExceeded, "goal reconciliation expired");
    return {};
}

Result<EventEnvelope> read_dispatch_completion_source(IEventStore &events,
                                                      const GoalDispatchSubmission &submission,
                                                      const GoalDispatchCompletionSource &source) {
    EventSliceQuery query;
    query.session_id = submission.session;
    query.after_sequence = source.sequence - 1;
    query.through_sequence = source.sequence;
    query.limit = 1;
    auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (page.value().snapshot_through != source.sequence ||
        page.value().scanned_through != source.sequence || page.value().has_more ||
        page.value().events.size() != 1)
        return goal_error(ErrorCode::DataLoss, "completion source position is invalid");
    auto &event = page.value().events.front();
    if (event.runtime_id != source.runtime || event.event_id != source.event ||
        event.session_id != submission.session || event.task_id != submission.task ||
        event.session_sequence != source.sequence)
        return goal_error(ErrorCode::PermissionDenied, "completion source is outside the dispatch");
    if (event.timestamp_origin == EventTimestampOrigin::UnavailableLegacy ||
        event.timestamp.wall.time_since_epoch().count() <= 0)
        return goal_error(ErrorCode::InvalidState, "completion source has no reliable timestamp");
    if (event.payload.type == "ConversationSettled" || event.payload.type == "LoopSettled" ||
        event.payload.type == "AssistantSpeech" ||
        event.payload.type == kVerifiedTaskCompletionEvent)
        return goal_error(ErrorCode::InvalidArgument,
                          "completion source requires a verified outcome receipt");
    return std::move(event);
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

Result<GoalDispatchPreparation> prepare_dispatch_row(sqlite3 *db, const GoalKey &goal,
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
        return GoalDispatchPreparation{*stored.value(), false};
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
    return GoalDispatchPreparation{receipt, true};
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
    auto prepared = prepare_dispatch_attempt(goal, expected_revision, cutoff);
    if (!prepared)
        return prepared.error();
    return std::move(prepared).value().receipt;
}

Result<GoalDispatchPreparation>
SqliteGoalStore::prepare_dispatch_attempt(const GoalKey &goal, std::uint64_t expected_revision,
                                          GoalTime cutoff) {
    if (!valid_key(goal) || !valid_dispatch_revision(expected_revision) || !valid_time(cutoff))
        return goal_error(ErrorCode::InvalidArgument, "invalid goal dispatch preparation");
    return impl_->run<GoalDispatchPreparation>(
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

Result<GoalRecord> SqliteGoalStore::fulfill_dispatch(const GoalDispatchKey &key,
                                                     IEventStore &events) {
    auto fulfilled = reconcile_dispatch_completion(key, events, OperationContext{});
    if (!fulfilled)
        return fulfilled.error();
    if (!fulfilled.value())
        return goal_error(ErrorCode::NotFound, "the dispatched task has no durable completion");
    return std::move(*fulfilled.value());
}

Result<EventId> SqliteGoalStore::record_dispatch_completion(
    const GoalDispatchKey &key, IEventStore &events, const GoalDispatchCompletionSource &source,
    const GoalDispatchCompletionVerifier &verify, const OperationContext &context) {
    if (!valid_key(key.goal) || !valid_dispatch_revision(key.revision) || key.operation.is_nil() ||
        source.runtime.is_nil() || source.event.is_nil() || source.sequence == 0 ||
        source.sequence > static_cast<SessionSequence>(storage::kMaxSqlInteger) || !verify ||
        context.session.is_nil() || context.task.is_nil())
        return goal_error(ErrorCode::InvalidArgument, "invalid dispatch completion registration");
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto read_authorized = [&] {
        return impl_->run<std::optional<GoalDispatchReceipt>>(
            {key.goal.owner, GoalStoreResource::Goal, GoalStoreOperation::RecordDispatchCompletion,
             key.goal.id},
            [goal = key.goal](sqlite3 *db) { return read_dispatch_row(db, goal); });
    };
    const auto record = read_authorized();
    if (!record)
        return record.error();
    if (!record.value() || record.value()->key != key || !record.value()->submission)
        return goal_error(ErrorCode::InvalidState,
                          "completion requires the original submitted dispatch");
    const auto &submission = *record.value()->submission;
    if (context.session != submission.session || context.task != submission.task)
        return goal_error(ErrorCode::PermissionDenied,
                          "completion operation is outside the dispatch");
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto event = read_dispatch_completion_source(events, submission, source);
    if (!event)
        return event.error();
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto completed = [&]() -> Result<WallTimePoint> {
        try {
            return verify(*record.value(), event.value(), context);
        } catch (...) {
            return goal_error(ErrorCode::InvalidState, "dispatch completion verifier failed");
        }
    }();
    if (!completed)
        return completed.error();
    if (completed.value() < submission.submitted_at ||
        completed.value() > event.value().timestamp.wall)
        return goal_error(ErrorCode::InvalidState,
                          "completion time is outside the dispatch evidence");
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    if (const auto durable = events.flush(Durability::ProcessCrash); !durable)
        return durable.error();
    const auto current = read_authorized();
    if (!current)
        return current.error();
    if (current.value() != record.value())
        return goal_error(ErrorCode::InvalidState,
                          "dispatch changed during completion verification");
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto receipt = append_verified_task_completion(
        events, source.runtime, submission.session, submission.task,
        {key.goal.owner.tenant, key.goal.owner.user, completed.value(),
         VerifiedDispatchCompletionSource{key.operation, source.event, source.sequence}},
        0);
    if (!receipt)
        return receipt.error();
    return receipt.value().event_id;
}

Result<GoalPendingFulfillmentPage>
SqliteGoalStore::pending_fulfillments(const GoalPendingFulfillmentQuery &query) const {
    if (!valid_owner(query.owner) || query.limit == 0 || query.limit > 128 ||
        (query.after && query.after->is_nil()))
        return goal_error(ErrorCode::InvalidArgument, "invalid pending fulfillment query");
    return impl_->run<GoalPendingFulfillmentPage>(
        {query.owner, GoalStoreResource::Goal, GoalStoreOperation::ReadPendingFulfillments, {}},
        [query](sqlite3 *db) { return read_pending_fulfillments(db, query); });
}

Result<GoalDispatchOwnerPage>
SqliteGoalStore::dispatch_owners(const GoalDispatchOwnerQuery &query) const {
    if (!valid_time(query.as_of) || query.limit == 0 || query.limit > 128 ||
        (query.after && !valid_owner(*query.after)))
        return goal_error(ErrorCode::InvalidArgument, "invalid dispatch owner query");
    return impl_->channel.run<GoalDispatchOwnerPage>([state = impl_.get(), query](sqlite3 *db)
                                                         -> Result<GoalDispatchOwnerPage> {
        // Keep the three authorization checks tied to their respective pending work.
        Statement rows(db, "WITH work(tenant,owner,kind) AS ("
                           " SELECT g.tenant,g.owner,CASE WHEN d.task IS NULL THEN 0 ELSE 1 END"
                           " FROM goals g LEFT JOIN goal_dispatches d ON d.tenant=g.tenant"
                           " AND d.owner=g.owner AND d.goal_id=g.id"
                           " WHERE (g.tenant,g.owner)>(?1,?2) AND g.state=0"
                           " AND (d.revision IS NULL OR d.revision=g.revision)"
                           " AND (d.submitted_at IS NOT NULL OR (g.due_at IS NOT NULL"
                           " AND (g.expires_at IS NULL OR g.expires_at>?3)))"
                           " UNION ALL SELECT tenant,owner,2 FROM recurring_plans"
                           " WHERE (tenant,owner)>(?1,?2) AND state=0 AND next_at IS NOT NULL)"
                           " SELECT tenant,owner,max(kind=0),max(kind=1),max(kind=2) FROM work"
                           " GROUP BY tenant,owner ORDER BY tenant,owner");
        if (!rows.valid() ||
            !bind_text(rows, 1, query.after ? query.after->tenant.to_string() : std::string{}) ||
            !bind_text(rows, 2, query.after ? query.after->user.to_string() : std::string{}) ||
            !bind_time(rows, 3, query.as_of))
            return goal_error(ErrorCode::Internal, "dispatch owner query preparation failed");
        GoalDispatchOwnerPage page;
        int step = SQLITE_OK;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
            const GoalOwner owner{TenantId::parse(column_text(rows, 0)).value_or(TenantId{}),
                                  UserId::parse(column_text(rows, 1)).value_or(UserId{})};
            if (!valid_owner(owner))
                return goal_error(ErrorCode::DataLoss, "dispatch owner identity is malformed");
            const bool authorized =
                (sqlite3_column_int(rows.get(), 2) != 0 &&
                 state->allows(
                     {owner, GoalStoreResource::Goal, GoalStoreOperation::ReadDispatchDue, {}})) ||
                (sqlite3_column_int(rows.get(), 3) != 0 &&
                 state->allows({owner,
                                GoalStoreResource::Goal,
                                GoalStoreOperation::ReadPendingFulfillments,
                                {}})) ||
                (sqlite3_column_int(rows.get(), 4) != 0 &&
                 state->allows(
                     {owner, GoalStoreResource::RecurringPlan, GoalStoreOperation::ReadDue, {}}));
            if (!authorized)
                continue;
            if (page.owners.size() == query.limit) {
                page.next = page.owners.back();
                return page;
            }
            page.owners.push_back(owner);
        }
        if (step != SQLITE_DONE)
            return goal_error(ErrorCode::Internal, "dispatch owner query failed");
        return page;
    });
}

Result<std::optional<GoalRecord>>
SqliteGoalStore::reconcile_dispatch_completion(const GoalDispatchKey &key, IEventStore &events,
                                               const OperationContext &context) {
    if (!valid_key(key.goal) || !valid_dispatch_revision(key.revision) || key.operation.is_nil())
        return goal_error(ErrorCode::InvalidArgument, "invalid goal dispatch identity");
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto dispatch = read_dispatch(key.goal);
    if (!dispatch)
        return dispatch.error();
    if (!dispatch.value() || dispatch.value()->key != key || !dispatch.value()->submission)
        return std::optional<GoalRecord>{};
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    const auto &submission = *dispatch.value()->submission;
    EventSliceQuery query;
    query.session_id = submission.session;
    query.task_id = submission.task;
    query.event_type = kVerifiedTaskCompletionEvent;
    query.limit = 2;
    query.scan_limit = 4096;
    query.byte_limit = 1024;
    const auto page = events.read_slice(query);
    if (!page)
        return page.error();
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    if (page.value().events.size() > 1)
        return goal_error(ErrorCode::DataLoss, "the dispatched task has conflicting completions");
    if (page.value().has_more)
        return goal_error(ErrorCode::ResourceExhausted, "completion search exceeds its bound");
    if (page.value().events.empty())
        return std::optional<GoalRecord>{};
    const auto &event = page.value().events.front();
    if (event.session_id != submission.session || event.task_id != submission.task ||
        event.session_sequence > page.value().scanned_through ||
        event.session_sequence > page.value().snapshot_through)
        return goal_error(ErrorCode::DataLoss, "completion is outside the dispatched task");
    const auto completion = parse_verified_task_completion(event);
    if (!completion)
        return completion.error();
    const auto at = std::chrono::floor<std::chrono::milliseconds>(completion.value().completed_at);
    if (completion.value().tenant != key.goal.owner.tenant ||
        completion.value().user != key.goal.owner.user || at < submission.submitted_at ||
        (completion.value().dispatch_source &&
         completion.value().dispatch_source->operation != key.operation))
        return goal_error(ErrorCode::InvalidState,
                          "completion does not match the dispatch owner or time");
    if (const auto durable = events.flush(Durability::ProcessCrash); !durable)
        return durable.error();
    if (const auto ready = check_reconciliation_context(context); !ready)
        return ready.error();
    auto settled = settle(key.goal, key.revision, {GoalState::Fulfilled, event.event_id, at});
    if (!settled)
        return settled.error();
    return std::optional{std::move(settled).value()};
}

} // namespace mira
