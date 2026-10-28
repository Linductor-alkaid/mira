#pragma once

// Shared fixtures for the M28 interaction-coordinator IVA test matrix
// (DEC-068; milestone plan docs/plans/m28-interaction-coordinator-first-stage.md
// §4 frozen contract semantics, §6 gates M28-G1..M28-G8). Consumers:
// tests/m28/m28_input_admission_test.cpp, tests/m28/m28_input_ledger_test.cpp,
// tests/m28/m28_routing_test.cpp, tests/m28/m28_result_association_test.cpp,
// tests/m28/m28_context_ownership_test.cpp, tests/m28/m28_lifecycle_test.cpp.
//
// Everything here pins the §4 frozen surface: the closed input stage set, the
// closed routing intent set, the mira.interaction error domain and the
// closed mira.interaction.*.v1 event family.

#include "../support/fake_environment.hpp"
#include "../support/test.hpp"

#include <mira/event_store.hpp>
#include <mira/interaction_coordinator.hpp>
#include <mira/runtime.hpp>

#include <kairo/executor.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mira::m28 {

// The capability the coordinator requires by default.
inline constexpr std::string_view kCapability = "mira.interaction.input";

// A principal that holds one capability grant. The grant never expires
// unless the test says so.
[[nodiscard]] inline PrincipalContext make_principal(std::string_view capability = kCapability,
                                                     GrantVersion identity_version = 1) {
    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.host_id = HostInstanceId::generate();
    principal.auth_strength = AuthenticationStrength::Strong;
    principal.authenticated_at = std::chrono::system_clock::now();
    principal.identity_version = identity_version;
    CapabilityGrant grant;
    grant.capability = std::string(capability);
    grant.source = GrantSource::Host;
    grant.version = 1;
    grant.expires_at = std::chrono::system_clock::time_point{};
    principal.grants.push_back(grant);
    return principal;
}

// A principal without any grant: the fail-closed case.
[[nodiscard]] inline PrincipalContext make_ungranted_principal() {
    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.host_id = HostInstanceId::generate();
    principal.auth_strength = AuthenticationStrength::Session;
    principal.identity_version = 1;
    return principal;
}

// One recorded delivery of the recording sink.
struct Delivery final {
    std::string input_id;
    TaskId task_id;
    std::string content;
};

// A sink that records the deliveries instead of running a loop.
class RecordingSink final : public IUserInputSink {
  public:
    Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) override {
        if (refuse) {
            return interaction_error(ErrorCode::Unavailable, InteractionDomainCode::SinkUnavailable,
                                     "the recording sink refuses the delivery");
        }
        deliveries.push_back(Delivery{envelope.input_id, task_id, envelope.content});
        return Result<void>{};
    }

    bool refuse = false;
    std::vector<Delivery> deliveries;
};

// The host fixture: one executor, one runtime, one event store and one
// coordinator. The shutdown order is the documented one: the coordinator
// stops producing work, the runtime stops, and the executor shuts down from
// the host thread.
class CoordinatorFixture final {
  public:
    explicit CoordinatorFixture(CoordinatorConfig config = CoordinatorConfig{}) {
        kairo::ExecutorConfig executor_config;
        executor_config.min_threads = 2;
        executor_config.max_threads = 2;
        executor_config.queue_capacity = 32;
        (void)executor_.initialize(executor_config);
        runtime_ = std::make_unique<MiraRuntime>(RuntimeConfig{2, 16, 64});
        (void)runtime_->initialize();
        events_ = std::make_shared<MemoryEventStore>();
        environment_ = std::make_shared<test::FakeEnvironment>();
        config_ = std::move(config);
    }

    ~CoordinatorFixture() {
        coordinator_.reset();
        const auto shutdown = runtime_->request_shutdown();
        if (shutdown.has_value()) {
            (void)shutdown.value().outcome(std::chrono::seconds(5));
        }
        (void)runtime_->finish_shutdown();
        (void)executor_.shutdown(true);
    }

    // Builds and initializes the coordinator. The route resolver seam is
    // bound to the fixture so a test can install its own suggestions.
    [[nodiscard]] Result<void> start() {
        coordinator_ = std::make_unique<InteractionCoordinator>(config_, *runtime_, executor_);
        coordinator_->set_event_store(events_, RuntimeId::generate());
        coordinator_->set_environment_provider([this]() -> Result<std::shared_ptr<IEnvironment>> {
            return std::shared_ptr<IEnvironment>(environment_);
        });
        coordinator_->set_route_resolver(
            [this](const InputEnvelope &envelope,
                   const PrincipalState &state) -> std::optional<RouteSuggestion> {
                if (!resolver) {
                    return std::nullopt;
                }
                return resolver(envelope, state);
            });
        return coordinator_->initialize();
    }

    [[nodiscard]] Result<InputDisposition> submit(const InputEnvelope &envelope) {
        return coordinator_->submit_input(envelope);
    }

    [[nodiscard]] InputEnvelope envelope(std::string input_id, std::string content,
                                         const PrincipalContext &principal) {
        InputEnvelope value;
        value.principal = principal;
        value.input_id = std::move(input_id);
        value.content = std::move(content);
        return value;
    }

    // The event types of the durable interaction ledger partition, in the
    // order they were appended.
    [[nodiscard]] std::vector<std::string> ledger_event_types() const {
        std::vector<std::string> types;
        EventQuery query;
        query.session_id = interaction_ledger_partition();
        while (true) {
            const auto page = events_->read(query);
            if (!page) {
                break;
            }
            for (const auto &envelope : page.value().events) {
                types.push_back(envelope.payload.type);
            }
            if (!page.value().has_more || page.value().events.empty()) {
                break;
            }
            query.after_sequence = page.value().events.back().session_sequence;
        }
        return types;
    }

    // The payloads of the durable interaction ledger partition, in order.
    [[nodiscard]] std::vector<EventPayload> ledger_payloads() const {
        std::vector<EventPayload> payloads;
        EventQuery query;
        query.session_id = interaction_ledger_partition();
        while (true) {
            const auto page = events_->read(query);
            if (!page) {
                break;
            }
            for (const auto &envelope : page.value().events) {
                payloads.push_back(envelope.payload);
            }
            if (!page.value().has_more || page.value().events.empty()) {
                break;
            }
            query.after_sequence = page.value().events.back().session_sequence;
        }
        return payloads;
    }

    kairo::Executor executor_;
    std::unique_ptr<MiraRuntime> runtime_;
    std::unique_ptr<InteractionCoordinator> coordinator_;
    std::shared_ptr<MemoryEventStore> events_;
    std::shared_ptr<test::FakeEnvironment> environment_;
    CoordinatorConfig config_;
    // The host-side route suggestion seam of the test.
    RouteResolver resolver;
};

// Finds the registry entry of one task in a principal projection.
[[nodiscard]] inline const PrincipalTaskEntry *find_task(const PrincipalState &state,
                                                         TaskId task_id) {
    for (const auto &task : state.tasks) {
        if (task.task_id == task_id) {
            return &task;
        }
    }
    return nullptr;
}

} // namespace mira::m28
