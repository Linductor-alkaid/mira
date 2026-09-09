#pragma once

// Shared fixtures for the M13 stage-F tests (DEC-029/030): an in-memory
// IMemory double with working exact/kind/scope filtering and failure knobs,
// a scripted tool whose dispatch outcomes alternate per script, and the
// learning scope used across scenarios. Consumers: tests/m13/*.cpp.

#include "m9_support.hpp"

#include <mira/memory_contracts.hpp>
#include <mira/workflow_learning.hpp>

#include <algorithm>
#include <mutex>

namespace mira::testing {

// In-memory IMemory double. query() filters by scope allowlist, kind, Active
// status and verbatim exact terms, then ranks by recency (newest first) with
// descending scores — enough for the failure-retrieval path without pulling
// the SQLite backend into the runtime test. apply() enforces mutation
// validation and mutation-id idempotency like the reference store.
class FakeLearningMemory final : public IMemory {
  public:
    Result<MemoryQueryResult> query(const MemoryQuery &query) const override {
        ++queries;
        if (fail_queries.load()) {
            Error error;
            error.code = ErrorCode::Unavailable;
            error.domain = "mira.test";
            error.safe_message = "learning query backend down";
            return error;
        }
        std::lock_guard lock(mutex_);
        MemoryQueryResult result;
        std::vector<const MemoryRecord *> matches;
        for (const auto &record : records_) {
            const bool scope_ok =
                std::any_of(query.scopes.begin(), query.scopes.end(),
                            [&](const MemoryScope &scope) { return scope == record.scope; });
            if (!scope_ok || record.status != MemoryStatus::Active) {
                continue;
            }
            if (query.kinds.has_value() &&
                std::find(query.kinds->begin(), query.kinds->end(), record.kind) ==
                    query.kinds->end()) {
                continue;
            }
            const bool terms_ok = std::all_of(
                query.exact_terms.begin(), query.exact_terms.end(), [&](const std::string &term) {
                    return record.statement.find(term) != std::string::npos;
                });
            if (!terms_ok) {
                continue;
            }
            matches.push_back(&record);
        }
        std::sort(matches.begin(), matches.end(),
                  [](const MemoryRecord *left, const MemoryRecord *right) {
                      return left->recorded_at > right->recorded_at;
                  });
        const std::size_t bounded = std::min(matches.size(), query.max_results);
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
        if (fail_applies.exchange(false)) {
            Error error;
            error.code = ErrorCode::Internal;
            error.domain = "mira.test";
            error.safe_message = "learning apply forced failure";
            return error;
        }
        if (auto valid = mutation.validate(); !valid.has_value()) {
            return valid.error();
        }
        std::lock_guard lock(mutex_);
        ++applies;
        for (const auto &applied : applied_mutations_) {
            if (applied == mutation.id) {
                MemoryMutationResult result;
                result.applied = MemoryMutationType::Noop;
                result.record = mutation.proposed.id;
                result.idempotent_replay = true;
                return result;
            }
        }
        applied_mutations_.push_back(mutation.id);
        records_.push_back(mutation.proposed);
        MemoryMutationResult result;
        result.applied = mutation.type;
        result.record = mutation.proposed.id;
        result.new_version = 1;
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

    [[nodiscard]] std::vector<MemoryRecord> snapshot() const {
        std::lock_guard lock(mutex_);
        return records_;
    }

    mutable std::atomic<bool> fail_queries{false};
    std::atomic<bool> fail_applies{false};
    std::atomic<std::uint64_t> applies{0};
    mutable std::atomic<std::uint64_t> queries{0};

  private:
    mutable std::mutex mutex_;
    std::vector<MemoryRecord> records_;
    std::vector<MutationId> applied_mutations_;
};

// One scripted tool: dispatch number `fail_on` (1-based, counting across all
// dispatches of this instance) fails; every other dispatch succeeds. Lets a
// single registry entry fail run A, recover run B and fail run C in order.
class ScriptedTool final {
  public:
    explicit ScriptedTool(std::vector<int> fail_on) : fail_on_(std::move(fail_on)) {}

    [[nodiscard]] BuiltinToolRegistration registration(const std::string &wire_name) {
        BuiltinToolRegistration registration;
        registration.spec.wire_name = wire_name;
        registration.spec.description = "m13 scripted tool";
        registration.spec.parameters_schema = JsonSchema{parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        registration.spec.has_side_effects = false;
        registration.handler = [this](const JsonValue &,
                                      const OperationContext &) -> Result<JsonValue> {
            const auto count = ++dispatches;
            if (std::find(fail_on_.begin(), fail_on_.end(), count) != fail_on_.end()) {
                Error error;
                error.code = ErrorCode::PlatformError;
                error.domain = "mira.test";
                error.domain_code = 77;
                error.safe_message = "scripted failure";
                return error;
            }
            return JsonValue{"sent"};
        };
        return registration;
    }

    std::atomic<int> dispatches{0};

  private:
    std::vector<int> fail_on_;
};

[[nodiscard]] inline MemoryScope learning_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Agent;
    scope.subject_id = "mira.workflow.learning";
    return scope;
}

// Scans the fixture event store for learning audit events of one type.
[[nodiscard]] inline std::vector<std::string>
learning_event_outcomes(const MemoryEventStore &events, const SessionId &session,
                        std::string_view type) {
    EventQuery query;
    query.session_id = session;
    const auto page = events.read(query);
    std::vector<std::string> outcomes;
    if (!page.has_value()) {
        return outcomes;
    }
    for (const auto &envelope : page.value().events) {
        if (envelope.payload.type != type) {
            continue;
        }
        JsonValue payload = parse_json(envelope.payload.data).value();
        const auto *outcome = payload.find("outcome");
        if (outcome != nullptr && outcome->is_string()) {
            outcomes.push_back(*outcome->as_string());
        }
    }
    return outcomes;
}

} // namespace mira::testing
