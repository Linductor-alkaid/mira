#include <mira/adapters/net/socket_transport.hpp>
#include <mira/adapters/simulator/simulator_environment.hpp>
#include <mira/runtime_baseline.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <mira/workflow_compiler.hpp>
#include <mira/workflow_ir.hpp>
#include <mira/workflow_learning.hpp>
#include <mira/workflow_recovery.hpp>
#include <mira/workflow_runtime.hpp>
#include <mira/workflow_tools.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <memory>
#include <system_error>

#include <executor/executor.hpp>

#ifdef MIRA_CONSUMER_HAS_MBEDTLS
#include <mira/adapters/net/mbedtls_tls.hpp>
#endif

int main() {
    mira::adapters::simulator::SimulatorEnvironment environment{
        mira::adapters::simulator::SimulatorSetup::single_display()};

    mira::ObservationRequest request;
    request.required.screen = true;
    mira::OperationContext context;
    context.operation = mira::OperationId::generate();
    context.started_at = mira::Timestamp::now();
    const auto observation = environment.observe(request, context);
    if (!observation.has_value() || !observation.value().screen.has_value()) {
        return 1;
    }
    mira::RuntimeBaseline runtime;
    if (!runtime.initialize()) {
        return 2;
    }
    const auto submission = runtime.submit({1, 1, 0, mira::BaselineCommandKind::Command});
    if (!submission.admitted) {
        return 3;
    }
    const auto result = runtime.wait(1, std::chrono::seconds(2));
    if (!runtime.request_shutdown()) {
        return 5;
    }
    runtime.finish_shutdown();
    if (result.code != mira::BaselineResultCode::Applied) {
        return 4;
    }

    // Durable state stores: the installed package must export Mira::state_store
    // (with its vendored SQLite closure), not just the public headers.
    executor::Executor store_exec;
    if (!store_exec.initialize(executor::ExecutorConfig{})) {
        return 6;
    }
    {
        std::error_code fs_error;
        const auto root =
            std::filesystem::temp_directory_path(fs_error) / "mira-installed-consumer";
        std::filesystem::create_directories(root, fs_error);

        mira::SqliteStoreOptions checkpoint_options;
        checkpoint_options.path = root / "consumer-checkpoints.db";
        auto checkpoint_store = mira::SqliteCheckpointStore::open(store_exec, checkpoint_options);
        if (!checkpoint_store) {
            return 7;
        }

        mira::SqliteMemoryStoreOptions memory_options;
        memory_options.path = root / "consumer-memory.db";
        auto memory_store = mira::SqliteMemoryStore::open(store_exec, memory_options);
        if (!memory_store) {
            return 8;
        }

        (void)memory_store.value()->close();
        (void)checkpoint_store.value()->close();
        std::filesystem::remove_all(root, fs_error);
    }
    if (store_exec.shutdown(true) != executor::ShutdownResult::Completed) {
        return 9;
    }

    // Network transports: the installed package must export the adapter
    // headers and libraries so a package-only consumer can construct the
    // official production transport stack (GitHub #14). A PEM CA bundle is
    // the caller's responsibility; a missing file must fail closed at
    // initialize() rather than at first use.
    {
        executor::Executor net_exec;
        if (!net_exec.initialize(executor::ExecutorConfig{})) {
            return 10;
        }
        auto secrets = std::make_shared<mira::NullSecretResolver>();
        mira::adapters::net::SocketHttpTransport transport{net_exec, secrets};
        if (!transport.start()) {
            return 11;
        }
        if (!transport.running()) {
            return 12;
        }
        transport.shutdown();
        if (transport.running()) {
            return 13;
        }
#ifdef MIRA_CONSUMER_HAS_MBEDTLS
        mira::adapters::net::MbedTlsChannelFactory tls{
            "/nonexistent/mira-consumer-ca.pem"};
        if (tls.initialize()) {
            return 14; // A missing CA bundle must not initialize.
        }
#endif
        if (net_exec.shutdown(true) != executor::ShutdownResult::Completed) {
            return 15;
        }
    }

    // Workflow contracts (M8): the installed package must export Mira::workflow
    // so a package-only consumer can parse, bind and validate IR documents and
    // workflow tool calls without any runtime execution.
    {
        const char *ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-probe",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "fedcba9876543210fedcba9876543210", "kind": "tool_call",
                 "arguments": {"text": {"$param": "contact"}}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto definition = mira::parse_workflow_definition(ir);
        if (!definition.has_value()) {
            return 16;
        }
        mira::JsonValue input{mira::JsonValue::Object{}};
        input.set("contact", mira::JsonValue{std::string("zhang san")});
        auto bindings = mira::bind_workflow_parameters(definition.value(), input);
        if (!bindings.has_value()) {
            return 17;
        }
        auto arguments = mira::resolve_step_arguments(
            bindings.value(), definition.value().steps.front().arguments);
        if (!arguments.has_value() ||
            *arguments.value().find("text")->as_string() != "zhang san") {
            return 18;
        }
        mira::JsonValue run_arguments{mira::JsonValue::Object{}};
        run_arguments.set("workflow_id",
                          mira::JsonValue{std::string("0123456789abcdef0123456789abcdef")});
        run_arguments.set(
            "ir_digest",
            mira::JsonValue{mira::workflow_definition_digest(definition.value()).to_string()});
        if (!mira::validate_workflow_operation(mira::WorkflowOperation::RunWorkflow,
                                               run_arguments)) {
            return 19;
        }
    }

    // Workflow runtime (M9): the installed package executes a DryRun closed
    // loop over its own Executor and control plane, proving that
    // Mira::workflow's executor dependency resolves for package consumers.
    {
        executor::Executor host_executor;
        executor::ExecutorConfig host_config;
        // 4 threads: one async drive + its monitor + one step dispatch +
        // one spare (M13 resumed drives occupy workers while waiting on
        // step futures, mirroring the test fixture).
        host_config.min_threads = 4;
        host_config.max_threads = 4;
        host_config.queue_capacity = 16;
        if (!host_executor.initialize(host_config)) {
            return 20;
        }
        mira::MiraRuntime runtime;
        if (!runtime.initialize()) {
            return 21;
        }
        auto environment = std::make_shared<mira::adapters::simulator::SimulatorEnvironment>(
            mira::adapters::simulator::SimulatorSetup::single_display());
        const auto session = runtime.open_session(environment);
        if (!session.has_value() ||
            !session.value().command.outcome(std::chrono::seconds(2)).has_value()) {
            return 22;
        }
        mira::WorkflowRuntime workflows(host_executor, runtime, session.value().id, environment);
        const char *ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-dry-run",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "fedcba9876543210fedcba9876543210", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "exists"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto definition = mira::parse_workflow_definition(ir);
        if (!definition.has_value()) {
            return 23;
        }
        mira::JsonValue parameters{mira::JsonValue::Object{}};
        parameters.set("contact", mira::JsonValue{std::string("zhang san")});
        const auto created = workflows.create_run(definition.value(), parameters,
                                                 mira::WorkflowPolicy::DryRun);
        if (!created.has_value()) {
            return 24;
        }
        mira::OperationContext context;
        context.started_at = mira::Timestamp::now();
        const auto result = workflows.execute_run(created.value().run_id, context);
        if (!result.has_value() ||
            result.value().state != mira::WorkflowRunState::Completed) {
            return 25;
        }

        // Workflow intervention (M10): an Interactive run parks in
        // WaitingUser on failure, accepts a run patch in the wait state and
        // resolves the decision point through the installed package.
        const char *interactive_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "0123456789abcdef0123456789abcdef",
            "name": "consumer-interactive",
            "parameters": [
                {"name": "contact", "type": "string", "required": true}
            ],
            "steps": [
                {"step_id": "11111111111111111111111111111111", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "eq",
                                  "value": "blocked"}},
                {"step_id": "22222222222222222222222222222222", "kind": "verify",
                 "verification": {"signal": "run_parameter:contact", "op": "exists"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "interactive"]
        })";
        auto interactive = mira::parse_workflow_definition(interactive_ir);
        if (!interactive.has_value()) {
            return 30;
        }
        const auto started = workflows.create_run(interactive.value(), parameters,
                                                  mira::WorkflowPolicy::Interactive);
        if (!started.has_value()) {
            return 31;
        }
        const auto parked = workflows.execute_run(started.value().run_id, context);
        if (!parked.has_value() ||
            parked.value().state != mira::WorkflowRunState::WaitingUser) {
            return 32;
        }
        mira::WorkflowPatchEntry entry;
        entry.target = mira::WorkflowPatchTarget::RunParameters;
        entry.op = mira::WorkflowPatchOp::Set;
        entry.path = "contact";
        entry.value = mira::JsonValue{std::string("resolved")};
        const auto patched = workflows.patch_run(started.value().run_id,
                                                 mira::WorkflowPatchId::generate(), {entry});
        if (!patched.has_value() || !patched.value().applied ||
            patched.value().view.run_patch_epoch != 1) {
            return 33;
        }
        const auto decision = workflows.pending_decision_request(started.value().run_id);
        if (!decision.has_value() ||
            decision.value().kind != mira::WorkflowDecisionKind::StepFailure) {
            return 34;
        }
        const auto resolved = workflows.resolve_decision(
            started.value().run_id, decision.value().decision_id,
            decision.value().payload_digest, mira::WorkflowDecisionResolution::Accept);
        if (!resolved.has_value()) {
            return 35;
        }
        const auto settled = workflows.wait_run(started.value().run_id, std::chrono::seconds(10));
        if (!settled.has_value() ||
            settled.value().state != mira::WorkflowRunState::Completed) {
            return 36;
        }

        // Workflow compilation (M11): a successful side-effecting run is
        // captured, compiled with the observed value baked as the default,
        // published through the DryRun gate and re-run from the library;
        // induction across two runs then reopens the parameter.
        auto tools = std::make_shared<mira::BuiltinToolRegistry>();
        if (!tools->register_tool(mira::make_wait_tool().spec,
                                  mira::make_wait_tool().handler)) {
            return 37;
        }
        workflows.set_tool_registry(tools);
        const char *compilable_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "ffffffffffffffffffffffffffffffff",
            "name": "consumer-compilable",
            "parameters": [
                {"name": "delay", "type": "integer", "required": false, "default": 1}
            ],
            "steps": [
                {"step_id": "abababababababababababababababab", "kind": "tool_call",
                 "arguments": {"tool": "wait", "duration_ms": {"$param": "delay"}}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto compilable = mira::parse_workflow_definition(compilable_ir);
        if (!compilable.has_value()) {
            return 38;
        }
        mira::JsonValue fast{mira::JsonValue::Object{}};
        fast.set("delay", mira::JsonValue{std::int64_t{5}});
        const auto first = workflows.create_run(compilable.value(), fast,
                                                mira::WorkflowPolicy::Strict);
        if (!first.has_value() ||
            !workflows.execute_run(first.value().run_id, context).has_value()) {
            return 39;
        }
        const auto trajectory = workflows.capture_trajectory(first.value().run_id);
        if (!trajectory.has_value()) {
            return 40;
        }
        mira::WorkflowCompileOptions options;
        options.workflow_id = compilable.value().workflow_id;
        options.name = "consumer-compiled";
        const auto compiled = mira::compile_workflow(trajectory.value(), options);
        if (!compiled.has_value()) {
            return 41;
        }
        const auto published = workflows.publish_validated(compiled.value(), "consumer",
                                                           "bake observed delay",
                                                           first.value().run_id);
        if (!published.has_value() || published.value().idempotent) {
            return 42;
        }
        const auto rerun = workflows.create_run(
            compilable.value().workflow_id, published.value().ir_digest,
            mira::JsonValue{mira::JsonValue::Object{}}, mira::WorkflowPolicy::Strict);
        if (!rerun.has_value() ||
            !workflows.execute_run(rerun.value().run_id, context).has_value()) {
            return 43;
        }
        mira::JsonValue slow{mira::JsonValue::Object{}};
        slow.set("delay", mira::JsonValue{std::int64_t{7}});
        const auto second = workflows.create_run(compilable.value(), slow,
                                                 mira::WorkflowPolicy::Strict);
        if (!second.has_value() ||
            !workflows.execute_run(second.value().run_id, context).has_value()) {
            return 44;
        }
        const auto second_trajectory = workflows.capture_trajectory(second.value().run_id);
        if (!second_trajectory.has_value()) {
            return 45;
        }
        const auto candidates = mira::induce_parameters(
            {trajectory.value(), second_trajectory.value()});
        if (!candidates.has_value() || candidates.value().size() != 1 ||
            candidates.value().front().name != "delay") {
            return 46;
        }
        mira::WorkflowCompileOptions derived;
        derived.workflow_id = mira::WorkflowId::generate();
        derived.name = "consumer-induced";
        const auto induced = mira::compile_workflow(trajectory.value(), derived,
                                                    candidates.value());
        if (!induced.has_value()) {
            return 47;
        }
        const auto induced_publish = workflows.publish_validated(
            induced.value(), "consumer", "induced parameterization", std::nullopt);
        if (!induced_publish.has_value()) {
            return 48;
        }
        mira::JsonValue override_parameters{mira::JsonValue::Object{}};
        override_parameters.set("delay", mira::JsonValue{std::int64_t{9}});
        const auto induced_run = workflows.create_run(
            induced.value().workflow_id, induced_publish.value().ir_digest,
            override_parameters, mira::WorkflowPolicy::Strict);
        if (!induced_run.has_value() ||
            !workflows.execute_run(induced_run.value().run_id, context).has_value()) {
            return 49;
        }

        // Navigation (M12): the host installs an App Model and a screen
        // state provider (the recognition boundary stays host-side); a
        // navigate step resolves through the planner, dispatches its edge
        // action through the tool channel, verifies arrival and writes
        // confidence back into the runtime projection.
        mira::AppModelState home;
        home.id = "home";
        home.page = "Launcher";
        mira::AppModelState inbox;
        inbox.id = "inbox";
        inbox.page = "Inbox";
        mira::AppModelTransition open_inbox;
        open_inbox.id = "t-open-inbox";
        open_inbox.from_state = "home";
        open_inbox.to_state = "inbox";
        mira::JsonValue::Object action;
        action.emplace_back("tool", "consumer_navigate");
        open_inbox.action = mira::JsonValue{std::move(action)};
        open_inbox.costs.latency_ms = 10.0;
        mira::AppModel model;
        model.app_id = "com.example.consumer";
        model.name = "consumer-app";
        model.states = {home, inbox};
        model.transitions = {open_inbox};

        std::string screen = "home";
        const auto installed = workflows.set_navigation_context(
            model, [&screen]() -> std::optional<mira::ScreenStateSnapshot> {
                return mira::ScreenStateSnapshot{screen, 1};
            });
        if (!installed.has_value()) {
            return 50;
        }
        mira::BuiltinToolRegistration navigator;
        navigator.spec.wire_name = "consumer_navigate";
        navigator.spec.description = "consumer navigation edge action";
        navigator.spec.parameters_schema = mira::JsonSchema{mira::parse_json(R"json({
            "type": "object",
            "properties": {},
            "additionalProperties": false
        })json").value()};
        navigator.spec.has_side_effects = true;
        navigator.handler = [&screen](const mira::JsonValue &,
                                      const mira::OperationContext &) -> mira::Result<mira::JsonValue> {
            screen = "inbox";
            return mira::JsonValue{"moved"};
        };
        if (!tools->register_tool(navigator.spec, navigator.handler)) {
            return 51;
        }
        const char *navigating_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
            "name": "consumer-navigating",
            "steps": [
                {"step_id": "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", "kind": "navigate",
                 "arguments": {"target": "inbox"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "dry_run"]
        })";
        auto navigating = mira::parse_workflow_definition(navigating_ir);
        if (!navigating.has_value()) {
            return 52;
        }
        const auto navigated =
            workflows.create_run(navigating.value(), mira::JsonValue{mira::JsonValue::Object{}},
                                 mira::WorkflowPolicy::Strict);
        if (!navigated.has_value()) {
            return 53;
        }
        const auto arrived = workflows.execute_run(navigated.value().run_id, context);
        if (!arrived.has_value() ||
            arrived.value().state != mira::WorkflowRunState::Completed) {
            return 54;
        }
        const auto projection = workflows.app_model_snapshot();
        if (!projection.has_value() ||
            projection.value().transitions.front().confidence.verified_count != 1) {
            return 55;
        }
        // The planner is usable directly from the installed headers too, and
        // the confidence functions are pure: decaying and re-checking the
        // exploration flag needs no runtime at all.
        const auto planned = mira::plan_navigation(
            model, "home", "inbox", mira::NavigationCostProfile{},
            mira::JsonValue{mira::JsonValue::Object{}});
        if (!planned.has_value() || planned.value().transition_ids.size() != 1) {
            return 56;
        }
        const auto decayed = mira::apply_confidence_decay(
            projection.value().transitions.front().confidence,
            projection.value().transitions.front().confidence.last_verified_ms + 604'800'000,
            604'800'000);
        if (!mira::needs_exploration(decayed, 0.4)) {
            return 57;
        }

        // Learning loop (M13): the four-domain organization and the learning
        // contracts are usable straight from the installed headers, and the
        // runtime closes the loop through a host-supplied memory backend.
        if (mira::memory_domain_of(mira::MemoryKind::RecoveryLesson) !=
                mira::MemoryDomain::ProceduralMemory ||
            mira::memory_kinds_of_domain(mira::MemoryDomain::EpisodicMemory).size() != 1) {
            return 58;
        }
        mira::WorkflowEpisodeRecord episode_probe;
        episode_probe.run_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        episode_probe.workflow_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        episode_probe.ir_digest = mira::digest_string("probe").to_string();
        episode_probe.policy = "recoverable";
        episode_probe.outcome = "failed";
        episode_probe.failed_step_id = "cccccccccccccccccccccccccccccccc";
        episode_probe.failure_reason_code = "mira.workflow:6";
        episode_probe.recorded_at_ms = 1;
        auto episode_rebuilt =
            mira::workflow_episode_from_json(mira::workflow_episode_to_json(episode_probe));
        if (!episode_rebuilt.has_value() || !(episode_rebuilt.value() == episode_probe)) {
            return 59;
        }
        mira::WorkflowFailureSignature signature;
        signature.workflow_id = episode_probe.workflow_id;
        signature.step_id = episode_probe.failed_step_id.value_or(std::string{});
        signature.reason_code = episode_probe.failure_reason_code.value_or(
            std::string{"mira.workflow:6"});
        mira::MemoryScope learning_scope;
        learning_scope.kind = mira::MemoryScopeKind::Agent;
        learning_scope.subject_id = "consumer.learning";
        const auto probe_record = mira::episode_to_memory_record(
            episode_probe, learning_scope, {mira::EventId::generate()},
            std::chrono::system_clock::now());
        if (!probe_record.validate().has_value()) {
            return 60;
        }
        const auto retrieval = mira::failure_retrieval_query(signature, learning_scope);
        if (!retrieval.has_value() || retrieval.value().exact_terms.size() != 2 ||
            !retrieval.value().query_embedding.empty()) {
            return 61;
        }

        class ConsumerMemory final : public mira::IMemory {
          public:
            mira::Result<mira::MemoryQueryResult>
            query(const mira::MemoryQuery &query) const override {
                mira::MemoryQueryResult result;
                for (const auto &record : records_) {
                    const bool scope_ok = std::any_of(
                        query.scopes.begin(), query.scopes.end(),
                        [&](const mira::MemoryScope &scope) { return scope == record.scope; });
                    const bool kind_ok =
                        !query.kinds.has_value() ||
                        std::find(query.kinds->begin(), query.kinds->end(), record.kind) !=
                            query.kinds->end();
                    if (!scope_ok || !kind_ok) {
                        continue;
                    }
                    bool terms_ok = true;
                    for (const auto &term : query.exact_terms) {
                        if (record.statement.find(term) == std::string::npos) {
                            terms_ok = false;
                        }
                    }
                    if (terms_ok) {
                        result.records.push_back(record);
                        result.scores.push_back(1.0);
                    }
                }
                result.quality.exact_leg_ran = true;
                return result;
            }
            mira::Result<std::optional<mira::MemoryRecord>>
            get(mira::MemoryId record) const override {
                for (const auto &stored : records_) {
                    if (stored.id == record) {
                        return std::optional<mira::MemoryRecord>{stored};
                    }
                }
                return std::optional<mira::MemoryRecord>{};
            }
            mira::Result<mira::MemoryMutationResult>
            apply(const mira::MemoryMutation &mutation) override {
                if (auto valid = mutation.validate(); !valid.has_value()) {
                    return valid.error();
                }
                ++applies;
                records_.push_back(mutation.proposed);
                mira::MemoryMutationResult result;
                result.record = mutation.proposed.id;
                return result;
            }
            mira::Result<mira::MemoryCompactionResult>
            compact(const mira::MemoryScope &) override {
                return mira::MemoryCompactionResult{};
            }
            mira::Result<mira::ErasureResult> erase(const mira::ErasureRequest &) override {
                mira::ErasureResult result;
                result.status = mira::ErasureStatus::Complete;
                return result;
            }
            std::vector<mira::MemoryRecord> records_;
            int applies = 0;
        };
        auto learning = std::make_shared<ConsumerMemory>();
        // Learning writes need traceable provenance: wire an event store so
        // settlements anchor the memory records (DEC-030 §2).
        workflows.set_event_store(std::make_shared<mira::MemoryEventStore>());
        if (!workflows.set_learning_context(learning, learning_scope).has_value()) {
            return 62;
        }
        int failing_dispatches = 0;
        mira::BuiltinToolRegistration flaky;
        flaky.spec.wire_name = "consumer_flaky";
        flaky.spec.description = "consumer learning failure source";
        flaky.spec.parameters_schema = mira::JsonSchema{mira::parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        flaky.spec.has_side_effects = false;
        flaky.handler = [&failing_dispatches](const mira::JsonValue &,
                                              const mira::OperationContext &) -> mira::Result<mira::JsonValue> {
            if (++failing_dispatches <= 2) {
                mira::Error error;
                error.code = mira::ErrorCode::PlatformError;
                error.domain = "mira.consumer";
                error.safe_message = "scripted consumer failure";
                return error;
            }
            return mira::JsonValue{"sent"};
        };
        if (!tools->register_tool(flaky.spec, flaky.handler)) {
            return 63;
        }
        const char *learning_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "dddddddddddddddddddddddddddddddd",
            "name": "consumer-learning",
            "steps": [
                {"step_id": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "kind": "tool_call",
                 "arguments": {"tool": "consumer_flaky", "payload": "p"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "recoverable"]
        })";
        auto learnable = mira::parse_workflow_definition(learning_ir);
        if (!learnable.has_value()) {
            return 64;
        }
        const auto failed_run = workflows.create_run(learnable.value(),
                                                     mira::JsonValue{mira::JsonValue::Object{}},
                                                     mira::WorkflowPolicy::Strict);
        const auto failed_outcome =
            failed_run.has_value()
                ? workflows.execute_run(failed_run.value().run_id, context)
                : mira::Result<mira::WorkflowRunResult>{mira::Error{}};
        if (!failed_outcome.has_value() ||
            failed_outcome.value().state != mira::WorkflowRunState::Failed) {
            return 65;
        }
        const auto recovered_run = workflows.create_run(
            learnable.value(), mira::JsonValue{mira::JsonValue::Object{}},
            mira::WorkflowPolicy::Recoverable);
        if (!recovered_run.has_value()) {
            return 66;
        }
        const auto escalated = workflows.execute_run(recovered_run.value().run_id, context);
        if (!escalated.has_value() ||
            escalated.value().state != mira::WorkflowRunState::WaitingAgent) {
            return 67;
        }
        const auto continuation = workflows.agent_continuation(recovered_run.value().run_id);
        if (!continuation.has_value() ||
            continuation.value().relevant_lessons.size() != 1 ||
            continuation.value().relevant_lessons[0].kind != mira::MemoryKind::Episode) {
            return 68;
        }
        const auto resumed = workflows.resume_run(recovered_run.value().run_id);
        if (!resumed.has_value()) {
            std::cerr << "resume failed: " << resumed.error().safe_message << '\n';
            return 69;
        }
        const auto recovered_outcome =
            workflows.wait_run(recovered_run.value().run_id, std::chrono::seconds(10));
        if (!recovered_outcome.has_value()) {
            std::cerr << "wait failed: " << recovered_outcome.error().safe_message << '\n';
            return 74;
        }
        if (recovered_outcome.value().state != mira::WorkflowRunState::Completed) {
            std::cerr << "recovered state: "
                      << mira::workflow_run_state_name(recovered_outcome.value().state)
                      << " summary: " << recovered_outcome.value().safe_summary << '\n';
            return 75;
        }
        const auto lesson = workflows.record_recovery_lesson(recovered_run.value().run_id);
        if (!lesson.has_value() || !lesson.value().resumed_without_patch ||
            lesson.value().outcome != "recovered") {
            return 70;
        }
        if (learning->records_.size() != 3 || learning->applies != 3) {
            return 71;
        }
        mira::MemoryQuery lesson_query;
        lesson_query.scopes = {learning_scope};
        lesson_query.kinds = std::vector<mira::MemoryKind>{mira::MemoryKind::RecoveryLesson};
        lesson_query.exact_terms = {learnable.value().workflow_id.to_string()};
        const auto lessons_found = learning->query(lesson_query);
        if (!lessons_found.has_value() || lessons_found.value().records.size() != 1) {
            return 72;
        }
        auto parsed_lesson =
            mira::recovery_lesson_from_record(lessons_found.value().records.front());
        if (!parsed_lesson.has_value() || !(parsed_lesson.value() == lesson.value())) {
            return 73;
        }

        // Recovery orchestration (M14, DEC-031): a package-only consumer
        // assembles the recovery orchestrator over its own ModelGateway and a
        // scripted provider; a WaitingAgent run is repaired through the
        // model-synthesized patch and the audit event lands in the same
        // event store (start_run -> wait_run -> notify -> attempt -> wait).
        class ConsumerRecoveryProvider final : public mira::IModelProvider {
          public:
            mira::ModelProfile profile_;
            std::vector<mira::ModelResponse> script;
            std::size_t consumed = 0;

            [[nodiscard]] const mira::ModelProfile &profile() const override { return profile_; }
            [[nodiscard]] mira::Result<mira::ModelResponse>
            infer(const mira::ModelRequest &request, const mira::OperationContext &,
                  const mira::ProviderInferOptions &) override {
                if (consumed >= script.size()) {
                    return mira::make_model_error(mira::ModelDomainCode::ModelResourceExhausted,
                                                  "consumer recovery script exhausted", false,
                                                  request.operation_id);
                }
                mira::ModelResponse response = script[consumed++];
                response.request_id = request.request_id;
                response.operation_id = request.operation_id;
                response.profile_id = request.profile_id;
                return response;
            }
        };
        auto recovery_provider = std::make_shared<ConsumerRecoveryProvider>();
        recovery_provider->profile_.id = mira::ModelProfileId::generate();
        recovery_provider->profile_.display_name = "consumer-recovery";
        recovery_provider->profile_.version = mira::SemanticVersion{1, 0, 0};
        recovery_provider->profile_.dialect = mira::ProtocolDialect::OpenAIResponsesV1;
        recovery_provider->profile_.endpoint_origin = "https://recovery.consumer.test";
        recovery_provider->profile_.model_selector = "consumer-recovery-model";
        recovery_provider->profile_.capabilities.text = {true, mira::CapabilityEvidence::FixtureVerified, ""};
        recovery_provider->profile_.capabilities.strict_json_schema = {
            true, mira::CapabilityEvidence::FixtureVerified, ""};
        mira::ModelResponse decision_response;
        decision_response.contract_version = mira::SchemaVersion{1, 0};
        decision_response.status = mira::ModelCompletionStatus::Completed;
        mira::MessageOutput decision_message;
        mira::OutputTextPart decision_text;
        decision_text.text = "{\"action\":\"need_user\",\"used_lessons\":[],\"rationale\":\"consumer probe\"}";
        decision_message.content.emplace_back(std::move(decision_text));
        decision_response.output.emplace_back(std::move(decision_message));
        decision_response.requested_model = "consumer-recovery-model";
        recovery_provider->script.push_back(std::move(decision_response));
        mira::ModelRouter recovery_router;
        recovery_router.register_profile(
            std::make_shared<const mira::ModelProfile>(recovery_provider->profile_));
        mira::ModelGateway recovery_gateway{host_executor, recovery_router, nullptr,
                                            mira::PriceTable{}, mira::ModelGatewayConfig{}};
        recovery_gateway.register_provider(recovery_provider);
        mira::WorkflowRecoveryConfig recovery_config;
        recovery_config.profile_id = recovery_provider->profile_.id;
        mira::WorkflowRecoveryOrchestrator recovery{
            host_executor, workflows, runtime, recovery_gateway, session.value().id,
            recovery_config};
        recovery.set_event_store(std::make_shared<mira::MemoryEventStore>());
        mira::BuiltinToolRegistration recovery_flaky;
        recovery_flaky.spec.wire_name = "consumer_recovery_flaky";
        recovery_flaky.spec.description = "consumer recovery failure source";
        recovery_flaky.spec.parameters_schema = mira::JsonSchema{mira::parse_json(R"json({
            "type": "object",
            "properties": {"payload": {"type": "string"}},
            "additionalProperties": false
        })json").value()};
        recovery_flaky.spec.has_side_effects = false;
        recovery_flaky.handler = [](const mira::JsonValue &,
                                    const mira::OperationContext &) -> mira::Result<mira::JsonValue> {
            mira::Error error;
            error.code = mira::ErrorCode::PlatformError;
            error.domain = "mira.consumer";
            error.safe_message = "scripted recovery failure";
            return error;
        };
        if (!tools->register_tool(recovery_flaky.spec, recovery_flaky.handler)) {
            return 76;
        }
        const char *recovery_ir = R"({
            "schema_version": {"major": 1, "minor": 0},
            "workflow_id": "ababababababababababababababab12",
            "name": "consumer-recovery",
            "steps": [
                {"step_id": "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcd12", "kind": "tool_call",
                 "arguments": {"tool": "consumer_recovery_flaky", "payload": "p"}}
            ],
            "default_policy": "strict",
            "allowed_policies": ["strict", "recoverable"]
        })";
        auto recoverable = mira::parse_workflow_definition(recovery_ir);
        if (!recoverable.has_value()) {
            return 77;
        }
        const auto escalating = workflows.create_run(
            recoverable.value(), mira::JsonValue{mira::JsonValue::Object{}},
            mira::WorkflowPolicy::Recoverable);
        if (!escalating.has_value()) {
            return 78;
        }
        const auto wait_escalated =
            workflows.execute_run(escalating.value().run_id, context);
        if (!wait_escalated.has_value() ||
            wait_escalated.value().state != mira::WorkflowRunState::WaitingAgent) {
            return 79;
        }
        const auto attempt = recovery.attempt_recovery(escalating.value().run_id);
        if (!attempt.has_value() ||
            attempt.value().outcome != mira::WorkflowRecoveryOutcome::DeferredToHost ||
            attempt.value().reason_code != "decision-need-user" ||
            !attempt.value().model_request_id.has_value() ||
            attempt.value().lessons_offered != 0) {
            return 80;
        }
        const auto after_attempt = workflows.run_snapshot(escalating.value().run_id);
        if (recovery_provider->consumed != 1 ||
            after_attempt.value().state != mira::WorkflowRunState::WaitingAgent) {
            return 81;
        }
        const auto recovery_report = recovery.shutdown();
        if (!recovery_report.clean) {
            return 82;
        }

        const auto report = workflows.shutdown();
        if (!report.clean) {
            return 26;
        }
        const auto shutdown = runtime.request_shutdown();
        if (!shutdown.has_value() ||
            !shutdown.value().outcome(std::chrono::seconds(5)).has_value()) {
            return 27;
        }
        if (!runtime.finish_shutdown().clean) {
            return 28;
        }
        if (host_executor.shutdown(true) != executor::ShutdownResult::Completed) {
            return 29;
        }
    }
    return 0;
}
