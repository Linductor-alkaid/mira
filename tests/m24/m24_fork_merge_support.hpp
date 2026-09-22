#pragma once

// Shared fixtures for the M24 Stage W5 subagent fork/merge tests:
// deterministic snapshot/item/seed builders (M21-M23 style) and a gated
// working-context store double for the supervisor cancellation paths.
// Consumers: tests/m24/*.cpp.

#include "../support/test.hpp"

#include <mira/context_consolidation.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_fork.hpp>

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
// Deterministic builders (M21/M22/M23 style, m24 seed spaces)
// ---------------------------------------------------------------------------

[[nodiscard]] inline Id128 m24_id_from_seed(std::uint64_t seed) {
    // All eight seed bytes participate, so distinct seeds never collide.
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed >> ((index % sizeof(seed)) * 8U));
    }
    return Id128{bytes};
}

[[nodiscard]] inline SessionId m24_session_from_seed(std::uint64_t seed) {
    return SessionId{m24_id_from_seed(seed)};
}

[[nodiscard]] inline TaskId m24_task_from_seed(std::uint64_t seed) {
    return TaskId{m24_id_from_seed(seed)};
}

[[nodiscard]] inline EventId m24_event_from_seed(std::uint64_t seed) {
    return EventId{m24_id_from_seed(seed)};
}

[[nodiscard]] inline ModelProfileId m24_profile_from_seed(std::uint64_t seed) {
    return ModelProfileId{m24_id_from_seed(seed)};
}

// Fixed wall clock so created_at/validity assertions stay exact.
[[nodiscard]] inline Timestamp m24_fixed_now() {
    Timestamp stamp;
    stamp.wall = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    return stamp;
}

[[nodiscard]] inline WorkingContextItem m24_item(std::string content, std::uint64_t event_seed,
                                                 double confidence) {
    WorkingContextItem item;
    item.content = std::move(content);
    item.source_events = {m24_event_from_seed(event_seed)};
    item.source_sequence = event_seed;
    item.confidence = confidence;
    return item;
}

[[nodiscard]] inline WorkingContextIdentity m24_identity_from_seed(std::uint64_t seed) {
    WorkingContextIdentity identity;
    identity.task = m24_task_from_seed(seed);
    identity.task_epoch = 3;
    identity.environment_epoch = 7;
    return identity;
}

// Minimal valid parent snapshot: identity, watermark, one source checkpoint
// and the frozen `generated_by` left nil (the deterministic-W1 parent form).
[[nodiscard]] inline WorkingContextSnapshot m24_snapshot(std::uint64_t seed) {
    WorkingContextSnapshot snapshot;
    snapshot.id = working_context_snapshot_id_from_seed("m24-snapshot-" + std::to_string(seed));
    snapshot.session_id = m24_session_from_seed(seed);
    snapshot.task_id = m24_task_from_seed(seed + 1);
    snapshot.task_epoch = 3;
    snapshot.environment_epoch = 7;
    snapshot.through_event_sequence = 48;
    snapshot.source_checkpoints = {
        conversation_checkpoint_id_from_seed("m24-checkpoint-" + std::to_string(seed))};
    snapshot.created_at = m24_fixed_now();
    return snapshot;
}

// The frozen eight-section word list in struct declaration order (W5 plan
// §4.2). The delta vocabulary and the baseline canonical numbering both
// derive from this order.
[[nodiscard]] inline const std::vector<const char *> &m24_section_names() {
    static const std::vector<const char *> sections = {
        "constraints", "decisions", "open_issues", "active_tasks",
        "verified_facts", "failed_attempts", "important_refs", "next_actions"};
    return sections;
}

// Fills every one of the eight sections with one distinct item so verbatim
// copy assertions have full coverage.
inline void m24_fill_all_sections(WorkingContextSnapshot &snapshot, std::uint64_t event_seed) {
    std::uint64_t running = event_seed;
    const auto append = [&running](std::vector<WorkingContextItem> &section,
                                   const std::string &content) {
        section.push_back(m24_item(content, running, 0.8));
        running += 1;
    };
    append(snapshot.constraints, "constraint: deploy only on tuesdays");
    append(snapshot.decisions, "decision: batch provider after the rate limit incident");
    append(snapshot.open_issues, "issue: tenant quota still unanswered");
    append(snapshot.active_tasks, "task: finish the tenant onboarding flow");
    append(snapshot.verified_facts, "fact: staging accepts the v2 payload");
    append(snapshot.failed_attempts, "attempt: retry loop hit rate limit twice");
    append(snapshot.important_refs, "ref: tenant quota thread in ticket 482");
    append(snapshot.next_actions, "action: ask user about the deploy window");
}

// ---------------------------------------------------------------------------
// GatedWorkingContextStore: an IWorkingContextStore double forwarding to the
// in-memory reference store, with an armable gate parked inside put() so a
// supervised merge-commit can be observed in flight and cancelled at
// shutdown (m23 FaithfulMemory pattern; mutex/cv, no sleeps for sequencing).
// ---------------------------------------------------------------------------

class GatedWorkingContextStore final : public IWorkingContextStore {
  public:
    explicit GatedWorkingContextStore(WorkingContextStorePolicy policy = {})
        : store_(policy) {}

    void arm_gate() {
        std::lock_guard lock(mutex_);
        gated_ = true;
    }

    void set_cancel_probe(SupervisorToken probe) {
        std::lock_guard lock(mutex_);
        cancel_probe_ = std::move(probe);
    }

    void wait_put_entered(std::size_t count) const {
        std::unique_lock lock(mutex_);
        entered_cv_.wait(lock, [this, count] { return entered_ >= count; });
    }

    void release_gate() {
        std::lock_guard lock(mutex_);
        released_ = true;
        gate_cv_.notify_all();
    }

    Result<void> put(const WorkingContextSnapshot &snapshot) override {
        {
            std::unique_lock lock(mutex_);
            if (gated_) {
                ++entered_;
                entered_cv_.notify_all();
                // The supervisor signals cancellation through its stop flag
                // only, so the cancellable wait polls the probe (M22/M23
                // precedent); release() still wakes the waiter immediately.
                while (!(released_ || cancel_probe_.stop_requested())) {
                    gate_cv_.wait_for(lock, std::chrono::milliseconds(1));
                }
                if (!released_) {
                    Error cancelled;
                    cancelled.code = ErrorCode::Cancelled;
                    cancelled.domain = "mira.test.m24";
                    cancelled.safe_message = "gated store refused the merge write at shutdown";
                    return cancelled;
                }
            }
        }
        const auto written = store_.put(snapshot);
        if (written.has_value()) {
            std::lock_guard lock(mutex_);
            ++puts_completed_;
        }
        return written;
    }

    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    latest(SessionId session) const override {
        return store_.latest(session);
    }

    [[nodiscard]] Result<std::optional<WorkingContextSnapshot>>
    latest_at_or_before(SessionId session, std::uint64_t max_sequence) const override {
        return store_.latest_at_or_before(session, max_sequence);
    }

    [[nodiscard]] Result<std::size_t> count(SessionId session) const override {
        return store_.count(session);
    }

    Result<std::size_t> erase_session(SessionId session, std::string reason) override {
        return store_.erase_session(session, std::move(reason));
    }

    [[nodiscard]] std::size_t puts_completed() const {
        std::lock_guard lock(mutex_);
        return puts_completed_;
    }

  private:
    InMemoryWorkingContextStore store_;
    mutable std::mutex mutex_;
    mutable std::condition_variable entered_cv_;
    std::condition_variable gate_cv_;
    bool gated_ = false;
    bool released_ = false;
    std::size_t entered_ = 0;
    std::size_t puts_completed_ = 0;
    SupervisorToken cancel_probe_;
};

// RAII guard: a failing MIRA_CHECK between arming and the deliberate release
// must never strand an Executor worker on a destroyed gate.
struct StoreGateReleaser final {
    GatedWorkingContextStore &store;
    ~StoreGateReleaser() { store.release_gate(); }
};

} // namespace mira::testing
