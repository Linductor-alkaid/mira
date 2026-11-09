#pragma once

#include "support/conversation_loop_support.hpp"
#include <mira/interaction_coordinator.hpp>
#include <mira/memory_agent.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <condition_variable>
#include <filesystem>

namespace mira::testing {
using namespace std::chrono_literals;

inline ModelRequest memory_request(const AgentLoopSpec &spec, std::string text = "current task") {
    ModelRequest request;
    request.contract_version = {1, 0};
    request.request_id = ModelRequestId::generate();
    request.operation_id = OperationId::generate();
    request.task_id = spec.task_id;
    request.task_epoch = spec.task_epoch;
    request.profile_id = spec.profile_id;
    ModelInputItem system;
    system.role = ModelRole::System;
    system.content.emplace_back(TextPart{"host policy"});
    request.input.push_back(std::move(system));
    ModelInputItem item;
    item.content.emplace_back(TextPart{std::move(text)});
    request.input.push_back(std::move(item));
    request.generation.max_output_tokens = 64;
    return request;
}
inline OperationContext memory_context(const AgentLoopSpec &spec) {
    auto context = make_control_context();
    context.task = spec.task_id;
    context.session = spec.session_id;
    context.task_epoch = spec.task_epoch;
    context.deadline = std::chrono::steady_clock::now() + 5s;
    return context;
}
inline ModelInputItem history_text(std::string text, ModelRole role = ModelRole::User) {
    ModelInputItem item;
    item.role = role;
    item.content.emplace_back(TextPart{std::move(text)});
    return item;
}

class MemoryAgentFixture final {
  public:
    MemoryAgentFixture() {
        kairo::ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.queue_capacity = 16;
        if (!executor.initialize(config))
            std::abort();
        root =
            std::filesystem::temp_directory_path() / ("mira-m36-" + Id128::generate().to_string());
        std::filesystem::create_directories(root);
        events = std::make_shared<FileEventStore>(root / "events");
        SqliteMemoryStoreOptions options;
        options.path = root / "memory.db";
        auto opened = SqliteMemoryStore::open(executor, options);
        if (!opened)
            std::abort();
        memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};
        principal.tenant_id = TenantId::generate();
        principal.user_id = UserId::generate();
        principal.identity_version = 1;
    }
    ~MemoryAgentFixture() {
        if (service)
            (void)service->close();
        service.reset();
        (void)memory->close();
        memory.reset();
        (void)executor.shutdown(true);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void start(MemoryAgentProcessor processor = {}, MemoryAgentConfig config = {}) {
        config.artifacts = std::make_shared<FileArtifactStore>(root / "artifacts");
        service = std::make_unique<MemoryAgentService>(executor, memory, events, runtime,
                                                       std::move(processor), config);
        if (!service->initialize())
            std::abort();
    }
    MemoryWorkBinding binding(std::string goal = "fixture task") const {
        MemoryWorkBinding binding;
        binding.principal = principal;
        binding.work = {TaskId::generate(), SessionId::generate(), 1, std::move(goal),
                        ModelProfileId::generate()};
        binding.environment_epoch = 1;
        binding.scopes = {memory_session_scope(principal, binding.work.session_id)};
        return binding;
    }
    bool fact(const MemoryWorkBinding &binding, std::string key, std::string value) {
        AppendRequest request;
        request.event_id = EventId::generate();
        request.runtime_id = runtime;
        request.session_id = binding.work.session_id;
        request.task_id = binding.work.task_id;
        request.payload = {
            "TaskFactVerified",
            to_json_string(JsonValue::Object{{"key", std::move(key)}, {"value", std::move(value)}}),
            EventClass::State};
        return events->append(request).has_value();
    }
    bool seed(const MemoryWorkBinding &binding, std::string statement) {
        MemoryCandidate candidate;
        candidate.proposed.id = MemoryId::generate();
        candidate.proposed.scope = binding.scopes.front();
        candidate.proposed.kind = MemoryKind::EnvironmentFact;
        candidate.proposed.statement = std::move(statement);
        candidate.proposed.provenance = {EventId::generate()};
        candidate.evidence = candidate.proposed.provenance;
        candidate.proposed.verification = MemoryVerification::Verified;
        candidate.proposed.confidence = 0.9F;
        auto result = MemoryConsolidator{}.consolidate_candidates(
            *memory, {candidate}, binding.scopes.front(), Timestamp::now());
        return result && result.value().count_of(CandidateDisposition::Applied) == 1;
    }
    kairo::Executor executor;
    std::filesystem::path root;
    std::shared_ptr<SqliteMemoryStore> memory;
    std::shared_ptr<FileEventStore> events;
    RuntimeId runtime = RuntimeId::generate();
    PrincipalContext principal;
    std::unique_ptr<MemoryAgentService> service;
};

struct MemoryAgentProbe final {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    bool ignore_cancel = false;
    bool throws = false;
    MemoryAgentProposal proposal;
    MemoryAgentProcessor processor() {
        return [this](const MemoryAgentInput &,
                      const OperationContext &context) -> Result<MemoryAgentProposal> {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            while (!released && (ignore_cancel || !context.cancelled_or_expired(Timestamp::now())))
                cv.wait_for(lock, 2ms);
            if (throws)
                throw std::runtime_error("private model exception");
            return proposal;
        };
    }
    bool wait() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 3s, [&] { return entered; });
    }
    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
} // namespace mira::testing
