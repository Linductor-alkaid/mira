#include <mira/action_dispatcher.hpp>
#include <mira/json.hpp>

#include <utility>

namespace mira {
namespace {
Error dispatch_error(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.domain = "mira.action_dispatcher";
    error.safe_message = std::move(message);
    return error;
}

Sha256Digest input_digest(const InputSequence &input) {
    JsonValue::Array events;
    for (const auto &event : input.events) {
        events.emplace_back(
            JsonValue::Object{{"kind", event.kind},
                              {"payload", event.payload},
                              {"duration_ms", static_cast<std::int64_t>(event.duration_ms)}});
    }
    // Only the digest is persisted, never the payload (which may be typed text).
    return digest_string(to_json_string(
        JsonValue::Object{{"display", input.display ? input.display->to_string() : "primary"},
                          {"events", std::move(events)}}));
}
} // namespace

JournaledActionDispatcher::JournaledActionDispatcher(RuntimeId runtime, SessionId session,
                                                     std::shared_ptr<IEventStore> events,
                                                     ActionLeaseAcquirer acquire)
    : runtime_(runtime), session_(session), events_(std::move(events)),
      acquire_(std::move(acquire)) {}

Result<ExecutionReceipt> JournaledActionDispatcher::dispatch(IEnvironment &environment,
                                                             const InputSequence &input,
                                                             const OperationContext &context,
                                                             EnvironmentEpoch observed_epoch) {
    if (!events_ || !acquire_) {
        return dispatch_error(ErrorCode::InvalidState,
                              "action dispatcher requires store and lease acquirer");
    }
    if (runtime_.is_nil() || session_.is_nil() || context.session != session_ ||
        context.task.is_nil() || context.operation.is_nil() || input.events.empty()) {
        return dispatch_error(ErrorCode::InvalidArgument, "action dispatch frame is invalid");
    }
    const auto stopped = [&]() -> Result<void> {
        if (context.cancelled()) {
            return dispatch_error(ErrorCode::Cancelled, "action dispatch was cancelled");
        }
        if (context.expired(Timestamp::now())) {
            return dispatch_error(ErrorCode::DeadlineExceeded, "action dispatch deadline expired");
        }
        return {};
    };
    if (auto ready = stopped(); !ready) {
        return ready.error();
    }
    ActionIntent intent{
        ActionId::generate(),
        runtime_,
        session_,
        context.task,
        context.task_epoch,
        observed_epoch,
        input_digest(input),
        {"input", input.display ? input.display->to_string() : "primary", "session"}};
    ActionJournal journal(runtime_, *events_);
    if (auto prepared = journal.prepare(intent); !prepared) {
        return prepared.error();
    }
    Result<std::unique_ptr<IActionDispatchLease>> acquired(Error{});
    try {
        acquired = acquire_(intent, context);
    } catch (...) {
        return dispatch_error(ErrorCode::Internal, "action lease acquisition raised an exception");
    }
    if (!acquired) {
        return acquired.error();
    }
    auto lease = std::move(acquired).value();
    if (!lease) {
        return dispatch_error(ErrorCode::InvalidState, "action lease acquirer returned no lease");
    }
    if (auto ready = stopped(); !ready) {
        return ready.error();
    }
    if (auto started = journal.dispatch_started(intent); !started) {
        return started.error();
    }
    // Every path from here leaves a durable start. Even a pre-call failure
    // must be recovered conservatively; the journal is not rolled back.
    if (auto ready = stopped(); !ready) {
        return ready.error();
    }
    Result<ExecutionReceipt> receipt(Error{});
    const auto quiesce = [&]() -> Result<void> {
        try {
            return environment.interrupt(context);
        } catch (...) {
            return dispatch_error(ErrorCode::Internal, "input release raised an exception");
        }
    };
    try {
        if (auto valid = lease->validate(intent, context); !valid) {
            return valid.error();
        }
        if (auto ready = stopped(); !ready) {
            return ready.error();
        }
        receipt = environment.execute(input, context);
    } catch (...) {
        auto uncertain = journal.execution_uncertain(intent, "dispatch raised an exception");
        const auto released = quiesce();
        if (!uncertain) {
            return uncertain.error();
        }
        if (!released) {
            return released.error();
        }
        return dispatch_error(ErrorCode::Internal,
                              "action dispatch raised an exception; verify before retry");
    }
    if (!receipt) {
        auto uncertain = journal.execution_uncertain(intent, "dispatch returned an error");
        const auto released = quiesce();
        if (!uncertain) {
            return uncertain.error();
        }
        if (!released) {
            return released.error();
        }
        return receipt.error();
    }
    const auto &execution = receipt.value();
    const bool unknown = execution.side_effect_may_have_occurred ||
                         (execution.status != ExecutionStatus::Completed &&
                          execution.status != ExecutionStatus::Rejected);
    Result<AppendReceipt> recorded(Error{});
    if (unknown) {
        recorded = journal.execution_uncertain(intent, "execution requires verification");
        // A pending or unknown platform result must not outlive the host
        // reservation as an uncontrolled gesture. Release input before the
        // lease is destroyed, even if journaling the uncertainty failed.
        const auto released = quiesce();
        if (!recorded) {
            return recorded.error();
        }
        if (!released) {
            return released.error();
        }
    } else {
        auto summary = execution;
        summary.safe_message =
            execution.status == ExecutionStatus::Rejected ? "rejected" : "completed";
        recorded = journal.receipt(intent, summary);
    }
    if (!recorded) {
        // A receipt append failure cannot be treated as successful execution.
        // The surviving start keeps verification pending even if this second
        // best-effort append also fails.
        (void)journal.execution_uncertain(intent, "execution result could not be committed");
        return recorded.error();
    }
    return receipt;
}
} // namespace mira
