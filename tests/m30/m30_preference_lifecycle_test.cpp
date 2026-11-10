#include "m30_preference_support.hpp"
using namespace mira;
using namespace mira::testing;

class FaultArtifacts final : public IArtifactStore {
  public:
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &spec) override {
        return store.begin(spec);
    }
    Result<ArtifactDescriptor> commit(ArtifactWriter &writer) override {
        auto committed = store.commit(writer);
        if (after_commit)
            after_commit();
        if (fail_commit)
            return make_memory_error(MemoryDomainCode::StoreUnavailable,
                                     "injected artifact commit");
        return committed;
    }
    Result<ArtifactReader> open(const ArtifactDescriptor &ref) const override {
        return store.open(ref);
    }
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &request) override {
        if (fail_erase)
            return make_memory_error(MemoryDomainCode::StoreUnavailable, "injected artifact erase");
        return store.erase(request);
    }
    MemoryArtifactStore store;
    bool fail_commit = false;
    bool fail_erase = false;
    std::function<void()> after_commit;
};

int storage_failure_fences_and_interrupts_do_not_write() {
    for (bool lost_commit : {false, true}) {
        MemoryAgentFixture storage;
        const auto artifacts = std::make_shared<FaultArtifacts>();
        artifacts->fail_commit = lost_commit;
        artifacts->fail_erase = !lost_commit;
        MemoryAgentConfig config;
        config.artifacts = artifacts;
        MemoryAgentService service(storage.executor, storage.memory, storage.events,
                                   storage.runtime, {}, config);
        MIRA_CHECK(service.initialize());
        auto binding = storage.binding();
        binding.scopes = {memory_user_scope(storage.principal)};
        MIRA_CHECK(service.bind(binding));
        bool cancelled = false;
        auto context = memory_context(binding.work);
        context.cancellation_requested = [&] { return cancelled; };
        if (!lost_commit)
            artifacts->after_commit = [&] { cancelled = true; };
        auto future = storage.executor.submit_auto([&] {
            return service.propose_preference({MutationId::generate(), "tone=brief", {}, {}},
                                              context);
        });
        const auto failed = future.get();
        MIRA_CHECK(!failed && failed.error().code == ErrorCode::DataLoss);
        auto retry = storage.executor.submit_auto([&] {
            return service.propose_preference({MutationId::generate(), "tone=brief", {}, {}},
                                              memory_context(binding.work));
        });
        MIRA_CHECK(!retry.get());
        MIRA_CHECK(artifacts->store.size() == 1);
        MemoryQuery query;
        query.scopes = binding.scopes;
        MIRA_CHECK(storage.memory->query(query).value().records.empty());
        MIRA_CHECK(service.close());
    }
    PreferenceFixture host;
    auto proposed = host.propose("tone=brief");
    MIRA_CHECK(proposed);
    bool cancelled = false;
    host.fault->before_append = [&](const AppendRequest &request) {
        if (request.payload.type == "mira.memory-agent.preference-approval-intent.v1")
            cancelled = true;
    };
    auto context = host.context();
    context.cancellation_requested = [&] { return cancelled; };
    auto stopped = host.run([&] {
        return host.service->decide_preference(host.storage.principal, proposed.value().id,
                                               proposed.value().digest,
                                               ConfirmationDecision::Approve, context);
    });
    MIRA_CHECK(!stopped && stopped.error().code == ErrorCode::Cancelled);
    MIRA_CHECK(host.query().value().records.empty());
    MIRA_CHECK(host.preview(proposed.value().id).value().state ==
               PreferenceProposalState::Approving);
    MIRA_CHECK(
        !host.run([&] { return host.service->invalidate_scope(host.binding.scopes.front()); }));
    MIRA_CHECK(host.decide(proposed.value()));
    return 0;
}

int main() {
    MIRA_CHECK(storage_failure_fences_and_interrupts_do_not_write() == 0);
    MemoryAgentConfig config;
    config.preference_proposal_ttl = 1s;
    config.max_preference_proposals = 1;
    config.consolidation.approval_required_kinds.clear();
    PreferenceFixture host(config);
    auto context = host.context();
    context.deadline.reset();
    MIRA_CHECK(!host.run([&] {
        return host.service->propose_preference({MutationId::generate(), "tone=brief", {}, {}},
                                                context);
    }));
    context = host.context();
    context.cancellation_requested = [] { return true; };
    const auto cancelled = host.run([&] {
        return host.service->propose_preference({MutationId::generate(), "tone=brief", {}, {}},
                                                context);
    });
    MIRA_CHECK(!cancelled && cancelled.error().code == ErrorCode::Cancelled);
    const auto pending = host.propose("tone=brief");
    MIRA_CHECK(pending);
    const auto full = host.propose("theme=dark");
    MIRA_CHECK(!full && full.error().code == ErrorCode::ResourceExhausted);
    auto clock = host.storage.executor.submit_delayed(1100, [] {});
    clock.future.get();
    MIRA_CHECK(host.preview(pending.value().id).value().state == PreferenceProposalState::Expired);
    MIRA_CHECK(host.preview(pending.value().id).value().statement.empty());
    const auto expired = host.decide(pending.value());
    MIRA_CHECK(!expired && expired.error().code == ErrorCode::DeadlineExceeded);
    MIRA_CHECK(host.query().value().records.empty());
    MIRA_CHECK(host.service->close());
    MIRA_CHECK(!host.propose("voice=quiet"));
    return 0;
}
