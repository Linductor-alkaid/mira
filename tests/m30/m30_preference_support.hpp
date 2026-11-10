#pragma once
#include "support/m36_support.hpp"
#include "support/test.hpp"

namespace mira::testing {
class PreferenceFaultStore final : public IEventStore {
  public:
    explicit PreferenceFaultStore(std::shared_ptr<IEventStore> source)
        : source_(std::move(source)) {}
    Result<AppendReceipt> append(const AppendRequest &request) override {
        if (before_append)
            before_append(request);
        if (request.payload.type == fail_type && remaining > 0) {
            --remaining;
            if (after_write) {
                auto written = source_->append(request);
                if (!written)
                    return written.error();
            }
            return make_memory_error(MemoryDomainCode::StoreUnavailable,
                                     "injected lost acknowledgement");
        }
        auto receipt = source_->append(request);
        if (receipt && request.payload.type == weak_type)
            receipt.value().achieved = Durability::Buffered;
        return receipt;
    }
    Result<std::vector<AppendReceipt>>
    append_batch(std::span<const AppendRequest> requests) override {
        return source_->append_batch(requests);
    }
    Result<EventPage> read(const EventQuery &query) const override { return source_->read(query); }
    Result<StoreRecoveryReport> recover(const RecoveryOptions &options) override {
        return source_->recover(options);
    }
    Result<void> flush(Durability durability) override { return source_->flush(durability); }
    std::string fail_type;
    std::string weak_type;
    int remaining = 1;
    bool after_write = false;
    std::function<void(const AppendRequest &)> before_append;

  private:
    std::shared_ptr<IEventStore> source_;
};

class PreferenceFixture final {
  public:
    explicit PreferenceFixture(MemoryAgentConfig bounds = {}) : config(std::move(bounds)) {
        CapabilityGrant grant;
        storage.principal.auth_strength = AuthenticationStrength::Strong;
        grant.capability = "mira.memory.manage";
        grant.version = storage.principal.identity_version;
        storage.principal.grants = {grant};
        binding = storage.binding("answer preferences");
        binding.scopes = {memory_user_scope(storage.principal),
                          memory_session_scope(storage.principal, binding.work.session_id)};
        start();
    }
    ~PreferenceFixture() { (void)service->close(); }
    template <typename Function> auto run(Function function) {
        auto future = storage.executor.submit_auto(std::move(function));
        return future.get();
    }
    void start() {
        config.artifacts =
            std::make_shared<FileArtifactStore>(storage.root / "preference-artifacts");
        fault = std::make_shared<PreferenceFaultStore>(storage.events);
        service =
            std::make_unique<MemoryAgentService>(storage.executor, storage.memory, fault,
                                                 storage.runtime, MemoryAgentProcessor{}, config);
        if (!service->initialize() || !service->bind(binding))
            std::abort();
    }
    bool reopen() {
        if (!service->close())
            return false;
        service.reset();
        if (!storage.memory->close())
            return false;
        storage.events = std::make_shared<FileEventStore>(storage.root / "events");
        if (!storage.events->recover({}))
            return false;
        SqliteMemoryStoreOptions options;
        options.path = storage.root / "memory.db";
        auto opened = SqliteMemoryStore::open(storage.executor, options);
        if (!opened)
            return false;
        storage.memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};
        start();
        return true;
    }
    OperationContext context() const { return memory_context(binding.work); }
    Result<PreferenceProposal> propose(std::string statement,
                                       MutationId id = MutationId::generate()) {
        return run([&, statement = std::move(statement), id] {
            return service->propose_preference({id, statement, {}, {}}, context());
        });
    }
    Result<PreferenceProposal>
    decide(const PreferenceProposal &proposal,
           ConfirmationDecision decision = ConfirmationDecision::Approve) {
        return run([&] {
            return service->decide_preference(storage.principal, proposal.id, proposal.digest,
                                              decision, context());
        });
    }
    Result<PreferenceProposal> preview(MutationId id) {
        return run([&] { return service->preference_proposal(storage.principal, id, context()); });
    }
    Result<MemoryQueryResult> query() const {
        MemoryQuery query;
        query.scopes = {binding.scopes.front()};
        return storage.memory->query(query);
    }
    MemoryAgentFixture storage;
    MemoryAgentConfig config;
    MemoryWorkBinding binding;
    std::shared_ptr<PreferenceFaultStore> fault;
    std::unique_ptr<MemoryAgentService> service;
};
} // namespace mira::testing
