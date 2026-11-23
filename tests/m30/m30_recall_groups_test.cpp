#include "m30_support.hpp"

namespace {
using namespace mira;
using namespace mira::testing;

bool seed(IMemory &store, const MemoryScope &scope, MemoryKind kind, std::string statement) {
    MemoryCandidate candidate;
    candidate.proposed.id = MemoryId::generate();
    candidate.proposed.scope = scope;
    candidate.proposed.kind = kind;
    candidate.proposed.statement = std::move(statement);
    candidate.proposed.provenance = {EventId::generate()};
    candidate.proposed.verification = MemoryVerification::Verified;
    candidate.proposed.confidence = 0.9F;
    candidate.evidence = candidate.proposed.provenance;
    auto result = MemoryConsolidator{}.consolidate_candidates(store, {std::move(candidate)}, scope,
                                                              Timestamp::now());
    return result && result.value().count_of(CandidateDisposition::Applied) == 1;
}

int recall_groups_keep_independent_limits_and_scope() {
    MemoryAgentConfig config;
    config.max_records = 6;
    config.max_result_bytes = 612;
    config.recall_groups = {
        {{MemoryKind::Episode}, 1, 256},
        {{MemoryKind::EnvironmentFact}, 3, 100},
        {{MemoryKind::SkillHint}, 2, 256},
    };
    InteractionMemoryFixture host(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.mode == MemoryAgentMode::Recall && !input.records.empty())
                proposal.queries = {"retry", "volume"};
            return proposal;
        },
        config);
    const auto input = host.coordinator->submit_input(host.input("recall"));
    MIRA_CHECK(input && input.value().task_id);
    const auto task = *input.value().task_id;
    const auto binding = host.binding(input.value());
    const auto &scope = binding.scopes.front();
    auto &store = *host.storage.memory;
    MIRA_CHECK(seed(store, scope, MemoryKind::Episode, R"({"outcome":"volume settled"})"));
    MIRA_CHECK(seed(store, scope, MemoryKind::Episode, R"({"outcome":"retry settled"})"));
    MIRA_CHECK(seed(store, scope, MemoryKind::EnvironmentFact,
                    "volume=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    MIRA_CHECK(seed(store, scope, MemoryKind::EnvironmentFact,
                    "retry=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    MIRA_CHECK(seed(store, scope, MemoryKind::SkillHint, "volume adjust hint"));
    MIRA_CHECK(seed(store, scope, MemoryKind::Preference, "tone=volume tone"));

    auto foreign = scope;
    foreign.tenant_id = TenantId::generate().to_string();
    MIRA_CHECK(seed(store, foreign, MemoryKind::EnvironmentFact, "volume=private"));

    MIRA_CHECK(host.coordinator->register_sink(task, std::make_shared<m28::RecordingSink>()));
    const auto result = host.finish(task);
    MIRA_CHECK(result && result.value().degraded && result.value().candidates.size() == 3);
    const auto &candidates = result.value().candidates;
    const auto &episode = std::get<TextPart>(candidates[0].content.front()).text;
    const auto &environment = std::get<TextPart>(candidates[1].content.front()).text;
    const auto &skill = std::get<TextPart>(candidates[2].content.front()).text;
    MIRA_CHECK(episode.find("volume settled") != std::string::npos);
    MIRA_CHECK(episode.find("retry") == std::string::npos);
    MIRA_CHECK(environment.find("volume=aaaa") != std::string::npos);
    MIRA_CHECK(environment.find("retry") == std::string::npos);
    MIRA_CHECK(environment.find("private") == std::string::npos);
    MIRA_CHECK(environment.find("tone") == std::string::npos);
    MIRA_CHECK(skill.find("volume adjust hint") != std::string::npos);
    MIRA_CHECK(skill.starts_with("[recalled memory v1|count=1]"));
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        MIRA_CHECK(candidates[index].validate());
        MIRA_CHECK(candidates[index].sequence == index);
        MIRA_CHECK(std::get<TextPart>(candidates[index].content.front()).text.size() <=
                   config.recall_groups[index].max_bytes);
    }
    MIRA_CHECK(candidates[0].id != candidates[1].id && candidates[1].id != candidates[2].id);
    auto snapshot = host.service->context_supplier()(memory_request(binding.work),
                                                     memory_context(binding.work));
    MIRA_CHECK(snapshot && snapshot.value().current() && snapshot.value().candidates.size() == 3);
    MIRA_CHECK(host.service->invalidate_scope(scope));
    MIRA_CHECK(!snapshot.value().current());

    MemoryAgentRequest request;
    request.task = task;
    request.query = "unmatchedword";
    auto empty = host.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(empty && empty.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(empty.value().result.get() && empty.value().result.get().value().candidates.empty());
    return 0;
}

int invalid_group_layouts_fail_before_work() {
    MemoryAgentFixture fixture;
    for (auto groups : std::vector<std::vector<MemoryRecallGroup>>{
             {{{}, 1, 64}},
             {{{MemoryKind::Episode}, 0, 64}},
             {{{MemoryKind::Episode}, 1, 63}},
             {{{MemoryKind::Episode}, 1, 64}, {{MemoryKind::Episode}, 1, 64}},
             {{{static_cast<MemoryKind>(255)}, 1, 64}},
             {{{MemoryKind::Episode}, 16, 64}, {{MemoryKind::SkillHint}, 1, 64}},
             {{{MemoryKind::Episode}, 1, 32768}, {{MemoryKind::SkillHint}, 1, 64}},
         }) {
        MemoryAgentConfig config;
        config.recall_groups = std::move(groups);
        MemoryAgentService service(fixture.executor, fixture.memory, fixture.events,
                                   fixture.runtime, {}, config);
        const auto initialized = service.initialize();
        MIRA_CHECK(!initialized && initialized.error().code == ErrorCode::InvalidArgument);
    }
    return 0;
}
} // namespace

int main() {
    if (auto result = recall_groups_keep_independent_limits_and_scope(); result != 0)
        return result;
    return invalid_group_layouts_fail_before_work();
}
