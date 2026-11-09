#pragma once

#include <mira/conversation_loop.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kairo {
class Executor;
}

namespace mira {

// Stable child identity plus this exact execution. P0 creates one execution
// per child; future continuation must mint another execution, never reuse it.
struct SubagentHandle final {
    SessionId session_id;
    TaskId task_id;
    OperationId execution_id;
    friend bool operator==(const SubagentHandle &, const SubagentHandle &) = default;
};

enum class SubagentStatus : std::uint8_t {
    Starting,
    Running,
    Answered,
    Failed,
    Cancelled,
    TimedOut
};
[[nodiscard]] std::string subagent_status_name(SubagentStatus status);

struct SubagentSnapshot final {
    SubagentHandle handle;
    std::string profile;
    SubagentStatus status = SubagentStatus::Starting;
    // Bounded result, only populated for Answered. Not a user-facing utterance.
    std::string answer;
    std::string safe_summary;
    std::optional<JsonValue> structured;
    std::optional<ArtifactDescriptor> result_artifact;
    // Claim into the active parent history; does not assert model consumption.
    bool claimed = false;
};

// Each factory invocation transfers an exclusive provider to one child.
// Shared transports/registries remain the host's thread-safety responsibility.
struct SubagentResources final {
    std::unique_ptr<IModelProvider> provider;
    std::shared_ptr<BuiltinToolRegistry> tools;
    std::shared_ptr<IArtifactSource> artifacts;
    // Optional explicit supply, e.g. a host-rendered M24 snapshot baseline.
    ModelInputSupplier input_supplier;
};
using SubagentResourceFactory = std::function<Result<SubagentResources>(
    const AgentLoopSpec &child, const OperationContext &context)>;

struct SubagentProfile final {
    std::string name;
    std::string description;
    ModelProfile model;
    ConversationLoopConfig conversation;
    // Empty = no tools. Unknown IDs, side effects and delegation tools reject.
    std::vector<ToolId> allowed_tools;
    SubagentResourceFactory create_resources;
    std::optional<JsonSchema> output_schema;
    std::chrono::milliseconds timeout{60'000};
    std::uint64_t max_input_tokens_per_request = 16'384;
};

struct SubagentConfig final {
    std::size_t max_active = 4;
    std::size_t max_children = 16;
    // One result slot is reserved for every active/unclaimed child.
    std::size_t max_pending_results = 16;
    std::size_t max_prompt_bytes = 16 * 1024;
    std::size_t max_result_bytes = 8 * 1024;
    // Cumulative reserved ceilings, never refunded (including failures).
    // Inputs use Mira's estimate, outputs the requested generation ceiling;
    // these are delegation quotas, not a provider billing guarantee.
    std::uint64_t max_model_requests = 128;
    std::uint64_t max_input_tokens = 2 * 1024 * 1024;
    std::uint64_t max_output_tokens = 128 * 1024;
};

// DEC-099/M34: single-root Harness owner. It creates ordinary no-observation
// child Session/Task identities, isolated ConversationLoops and kairo managed
// blocking-I/O workers. It never initializes or shuts down the Executor.
// Configure profiles/tools before start_parent(); consume the returned future;
// close() from an external owner BEFORE Executor shutdown/destruction.
// No device adapter, recursive delegation, automatic delta merge or replay.
class SubagentCoordinator final {
  public:
    SubagentCoordinator(kairo::Executor &executor, AgentLoopSpec parent,
                        std::shared_ptr<IEventStore> events,
                        std::shared_ptr<IArtifactStore> artifacts, RuntimeId runtime,
                        SubagentConfig config = {});
    ~SubagentCoordinator();
    SubagentCoordinator(const SubagentCoordinator &) = delete;
    SubagentCoordinator &operator=(const SubagentCoordinator &) = delete;

    [[nodiscard]] Result<void> register_profile(SubagentProfile profile);
    // After registering all profiles, register these on the parent's registry.
    // The profile enum/description snapshot seals further profile changes.
    // Uses weak ownership; calls after coordinator destruction fail closed.
    [[nodiscard]] std::vector<BuiltinToolRegistration> tools() const;
    // Parent loop retains references to its gateway; host keeps dependencies
    // alive through future settlement AND close(). One start per coordinator.
    [[nodiscard]] Result<std::shared_future<Result<ConversationLoopResult>>>
    start_parent(std::unique_ptr<ConversationLoop> loop, OperationContext context);

    [[nodiscard]] Result<SubagentHandle> spawn(const std::string &profile, std::string prompt,
                                               const OperationContext &caller);
    [[nodiscard]] Result<SubagentSnapshot> get(const SubagentHandle &handle) const;
    [[nodiscard]] Result<void> cancel(const SubagentHandle &handle);
    void request_cancel();
    // Stops this root task; releasing takeover requires a new observed task,
    // never restarts this coordinator's already cancelled parent.
    void request_human_takeover();
    // Timeout is diagnostic, not forced termination; retain dependencies and
    // retry close after the factory/provider cooperates. Never call from a
    // parent/child callback or worker. Destructor requests cooperative close.
    [[nodiscard]] Result<void> close(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace mira
