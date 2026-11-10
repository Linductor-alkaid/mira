#pragma once

#include "m28/m28_support.hpp"
#include "support/m36_support.hpp"

#include <atomic>

namespace mira::testing {
class InteractionMemoryFixture final {
  public:
    InteractionMemoryFixture(MemoryAgentProcessor processor = {}, MemoryAgentConfig config = {}) {
        if (!runtime.initialize())
            std::abort();
        service =
            std::make_shared<MemoryAgentService>(storage.executor, storage.memory, storage.events,
                                                 storage.runtime, std::move(processor), config);
        coordinator = std::make_unique<InteractionCoordinator>(CoordinatorConfig{}, runtime,
                                                               storage.executor);
        coordinator->set_event_store(storage.events, storage.runtime);
        coordinator->set_environment_provider([]() -> Result<std::shared_ptr<IEnvironment>> {
            return std::shared_ptr<IEnvironment>{std::make_shared<test::FakeEnvironment>()};
        });
        if (!coordinator->set_memory_service(service) || !coordinator->initialize())
            std::abort();
    }
    ~InteractionMemoryFixture() {
        (void)coordinator->close();
        (void)service->close();
        const auto stopped = runtime.request_shutdown();
        if (stopped)
            (void)stopped.value().outcome(5s);
        (void)runtime.finish_shutdown();
    }
    InputEnvelope input(std::string id, std::string text = "volume") const {
        InputEnvelope value;
        value.input_id = std::move(id);
        value.content = std::move(text);
        value.principal = principal;
        return value;
    }
    AgentLoopSpec spec(const InputDisposition &input) const {
        const auto snapshot = runtime.task_snapshot(*input.task_id).value();
        return {*input.task_id, *input.session_id, snapshot.epoch, "volume", ModelProfileId{}};
    }
    Result<MemoryAgentResult> finish(TaskId task) const {
        const auto status = coordinator->memory_status(principal, task);
        if (!status)
            return status.error();
        if (status.value().error)
            return *status.value().error;
        if (!status.value().job ||
            status.value().job->result.wait_for(3s) != std::future_status::ready)
            return interaction_error(ErrorCode::DeadlineExceeded,
                                     InteractionDomainCode::InputInvalid,
                                     "test memory job did not settle");
        return status.value().job->result.get();
    }
    MemoryWorkBinding binding(const InputDisposition &input) const {
        MemoryWorkBinding value;
        value.principal = principal;
        value.work = spec(input);
        value.scopes = {memory_session_scope(principal, *input.session_id)};
        return value;
    }
    MemoryAgentFixture storage;
    MiraRuntime runtime{RuntimeConfig{2, 16, 64}};
    PrincipalContext principal = m28::make_principal();
    std::shared_ptr<MemoryAgentService> service;
    std::unique_ptr<InteractionCoordinator> coordinator;
};

inline bool approve_preference(IMemory &memory, const MemoryScope &scope, std::string statement) {
    MemoryCandidate candidate;
    candidate.proposed.id = MemoryId::generate();
    candidate.proposed.kind = MemoryKind::Preference;
    candidate.proposed.scope = scope;
    candidate.proposed.statement = std::move(statement);
    candidate.proposed.provenance = {EventId::generate()};
    candidate.evidence = candidate.proposed.provenance;
    candidate.proposed.verification = MemoryVerification::HumanConfirmed;
    candidate.proposed.confidence = 1.F;
    const auto proposed =
        MemoryConsolidator{}.consolidate_candidates(memory, {candidate}, scope, Timestamp::now());
    return proposed && proposed.value().pending_approval.size() == 1 &&
           MemoryConsolidator::apply_pending(memory, proposed.value().pending_approval.front());
}
} // namespace mira::testing
