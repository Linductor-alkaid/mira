#pragma once

// Shared fixtures for the M23 Stage W4 promotion tests: deterministic
// snapshot/item builders (M21/M22 style) and FaithfulMemory, an in-memory
// IMemory double mirroring the reference store semantics the consolidation
// pipeline relies on (M4/M13). Consumers: tests/m23/*.cpp.

#include "../support/test.hpp"

#include <mira/context_consolidation.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/memory_consolidation.hpp>
#include <mira/memory_contracts.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mira::testing {

// ---------------------------------------------------------------------------
// Deterministic builders
// ---------------------------------------------------------------------------

[[nodiscard]] inline Id128 m23_id_from_seed(std::uint64_t seed) {
    // All eight seed bytes participate, so distinct seeds never collide.
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

[[nodiscard]] inline SessionId m23_session_from_seed(std::uint64_t seed) {
    return SessionId{m23_id_from_seed(seed)};
}

[[nodiscard]] inline TaskId m23_task_from_seed(std::uint64_t seed) {
    return TaskId{m23_id_from_seed(seed)};
}

[[nodiscard]] inline EventId m23_event_from_seed(std::uint64_t seed) {
    return EventId{m23_id_from_seed(seed)};
}

[[nodiscard]] inline WorkingContextItem m23_item(std::string content, std::uint64_t event_seed,
                                                 double confidence) {
    WorkingContextItem item;
    item.content = std::move(content);
    item.source_events = {m23_event_from_seed(event_seed)};
    item.source_sequence = event_seed;
    item.confidence = confidence;
    return item;
}

[[nodiscard]] inline MemoryScope m23_env_scope(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    scope.tenant_id = "tenant-9";
    return scope;
}

[[nodiscard]] inline MemoryScope m23_user_scope(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::User;
    scope.subject_id = std::move(subject);
    return scope;
}

// Fixed wall clock so validity/recorded_at assertions are exact.
[[nodiscard]] inline Timestamp m23_fixed_now() {
    Timestamp stamp;
    stamp.wall = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    return stamp;
}

// Minimal valid snapshot: identity, watermark, one source checkpoint.
[[nodiscard]] inline WorkingContextSnapshot m23_snapshot(std::uint64_t seed) {
    WorkingContextSnapshot snapshot;
    snapshot.id = working_context_snapshot_id_from_seed("m23-snapshot-" + std::to_string(seed));
    snapshot.session_id = m23_session_from_seed(seed);
    snapshot.task_id = m23_task_from_seed(seed + 1);
    snapshot.task_epoch = 3;
    snapshot.environment_epoch = 7;
    snapshot.through_event_sequence = 48;
    snapshot.source_checkpoints = {
        conversation_checkpoint_id_from_seed("m23-checkpoint-" + std::to_string(seed))};
    snapshot.created_at = m23_fixed_now();
    return snapshot;
}

[[nodiscard]] inline EventEnvelope m23_fact_event(const SessionId &session, const TaskId &task,
                                                  std::string key, std::string value,
                                                  std::uint64_t sequence) {
    EventEnvelope event;
    event.event_id = EventId::generate();
    event.runtime_id = RuntimeId::generate();
    event.session_id = session;
    event.task_id = task;
    event.session_sequence = sequence;
    event.task_sequence = sequence;
    event.timestamp = m23_fixed_now();
    event.payload = EventPayload{
        "TaskFactVerified",
        to_json_string(JsonValue::Object{{"key", std::move(key)}, {"value", std::move(value)}}),
        EventClass::State};
    return event;
}

// Directly builds one hand-stored record (the pre-existing copies of the
// no-downgrade scenarios); bypasses the promotion path on purpose.
[[nodiscard]] inline MemoryRecord m23_stored_record(const MemoryScope &scope, MemoryKind kind,
                                                    std::string statement,
                                                    MemoryVerification verification,
                                                    std::uint64_t recorded_offset_seconds) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = kind;
    record.statement = std::move(statement);
    record.validity.valid_from =
        m23_fixed_now().wall +
        std::chrono::seconds{static_cast<std::int64_t>(recorded_offset_seconds)};
    record.recorded_at = record.validity.valid_from;
    record.provenance = {m23_event_from_seed(recorded_offset_seconds + 900)};
    record.verification = verification;
    record.confidence = 0.9F;
    return record;
}

// ---------------------------------------------------------------------------
// FaithfulMemory: scope-allowlisted Active-only retrieval with kind and
// verbatim exact-term filtering, mutation validation, mutation-id idempotent
// replay, Add id-uniqueness and Supersede target + version checks with
// predecessor close-out — the store contract the pipeline correctness rests
// on. apply() can be gated: an armed gate parks every call on a condition
// variable until it is released — or until the supervisor cancellation probe
// fires, in which case the wait is unblocked, the write is refused with
// Cancelled and nothing lands (the cancellable-wait path AGENTS.md requires
// for blocking store calls).
// ---------------------------------------------------------------------------
class FaithfulMemory final : public IMemory {
  public:
    std::atomic<bool> fail_applies{false};

    void arm_gate() {
        std::lock_guard lock(mutex_);
        gated_ = true;
    }

    void set_cancel_probe(SupervisorToken probe) {
        std::lock_guard lock(mutex_);
        cancel_probe_ = std::move(probe);
    }

    void wait_apply_entered(std::size_t count) const {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this, count] { return entered_ >= count; });
    }

    void release_gate() {
        std::lock_guard lock(mutex_);
        released_ = true;
        gate_cv_.notify_all();
    }

    Result<MemoryQueryResult> query(const MemoryQuery &request) const override {
        std::lock_guard lock(mutex_);
        MemoryQueryResult result;
        std::vector<const MemoryRecord *> matches;
        for (const auto &record : records_) {
            const bool scope_ok =
                std::any_of(request.scopes.begin(), request.scopes.end(),
                            [&](const MemoryScope &scope) { return scope == record.scope; });
            if (!scope_ok || record.status != MemoryStatus::Active) {
                continue;
            }
            if (request.kinds.has_value() && std::find(request.kinds->begin(), request.kinds->end(),
                                                       record.kind) == request.kinds->end()) {
                continue;
            }
            const bool terms_ok =
                std::all_of(request.exact_terms.begin(), request.exact_terms.end(),
                            [&](const std::string &term) {
                                return record.statement.find(term) != std::string::npos;
                            });
            if (!terms_ok) {
                continue;
            }
            matches.push_back(&record);
        }
        std::stable_sort(matches.begin(), matches.end(),
                         [](const MemoryRecord *left, const MemoryRecord *right) {
                             return left->recorded_at > right->recorded_at;
                         });
        const std::size_t bounded = std::min(matches.size(), request.max_results);
        double score = 1.0;
        for (std::size_t index = 0; index < bounded; ++index) {
            result.records.push_back(*matches[index]);
            result.scores.push_back(score);
            score = std::max(0.0, score - 0.1);
        }
        result.quality.exact_leg_ran = true;
        return result;
    }

    Result<std::optional<MemoryRecord>> get(MemoryId record) const override {
        std::lock_guard lock(mutex_);
        for (const auto &stored : records_) {
            if (stored.id == record) {
                return std::optional<MemoryRecord>{stored};
            }
        }
        return std::optional<MemoryRecord>{};
    }

    Result<MemoryMutationResult> apply(const MemoryMutation &mutation) override {
        {
            std::unique_lock lock(mutex_);
            if (gated_) {
                ++entered_;
                entered_cv_.notify_all();
                // The supervisor signals cancellation through its stop flag
                // only — it never notifies this condition variable — so the
                // cancellable wait polls the probe (M22 scripted-curator
                // precedent). The poll interval bounds wake-up latency, not
                // sequencing; release() still wakes the waiter immediately.
                while (!(released_ || cancel_probe_.stop_requested())) {
                    gate_cv_.wait_for(lock, std::chrono::milliseconds(1));
                }
                if (!released_) {
                    Error cancelled;
                    cancelled.code = ErrorCode::Cancelled;
                    cancelled.domain = "mira.test.m23";
                    cancelled.safe_message = "gated store refused the write at shutdown";
                    return cancelled;
                }
            }
        }
        if (fail_applies.exchange(false)) {
            Error error;
            error.code = ErrorCode::Internal;
            error.domain = "mira.test.m23";
            error.safe_message = "apply forced failure";
            return error;
        }
        if (const auto valid = mutation.validate(); !valid.has_value()) {
            return valid.error();
        }
        std::lock_guard lock(mutex_);
        for (const auto &past : replay_log_) {
            if (past.first == mutation.id) {
                MemoryMutationResult result = past.second;
                result.applied = MemoryMutationType::Noop;
                result.idempotent_replay = true;
                return result;
            }
        }
        MemoryMutationResult result;
        if (mutation.type == MemoryMutationType::Add) {
            for (const auto &stored : records_) {
                if (stored.id == mutation.proposed.id) {
                    return make_memory_error(MemoryDomainCode::VersionConflict,
                                             "add target already exists");
                }
            }
            MemoryRecord stored = mutation.proposed;
            stored.scope = mutation.scope;
            stored.status = MemoryStatus::Active;
            stored.version = 1;
            records_.push_back(stored);
            result.applied = MemoryMutationType::Add;
            result.record = stored.id;
            result.new_version = 1;
        } else if (mutation.type == MemoryMutationType::Supersede) {
            auto target =
                std::find_if(records_.begin(), records_.end(), [&](const MemoryRecord &record) {
                    return record.id == *mutation.target;
                });
            if (target == records_.end()) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "mutation target does not exist");
            }
            if (target->version != *mutation.expected_version) {
                return make_memory_error(MemoryDomainCode::VersionConflict,
                                         "memory version moved on; re-read and re-plan");
            }
            if (mutation.proposed.id == *mutation.target) {
                return make_memory_error(MemoryDomainCode::InvalidMutation,
                                         "supersede must introduce a fresh record id");
            }
            // Close out the predecessor exactly like the reference store.
            target->status = MemoryStatus::Superseded;
            target->validity.valid_until = mutation.proposed.validity.valid_from;
            target->version += 1;
            MemoryRecord stored = mutation.proposed;
            stored.scope = mutation.scope;
            stored.status = MemoryStatus::Active;
            stored.version = 1;
            records_.push_back(stored);
            result.applied = MemoryMutationType::Supersede;
            result.record = stored.id;
            result.new_version = 1;
        } else {
            return make_memory_error(MemoryDomainCode::InvalidMutation,
                                     "fixture supports Add and Supersede only");
        }
        replay_log_.emplace_back(mutation.id, result);
        ++applies_completed_;
        return result;
    }

    Result<MemoryCompactionResult> compact(const MemoryScope &) override {
        return MemoryCompactionResult{};
    }

    Result<ErasureResult> erase(const ErasureRequest &) override {
        ErasureResult result;
        result.status = ErasureStatus::Complete;
        return result;
    }

    [[nodiscard]] std::size_t stored_records() const {
        std::lock_guard lock(mutex_);
        return records_.size();
    }

    [[nodiscard]] std::vector<MemoryRecord> records() const {
        std::lock_guard lock(mutex_);
        return records_;
    }

    [[nodiscard]] std::size_t applies_completed() const {
        std::lock_guard lock(mutex_);
        return applies_completed_;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable entered_cv_;
    std::condition_variable gate_cv_;
    bool gated_ = false;
    bool released_ = false;
    std::size_t entered_ = 0;
    SupervisorToken cancel_probe_;
    std::vector<MemoryRecord> records_;
    std::vector<std::pair<MutationId, MemoryMutationResult>> replay_log_;
    std::size_t applies_completed_ = 0;
};

// RAII guard: a failing MIRA_CHECK between arming and the deliberate release
// must never strand an Executor worker on a destroyed gate.
struct GateReleaser final {
    FaithfulMemory &memory;
    ~GateReleaser() { memory.release_gate(); }
};

} // namespace mira::testing
