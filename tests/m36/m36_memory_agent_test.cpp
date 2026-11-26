#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/context_contracts.hpp>
#include <mira/context_retrieval.hpp>

#include <iostream>
#include <string>

namespace {
using namespace mira;
using namespace mira::testing;

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((ordinal >> ((index % 8) * 8)) & 0xFFU);
    }
    return Id128{bytes};
}

int scopes_and_work_revisions_are_isolated() {
    MemoryAgentFixture fixture;
    fixture.start();
    auto first = fixture.binding("volume task");
    auto second = fixture.binding("network task");
    MIRA_CHECK(fixture.service->bind(first));
    MIRA_CHECK(fixture.service->bind(second));
    MIRA_CHECK(fixture.seed(first, "volume=30"));
    MIRA_CHECK(fixture.seed(second, "network=offline"));
    MemoryAgentRequest request;
    request.task = first.work.task_id;
    request.query = "volume";
    auto handle = fixture.service->submit(request, memory_context(first.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &result = handle.value().result.get();
    MIRA_CHECK(result && result.value().candidates.size() == 1);
    const auto supplier = fixture.service->context_supplier();
    auto snapshot = supplier(memory_request(first.work), memory_context(first.work));
    MIRA_CHECK(snapshot && snapshot.value().current());
    MIRA_CHECK(snapshot.value().candidates.size() == 1);
    auto other = supplier(memory_request(second.work), memory_context(second.work));
    MIRA_CHECK(other && other.value().candidates.empty());
    MIRA_CHECK(fixture.service->revise(first.work.task_id, 1, 2));
    MIRA_CHECK(!snapshot.value().current());
    MIRA_CHECK(!fixture.service->result(handle.value().operation));
    auto foreign = first;
    foreign.work.task_id = TaskId::generate();
    foreign.scopes.front().tenant_id = TenantId::generate().to_string();
    MIRA_CHECK(!fixture.service->bind(foreign));
    MIRA_CHECK(memory_user_scope(first.principal).subject_id !=
               first.principal.user_id.to_string());
    return 0;
}

int terminal_event_recording_uses_existing_write_and_approval_gates() {
    MemoryAgentFixture fixture;
    fixture.start(
        [](const MemoryAgentInput &, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            proposal.memories = {{MemoryKind::Preference, {"tone=concise", {0}}},
                                 {MemoryKind::EnvironmentFact, {"invented=verified", {0}}}};
            return proposal;
        });
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MIRA_CHECK(fixture.fact(binding, "volume", "30"));
    MIRA_CHECK(fixture.service->retire(binding.work.task_id));
    MemoryAgentRequest request;
    request.mode = MemoryAgentMode::Record;
    request.task = binding.work.task_id;
    request.write_scope = binding.scopes.front();
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &result = handle.value().result.get();
    MIRA_CHECK(result);
    // DEC-115: the model-recorded preference writes automatically at its
    // Unverified source trust level.
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::Applied) == 2);
    MIRA_CHECK(result.value().consolidation.pending_approval.empty());
    MIRA_CHECK(result.value().consolidation.count_of(CandidateDisposition::RejectedInvalid) == 1);
    MIRA_CHECK(result.value().consolidation.entries.back().reason_code == "kind-policy");
    auto snapshot = fixture.service->context_supplier()(memory_request(binding.work),
                                                        memory_context(binding.work));
    MIRA_CHECK(!snapshot);
    MemoryQuery query;
    query.scopes = binding.scopes;
    query.text = "volume";
    auto recalled = fixture.memory->query(query);
    MIRA_CHECK(recalled && recalled.value().records.size() == 1);
    MIRA_CHECK(recalled.value().records.front().verification == MemoryVerification::Verified);
    return 0;
}

int session_facts_keep_their_kind_and_merge_evidence() {
    MemoryAgentFixture fixture;
    MemoryAgentConfig config;
    config.consolidation.allow_unverified_session_facts = true;
    config.recall_groups = {{{MemoryKind::EnvironmentFact}, 8, 4096},
                            {{MemoryKind::Preference}, 8, 4096}};
    fixture.start(
        [](const MemoryAgentInput &input, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            if (input.mode == MemoryAgentMode::Record) {
                proposal.memories = {
                    {MemoryKind::EnvironmentFact,
                     {"The office uses Atlas Wi-Fi", {0}},
                     MemoryClaim{"office.network_name", "Atlas"}},
                    {MemoryKind::EnvironmentFact,
                     {"Atlas is the office network", {1}},
                     MemoryClaim{"Office.network_name", "atlas"}},
                    {MemoryKind::EnvironmentFact,
                     {"The office network is Nova", {1}},
                     MemoryClaim{"office.network_name", "Nova"}},
                    {MemoryKind::Preference,
                     {"Prefer quiet office notifications\xE3\x80\x82", {0}}},
                    {MemoryKind::Preference, {"Prefer quiet office notifications", {1}}},
                };
            }
            return proposal;
        },
        config);
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    for (const auto *text : {"Atlas office network", "Atlas or Nova office network"})
        MIRA_CHECK(fixture.events->append({EventId::generate(),
                                           fixture.runtime,
                                           binding.work.session_id,
                                           binding.work.task_id,
                                           {"user_observation", text}}));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Record;
    request.write_scope = binding.scopes.front();
    const auto record = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(record && record.value().result.wait_for(3s) == std::future_status::ready);
    const auto &recorded = record.value().result.get();
    MIRA_CHECK(recorded && recorded.value().consolidation.entries.size() == 5);
    const auto &entries = recorded.value().consolidation.entries;
    MIRA_CHECK(entries[1].reason_code == "evidence-appended" &&
               entries[2].reason_code == "alternative-preserved" &&
               entries[4].reason_code == "evidence-appended");

    MemoryQuery query;
    query.scopes = binding.scopes;
    query.claim_key = "office.network_name";
    const auto facts = fixture.memory->query(query);
    MIRA_CHECK(facts && facts.value().records.size() == 2);
    for (const auto &fact : facts.value().records) {
        MIRA_CHECK(fact.kind == MemoryKind::EnvironmentFact &&
                   fact.verification == MemoryVerification::Unverified);
        if (memory_claim_key(fact).value == "atlas")
            MIRA_CHECK(fact.provenance.size() == 2 && !fact.alternative_of);
        else
            MIRA_CHECK(memory_claim_key(fact).value == "nova" && fact.alternative_of);
    }
    query.claim_key = "prefer quiet office notifications";
    const auto preferences = fixture.memory->query(query);
    MIRA_CHECK(preferences && preferences.value().records.size() == 1 &&
               preferences.value().records.front().provenance.size() == 2);

    request.mode = MemoryAgentMode::Recall;
    request.write_scope.reset();
    request.query = "office";
    const auto recall = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(recall && recall.value().result.wait_for(3s) == std::future_status::ready);
    const auto &recalled = recall.value().result.get();
    MIRA_CHECK(recalled && recalled.value().candidates.size() == 2);
    const auto &fact_block =
        std::get<TextPart>(recalled.value().candidates[0].content.front()).text;
    const auto &preference_block =
        std::get<TextPart>(recalled.value().candidates[1].content.front()).text;
    MIRA_CHECK(fact_block.find("Atlas Wi-Fi") != std::string::npos &&
               fact_block.find("[unverified]") != std::string::npos &&
               fact_block.find("quiet office notifications") == std::string::npos);
    MIRA_CHECK(preference_block.find("quiet office notifications") != std::string::npos &&
               preference_block.find("Atlas") == std::string::npos);
    return 0;
}

int compression_requires_real_citations_and_matching_protocol_prefix() {
    MemoryAgentFixture fixture;
    fixture.start(
        [](const MemoryAgentInput &, const OperationContext &) -> Result<MemoryAgentProposal> {
            MemoryAgentProposal proposal;
            proposal.summary = {{"old decision: volume stays below 30", {0, 1}}};
            return proposal;
        });
    const auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.mode = MemoryAgentMode::Compress;
    request.history = {
        history_text("volume below 30"), history_text("acknowledged", ModelRole::Assistant),
        history_text("new correction: 20"), history_text("latest answer", ModelRole::Assistant)};
    request.protected_items = 2;
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    MIRA_CHECK(handle.value().result.get());
    MIRA_CHECK(handle.value().result.get().value().projection_artifact.has_value());
    const auto artifact = *handle.value().result.get().value().projection_artifact;
    FileArtifactStore artifacts(fixture.root / "artifacts");
    MIRA_CHECK(artifacts.open(artifact));
    const auto transform = fixture.service->history_transformer(binding.work.task_id);
    auto compacted = transform(request.history);
    MIRA_CHECK(compacted && compacted.value().size() == 3);
    MIRA_CHECK(compacted.value().front().authority == Sensitivity::Public);
    MIRA_CHECK(prompt_digest(std::vector<ModelInputItem>{compacted.value().back()}) ==
               prompt_digest(std::vector<ModelInputItem>{request.history.back()}));
    auto changed = request.history;
    changed.front() = history_text("different source");
    auto untouched = transform(changed);
    MIRA_CHECK(untouched && untouched.value().size() == changed.size());
    MIRA_CHECK(fixture.service->revise(binding.work.task_id, 1, 1));
    MIRA_CHECK(transform(request.history).value().size() == request.history.size());
    MIRA_CHECK(fixture.service->erase_scope(binding.scopes.front()));
    MIRA_CHECK(!artifacts.open(artifact));
    MIRA_CHECK(fixture.service->close());
    fixture.start();
    MIRA_CHECK(!fixture.service->bind(binding));
    MIRA_CHECK(!artifacts.open(artifact));
    return 0;
}
int recall_renders_one_framed_block_and_dedupes_expansions() {
    // DEC-108: a scripted proposal re-queries the same records so the
    // expansion leg must dedupe against the first batch instead of doubling
    // the block.
    MemoryAgentProposal scripted;
    scripted.queries = {"volume"};
    MemoryAgentFixture fixture;
    fixture.start(
        [&scripted](const MemoryAgentInput &,
                    const OperationContext &) -> Result<MemoryAgentProposal> { return scripted; });
    auto binding = fixture.binding();
    MIRA_CHECK(fixture.service->bind(binding));
    // Two distinct claims (a same-claim second seed would supersede the
    // first) that both lexically match the query "volume".
    MIRA_CHECK(fixture.seed(binding, "volume=30"));
    MIRA_CHECK(fixture.seed(binding, "volume_level=high"));
    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "volume";
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &result = handle.value().result.get();
    MIRA_CHECK(result);
    MIRA_CHECK(result.value().candidates.size() == 1);
    const auto &block = result.value().candidates.front();
    const auto &text = std::get<TextPart>(block.content.front()).text;
    const auto header_end = text.find('\n');
    MIRA_CHECK(header_end != std::string::npos);
    MIRA_CHECK(text.substr(0, header_end) == "[recalled memory v1|count=2]");
    MIRA_CHECK(text.find("\n- volume=30") != std::string::npos);
    MIRA_CHECK(text.find("\n- volume_level=high") != std::string::npos);
    // The framed block is one retrieved-memory item: strictest sensitivity of
    // the selected records, merged provenance, deterministic id derived from
    // the operation and the record count, no wire role.
    MIRA_CHECK(block.kind == ContextItemKind::RetrievedMemory);
    MIRA_CHECK(block.authority == ContextAuthority::RetrievedMemory);
    MIRA_CHECK(!block.wire_role.has_value());
    // The block takes the strictest sensitivity of the selected records;
    // consolidated records default to Internal.
    MIRA_CHECK(std::get<TextPart>(block.content.front()).sensitivity == Sensitivity::Internal);
    MIRA_CHECK(block.provenance.size() == 2);
    MIRA_CHECK(block.sequence == 0);
    MIRA_CHECK(block.id ==
               ContextItemId{context_asset_id_from_seed("mira.memory.recall|" +
                                                        result.value().operation.to_string() + "|2")
                                 .value});
    MIRA_CHECK(block.validate().has_value());

    // A memory_ids selection citing an unknown record is an invalid proposal:
    // the job fails closed instead of recalling anything.
    scripted.queries.clear();
    scripted.selected_memories = {MemoryId::generate()};
    auto invalid_handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(invalid_handle &&
               invalid_handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &invalid_result = invalid_handle.value().result.get();
    MIRA_CHECK(!invalid_result);

    // A recall with no matching records settles with no candidates at all.
    scripted.selected_memories.clear();
    MemoryAgentRequest unmatched = request;
    unmatched.query = "totally-unrelated-zzz";
    auto empty_handle = fixture.service->submit(unmatched, memory_context(binding.work));
    MIRA_CHECK(empty_handle &&
               empty_handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &empty_result = empty_handle.value().result.get();
    MIRA_CHECK(empty_result && empty_result.value().candidates.empty());
    return 0;
} // DEC-111: the recall embedder turns the query into a vector; a paraphrase
// with no shared lexical token is answered by the vector leg alone, and an
// embed failure degrades the result without failing the recall.
int recall_embedder_sets_the_query_vector_and_degrades() {
    class ScriptedEmbedder final : public IEmbeddingProvider {
      public:
        EmbedderId embedder_id() const override { return id; }
        Result<std::vector<float>> embed(std::string_view) override {
            ++calls;
            if (fail) {
                return make_memory_error(MemoryDomainCode::IndexDegraded, "embedder down");
            }
            return std::vector<float>{1.0F, 0.0F};
        }
        EmbedderId id = EmbedderId::generate();
        bool fail = false;
        int calls = 0;
    };

    auto embedder = std::make_shared<ScriptedEmbedder>();
    MemoryAgentFixture fixture;
    fixture.start({}, {}, embedder);
    auto binding = fixture.binding("tone task");
    MIRA_CHECK(fixture.service->bind(binding));
    // Seed like the fixture does, keeping the record id so a vector can be
    // attached for the paraphrase proof.
    MemoryCandidate candidate;
    candidate.proposed.id = MemoryId::generate();
    candidate.proposed.scope = binding.scopes.front();
    candidate.proposed.kind = MemoryKind::EnvironmentFact;
    candidate.proposed.statement = "用户偏好简洁回复";
    candidate.proposed.provenance = {EventId::generate()};
    candidate.evidence = candidate.proposed.provenance;
    candidate.proposed.verification = MemoryVerification::Verified;
    candidate.proposed.confidence = 0.9F;
    const auto consolidated = MemoryConsolidator{}.consolidate_candidates(
        *fixture.memory, {candidate}, binding.scopes.front(), Timestamp::now());
    MIRA_CHECK(consolidated && consolidated.value().count_of(CandidateDisposition::Applied) == 1);

    // Manual vector for the seeded statement: the query paraphrase shares
    // no lexical token, so only the vector leg can bridge.
    MIRA_CHECK(fixture.memory
                   ->index_embedding(candidate.proposed.id, embedder->embedder_id(),
                                     std::vector<float>{1.0F, 0.0F})
                   .has_value());

    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "别太啰嗦";
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &recalled = handle.value().result.get();
    MIRA_CHECK(recalled && !recalled.value().degraded);
    MIRA_CHECK(recalled.value().candidates.size() == 1);
    MIRA_CHECK(embedder->calls == 1);

    // An embed failure only drops the vector leg: the recall still settles,
    // and the result is marked degraded.
    embedder->fail = true;
    auto failed_handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(failed_handle &&
               failed_handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &degraded = failed_handle.value().result.get();
    MIRA_CHECK(degraded && degraded.value().degraded);
    MIRA_CHECK(embedder->calls == 2);

    // The seam is frozen at initialize(): late configuration is refused.
    auto late = std::make_shared<ScriptedEmbedder>();
    MIRA_CHECK(!fixture.service->set_recall_embedder(late));
    MIRA_CHECK(!fixture.service->set_recall_embedder(nullptr));
    return 0;
}
// DEC-112 wiring: the work binding's label prefilter narrows the recall
// universe; a work without labels sees both records.
int recall_applies_the_work_label_prefilter() {
    MemoryAgentFixture fixture;
    fixture.start();
    auto binding = fixture.binding("labeled task");
    binding.labels = {"ops"};
    MIRA_CHECK(fixture.service->bind(binding));
    const auto seed_labeled = [&](std::uint64_t ordinal, std::string statement,
                                  std::vector<std::string> labels) {
        MemoryCandidate candidate;
        candidate.proposed.id = MemoryId{ordinal_id(ordinal)};
        candidate.proposed.scope = binding.scopes.front();
        candidate.proposed.kind = MemoryKind::EnvironmentFact;
        candidate.proposed.statement = std::move(statement);
        candidate.proposed.labels = std::move(labels);
        candidate.proposed.provenance = {EventId{ordinal_id(ordinal + 500)}};
        candidate.evidence = candidate.proposed.provenance;
        candidate.proposed.verification = MemoryVerification::Verified;
        candidate.proposed.confidence = 0.9F;
        const auto consolidated = MemoryConsolidator{}.consolidate_candidates(
            *fixture.memory, {candidate}, binding.scopes.front(), Timestamp::now());
        return consolidated && consolidated.value().count_of(CandidateDisposition::Applied) == 1;
    };
    MIRA_CHECK(seed_labeled(10, "volume=30 ops note", {"ops"}));
    MIRA_CHECK(seed_labeled(11, "network=offline tone note", {}));

    MemoryAgentRequest request;
    request.task = binding.work.task_id;
    request.query = "note";
    auto handle = fixture.service->submit(request, memory_context(binding.work));
    MIRA_CHECK(handle && handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &filtered = handle.value().result.get();
    MIRA_CHECK(filtered && filtered.value().candidates.size() == 1);
    MIRA_CHECK(!filtered.value().degraded);
    // The framed block carries only the labeled record.
    const auto &filtered_block =
        std::get<TextPart>(filtered.value().candidates.front().content.front()).text;
    MIRA_CHECK(filtered_block.find("volume=30 ops note") != std::string::npos);
    MIRA_CHECK(filtered_block.find("network=offline tone note") == std::string::npos);

    // A second work without labels sees both records.
    auto open_binding = fixture.binding("open task");
    open_binding.scopes = binding.scopes;
    MIRA_CHECK(fixture.service->bind(open_binding));
    MemoryAgentRequest open_request;
    open_request.task = open_binding.work.task_id;
    open_request.query = "note";
    auto open_handle = fixture.service->submit(open_request, memory_context(open_binding.work));
    MIRA_CHECK(open_handle && open_handle.value().result.wait_for(3s) == std::future_status::ready);
    const auto &open_result = open_handle.value().result.get();
    MIRA_CHECK(open_result && open_result.value().candidates.size() == 1);
    // One framed block carries all matching records for the label-less work.
    const auto &open_block =
        std::get<TextPart>(open_result.value().candidates.front().content.front()).text;
    MIRA_CHECK(open_block.find("volume=30 ops note") != std::string::npos);
    MIRA_CHECK(open_block.find("network=offline tone note") != std::string::npos);

    // Invalid label sets are refused at bind time.
    auto bad = fixture.binding("bad labels");
    bad.labels.assign(17, "x");
    MIRA_CHECK(!fixture.service->bind(bad));
    return 0;
}

} // namespace
int main() {
    if (recall_applies_the_work_label_prefilter())
        return 1;
    if (recall_embedder_sets_the_query_vector_and_degrades())
        return 1;
    if (scopes_and_work_revisions_are_isolated())
        return 1;
    if (recall_renders_one_framed_block_and_dedupes_expansions())
        return 1;
    if (terminal_event_recording_uses_existing_write_and_approval_gates())
        return 1;
    if (session_facts_keep_their_kind_and_merge_evidence())
        return 1;
    return compression_requires_real_citations_and_matching_protocol_prefix();
}
