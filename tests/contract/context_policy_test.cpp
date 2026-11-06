#include "support/test.hpp"
#include <mira/content_markers.hpp>
#include <mira/context_consolidation.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/context_working_context_fork.hpp>
#include <mira/context_working_context_promotion.hpp>
#include <mira/memory_consolidation.hpp>

namespace {
using namespace mira;

int defaults_are_stable_and_independently_owned() {
    const std::vector<std::string> forbidden{
        "api_key", "apikey", "authorization:", "bearer ", "password=", "secret="};
    const std::vector<std::string> injection{
        "ignore previous", "disregard previous", "you are now",
        "system:",         "new instructions:",  "override policy"};
    MIRA_CHECK(default_forbidden_content_markers() == forbidden);
    MIRA_CHECK(default_injection_content_markers() == injection);
    ConsolidationPolicy memory;
    ContextIndexPolicy index;
    ConsolidationOptions consolidation;
    ContextCurationOptions curation;
    MIRA_CHECK(memory.forbidden_markers == forbidden && memory.injection_markers == injection);
    MIRA_CHECK(index.forbidden_markers == forbidden);
    MIRA_CHECK(consolidation.forbidden_markers == forbidden &&
               consolidation.injection_markers == injection);
    MIRA_CHECK(curation.forbidden_markers == forbidden && curation.injection_markers == injection);
    memory.forbidden_markers.front() = "custom";
    memory.injection_markers.clear();
    MIRA_CHECK(index.forbidden_markers == forbidden);
    MIRA_CHECK(consolidation.injection_markers == injection);
    MIRA_CHECK(default_forbidden_content_markers() == forbidden);
    MIRA_CHECK(default_injection_content_markers() == injection);
    return 0;
}

int marker_configuration_fails_before_external_work() {
    class MemoryProbe final : public IMemory {
      public:
        mutable unsigned queries = 0;
        unsigned writes = 0;
        Result<MemoryQueryResult> query(const MemoryQuery &) const override {
            ++queries;
            return MemoryQueryResult{};
        }
        Result<std::optional<MemoryRecord>> get(MemoryId, const MemoryScope &) const override {
            return std::optional<MemoryRecord>{};
        }
        Result<MemoryMutationResult> apply(const MemoryMutation &) override {
            ++writes;
            return Error{};
        }
        Result<MemoryCompactionResult> compact(const MemoryScope &) override { return Error{}; }
        Result<ErasureResult> erase(const ErasureRequest &) override { return Error{}; }
    } memory;
    class ModelProbe final : public IConsolidationModel {
      public:
        unsigned calls = 0;
        std::vector<MemoryCandidate> propose(std::span<const EventEnvelope>,
                                             const MemoryScope &) override {
            ++calls;
            return {};
        }
    } model;
    for (bool forbidden : {false, true}) {
        ConsolidationPolicy policy;
        (forbidden ? policy.forbidden_markers : policy.injection_markers).push_back("");
        auto valid = policy.validate();
        MIRA_CHECK(!valid);
        MIRA_CHECK(valid.error().code == ErrorCode::InvalidArgument);
        MIRA_CHECK(valid.error().domain == "mira.memory");
        MIRA_CHECK(valid.error().domain_code ==
                   static_cast<std::int32_t>(MemoryDomainCode::InvalidMutation));
        MIRA_CHECK(valid.error().safe_message == (forbidden
                                                      ? "forbidden markers must not be empty"
                                                      : "injection markers must not be empty"));
        MemoryConsolidator consolidator(policy, &model);
        MIRA_CHECK(!consolidator.consolidate(memory, {}, MemoryScope{}, Timestamp::now()));
        MIRA_CHECK(!consolidator.consolidate_candidates(memory, {MemoryCandidate{}}, MemoryScope{},
                                                        Timestamp::now()));
    }
    MIRA_CHECK(model.calls == 0 && memory.queries == 0 && memory.writes == 0);

    // Empty lists remain explicit, valid configuration; an empty entry does not.
    ConsolidationPolicy memory_policy;
    ContextIndexPolicy index;
    ConsolidationOptions consolidation;
    ContextCurationOptions curation;
    memory_policy.forbidden_markers.clear();
    memory_policy.injection_markers.clear();
    index.forbidden_markers.clear();
    consolidation.forbidden_markers.clear();
    consolidation.injection_markers.clear();
    curation.forbidden_markers.clear();
    curation.injection_markers.clear();
    MIRA_CHECK(memory_policy.validate() && index.validate() && consolidation.validate() &&
               curation.validate());
    index.forbidden_markers.push_back("");
    consolidation.injection_markers.push_back("");
    curation.forbidden_markers.push_back("");
    MIRA_CHECK(!index.validate() && !consolidation.validate() && !curation.validate());
    return 0;
}

int expect_simple_error(const Result<void> &result, std::string_view domain,
                        std::string_view message) {
    MIRA_CHECK(!result);
    MIRA_CHECK(result.error().code == ErrorCode::InvalidArgument);
    MIRA_CHECK(result.error().domain == domain);
    MIRA_CHECK(result.error().domain_code == 0);
    MIRA_CHECK(!result.error().retryable);
    MIRA_CHECK(result.error().safe_message == message);
    MIRA_CHECK(!result.error().diagnostic_artifact && !result.error().operation_id);
    return 0;
}

int local_error_bindings_keep_the_public_contract() {
    WorkingContextMergeOptions merge;
    merge.max_items_per_section = 0;
    MIRA_CHECK(expect_simple_error(merge.validate(), "mira.context.working_context",
                                   "item bound per section is out of range") == 0);
    MIRA_CHECK(expect_simple_error(WorkingContextDelta{}.validate(), "mira.context.working_context",
                                   "working context delta requires the fork base snapshot id") ==
               0);
    WorkingContextTriggerPolicy trigger;
    trigger.watermark_interval = 0;
    MIRA_CHECK(expect_simple_error(trigger.validate(), "mira.context_working_context_auto",
                                   "watermark_interval must be positive") == 0);
    WorkingContextPromotionPolicy promotion;
    promotion.max_candidates_per_run = 0;
    MIRA_CHECK(expect_simple_error(promotion.validate(), "mira.context.working_context_promotion",
                                   "candidate bound must be positive") == 0);
    ConsolidationOptions consolidation;
    consolidation.max_statements_per_kind = 0;
    MIRA_CHECK(expect_simple_error(consolidation.validate(), "mira.context.consolidation",
                                   "statement bound per kind is out of range") == 0);
    ContextCurationOptions curation;
    curation.max_items_per_section = 0;
    MIRA_CHECK(expect_simple_error(curation.validate(), "mira.context.curator",
                                   "item bound per section is out of range") == 0);
    return 0;
}
} // namespace

int main() {
    MIRA_CHECK(defaults_are_stable_and_independently_owned() == 0);
    MIRA_CHECK(marker_configuration_fails_before_external_work() == 0);
    MIRA_CHECK(local_error_bindings_keep_the_public_contract() == 0);
    return 0;
}
