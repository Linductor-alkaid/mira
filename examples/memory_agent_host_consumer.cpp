#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/interaction_coordinator.hpp>
#include <mira/memory_agent.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <kairo/executor.hpp>

#include <filesystem>
#include <iostream>

namespace {
using namespace mira;
using namespace std::chrono_literals;

class InspectingProvider final : public IModelProvider {
  public:
    InspectingProvider() {
        profile_.id = ModelProfileId::generate();
        profile_.endpoint_origin = "https://offline-memory.invalid";
        profile_.api_prefix = "/v1";
        profile_.model_selector = "offline-memory-fixture";
        profile_.capabilities.text = {true, CapabilityEvidence::FixtureVerified, "offline only"};
    }
    const ModelProfile &profile() const override { return profile_; }
    Result<ModelResponse> infer(const ModelRequest &request, const OperationContext &,
                                const ProviderInferOptions &) override {
        requests.push_back(request);
        ModelResponse response;
        response.contract_version = request.contract_version;
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_.id;
        response.status = ModelCompletionStatus::Completed;
        MessageOutput message;
        OutputTextPart answer;
        answer.text = "offline request inspected";
        message.content.emplace_back(std::move(answer));
        response.output.emplace_back(std::move(message));
        response.usage.quality = UsageQuality::ProviderReported;
        response.usage.input_tokens = 16;
        response.usage.output_tokens = 4;
        return response;
    }
    std::vector<ModelRequest> requests;

  private:
    ModelProfile profile_;
};

OperationContext context_for(const AgentLoopSpec &spec) {
    auto context = make_control_context();
    context.task = spec.task_id;
    context.session = spec.session_id;
    context.task_epoch = spec.task_epoch;
    context.deadline = std::chrono::steady_clock::now() + 5s;
    return context;
}
std::string text(const ModelRequest &request) {
    std::string result;
    for (const auto &item : request.input)
        for (const auto &part : item.content)
            if (const auto *value = std::get_if<TextPart>(&part))
                result += value->text + "\n";
    return result;
}

bool run(kairo::Executor &executor, const std::filesystem::path &root) {
    auto events = std::make_shared<FileEventStore>(root / "events");
    SqliteMemoryStoreOptions options;
    options.path = root / "memory.db";
    auto opened = SqliteMemoryStore::open(executor, options);
    if (!opened)
        return false;
    auto memory = std::shared_ptr<SqliteMemoryStore>{std::move(opened).value()};
    const auto runtime_id = RuntimeId::generate();
    MemoryAgentService service(executor, memory, events, runtime_id);
    if (!service.initialize())
        return false;
    MiraRuntime runtime;
    if (!runtime.initialize())
        return false;
    struct RuntimeOwner final {
        MiraRuntime &runtime;
        ~RuntimeOwner() {
            if (auto shutdown = runtime.request_shutdown(); shutdown)
                (void)shutdown.value().outcome(5s);
            (void)runtime.finish_shutdown();
        }
    } owner{runtime};
    InteractionCoordinator coordinator({}, runtime, executor);
    coordinator.set_event_store(events, runtime_id);
    auto environment = std::make_shared<adapters::simulator::SimulatorEnvironment>();
    coordinator.set_environment_provider([environment]() -> Result<std::shared_ptr<IEnvironment>> {
        return std::shared_ptr<IEnvironment>{environment};
    });
    coordinator.set_route_resolver(
        [](const InputEnvelope &, const PrincipalState &) -> std::optional<RouteSuggestion> {
            RouteSuggestion suggestion;
            suggestion.intent = RouteIntent::NewTask;
            return suggestion;
        });
    if (!coordinator.initialize())
        return false;
    PrincipalContext principal;
    principal.tenant_id = TenantId::generate();
    principal.user_id = UserId::generate();
    principal.host_id = HostInstanceId::generate();
    principal.identity_version = 1;
    principal.auth_strength = AuthenticationStrength::Strong;
    CapabilityGrant grant;
    grant.capability = "mira.interaction.input";
    grant.source = GrantSource::Host;
    grant.version = 1;
    principal.grants.push_back(grant);
    const auto input = [&](std::string id, std::string content) {
        InputEnvelope envelope;
        envelope.principal = principal;
        envelope.input_id = std::move(id);
        envelope.content = std::move(content);
        return envelope;
    };
    auto first = coordinator.submit_input(input("first", "plan volume adjustment"));
    auto second = coordinator.submit_input(input("second", "inspect network configuration"));
    if (!first || !second || !first.value().task_id || !second.value().task_id ||
        first.value().task_id == second.value().task_id) {
        std::cerr << "memory host: input routing failed\n";
        return false;
    }
    auto provider = std::make_shared<InspectingProvider>();
    std::vector<MemoryWorkBinding> bindings;
    for (const auto &disposition : {first.value(), second.value()}) {
        const auto snapshot = runtime.task_snapshot(*disposition.task_id);
        if (!snapshot)
            return false;
        MemoryWorkBinding binding;
        binding.principal = principal;
        binding.work = {*disposition.task_id, *disposition.session_id, snapshot.value().epoch,
                        disposition.input_id == "first" ? "plan volume adjustment"
                                                        : "inspect network configuration",
                        provider->profile().id};
        binding.scopes = {memory_session_scope(principal, binding.work.session_id)};
        if (auto bound = service.bind(binding); !bound) {
            std::cerr << "memory host: binding failed: " << bound.error().safe_message << '\n';
            return false;
        }
        bindings.push_back(std::move(binding));
    }
    ModelRouter router;
    router.register_profile(std::make_shared<const ModelProfile>(provider->profile()));
    ModelGateway gateway(executor, router, nullptr, PriceTable{});
    gateway.register_provider(provider);
    gateway.set_context_supplier(service.context_supplier());
    ConversationLoop first_loop(gateway), second_loop(gateway);
    first_loop.set_event_store(events, runtime_id, bindings[0].work.session_id);
    second_loop.set_event_store(events, runtime_id, bindings[1].work.session_id);
    first_loop.set_history_transformer(service.history_transformer(bindings[0].work.task_id));
    second_loop.set_history_transformer(service.history_transformer(bindings[1].work.task_id));
    if (!coordinator.register_sink(
            bindings[0].work.task_id,
            service.input_sink(bindings[0].work.task_id,
                               std::make_shared<ConversationLoopInputSink>(first_loop))) ||
        !coordinator.register_sink(
            bindings[1].work.task_id,
            service.input_sink(bindings[1].work.task_id,
                               std::make_shared<ConversationLoopInputSink>(second_loop)))) {
        std::cerr << "memory host: sink registration failed\n";
        return false;
    }
    // Verified host facts feed the existing pipeline, then scoped recall.
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        AppendRequest fact;
        fact.event_id = EventId::generate();
        fact.runtime_id = runtime_id;
        fact.session_id = binding.work.session_id;
        fact.task_id = binding.work.task_id;
        fact.payload = {"TaskFactVerified",
                        i == 0 ? R"({"key":"volume","value":"30"})"
                               : R"({"key":"network","value":"offline"})",
                        EventClass::State};
        if (!events->append(fact))
            return false;
        MemoryAgentRequest request;
        request.task = binding.work.task_id;
        request.mode = MemoryAgentMode::Record;
        request.write_scope = binding.scopes.front();
        auto recorded = service.submit(request, context_for(binding.work));
        if (!recorded || !recorded.value().result.get()) {
            std::cerr << "memory host: recording failed\n";
            return false;
        }
        request.mode = MemoryAgentMode::Recall;
        request.query = i == 0 ? "volume" : "network";
        auto recalled = service.submit(request, context_for(binding.work));
        if (!recalled || !recalled.value().result.get()) {
            std::cerr << "memory host: recall failed\n";
            return false;
        }
    }
    ModelRequest before;
    before.task_id = bindings[0].work.task_id;
    before.task_epoch = bindings[0].work.task_epoch;
    auto old = service.context_supplier()(before, context_for(bindings[0].work));
    coordinator.set_route_resolver(
        [task = bindings[0].work.task_id](
            const InputEnvelope &, const PrincipalState &) -> std::optional<RouteSuggestion> {
            RouteSuggestion suggestion;
            suggestion.intent = RouteIntent::SupplementTask;
            suggestion.task_id = task;
            return suggestion;
        });
    auto correction = coordinator.submit_input(input("correction", "target volume must be 20"));
    if (!old || !correction || correction.value().task_id != bindings[0].work.task_id ||
        old.value().current()) {
        std::cerr << "memory host: correction fence failed\n";
        return false;
    }
    auto answer1 = first_loop.run(bindings[0].work, context_for(bindings[0].work));
    auto answer2 = second_loop.run(bindings[1].work, context_for(bindings[1].work));
    if (!answer1 || !answer2 || answer1.value().outcome != ConversationOutcome::Answered ||
        answer2.value().outcome != ConversationOutcome::Answered ||
        provider->requests.size() != 2) {
        std::cerr << "memory host: loop failed: "
                  << (answer1 ? answer1.value().safe_summary : answer1.error().safe_message) << "; "
                  << (answer2 ? answer2.value().safe_summary : answer2.error().safe_message)
                  << '\n';
        return false;
    }
    const bool correct =
        text(provider->requests[0]).find("target volume must be 20") != std::string::npos &&
        text(provider->requests[1]).find("network=offline") != std::string::npos &&
        text(provider->requests[1]).find("target volume") == std::string::npos &&
        environment->executed_inputs().empty();
    coordinator.shutdown();
    if (!correct)
        std::cerr << "memory host: scoped request inspection failed\n";
    return service.close().has_value() && correct;
}
} // namespace

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    if (!executor.initialize(config))
        return 1;
    const auto root = std::filesystem::temp_directory_path() /
                      ("mira-memory-host-" + Id128::generate().to_string());
    std::filesystem::create_directories(root);
    const bool passed = run(executor, root);
    (void)executor.shutdown(true);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << (passed ? "memory host: two work scopes, correction fence and final admission OK\n"
                         : "memory host failed\n");
    return passed ? 0 : 1;
}
