#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/runtime.hpp>
#include <mira/security.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairo {
class Executor;
} // namespace kairo

namespace mira {

class AgentLoop;
class ConversationLoop;

// ---------------------------------------------------------------------------
// M28 / DEC-068: the interaction coordinator — the single principal-level
// input entry point of Mira.
//
// The coordinator sits above MiraRuntime and below the host (DEC-068 §1). It
// owns the principal registry, the deterministic routing decision, the
// idempotency ledger and the result association. It does NOT implement a
// second session/task state machine: every state change goes through the
// existing MiraRuntime command plane. It does NOT create or drive an agent
// loop: the host binds a running loop through IUserInputSink.
// ---------------------------------------------------------------------------

// The error domain of the interaction surface. The domain code values are
// part of the public contract and must not be renumbered.
inline constexpr std::string_view kInteractionErrorDomain = "mira.interaction";

enum class InteractionDomainCode : std::int32_t {
    Unknown = 0,
    // The input envelope is not acceptable (empty input id, nil identity).
    InputInvalid = 1,
    // The input id was already used and its idempotency window has expired.
    InputExpired = 2,
    // The durable ledger could not be read or written.
    LedgerUnavailable = 3,
    // The bounded ledger index is full and nothing could be reclaimed.
    LedgerFull = 4,
    // The principal lacks the required capability, holds an expired grant, or
    // its identity version does not match (fail closed, DEC-068 R9).
    PermissionDenied = 5,
    // No environment provider is bound, so no session can be created.
    NoSessionAvailable = 6,
    // A MiraRuntime control-plane command failed.
    RuntimeCommandFailed = 7,
    // A MiraRuntime control-plane command did not settle in time.
    RuntimeCommandTimeout = 8,
    // The routing decision rejected the input.
    RouteRejected = 9,
    // The bound input sink refused the delivery.
    SinkUnavailable = 10,
    // The Executor refused the submission (backpressure).
    ExecutorRejected = 11,
    // The coordinator was used before initialize() succeeded.
    NotInitialized = 12,
};

// Builds an error in the mira.interaction domain with a stable domain code.
[[nodiscard]] Error interaction_error(ErrorCode code, InteractionDomainCode domain_code,
                                      std::string message, bool retryable = false);

// ---------------------------------------------------------------------------
// Input envelope and stage vocabulary
// ---------------------------------------------------------------------------

// One accepted principal input. The identity is the host-provided, already
// authenticated principal; input_id is the host-provided idempotency key and
// is NOT a content digest: two different inputs with the same content are two
// different inputs (DEC-068 §4).
struct InputEnvelope final {
    PrincipalContext principal;
    std::string input_id;
    std::string content;
    std::vector<std::string> attachment_refs;
    std::optional<std::string> reply_to;
};

// The stage of one accepted input. Receiving an input is not a completed task
// (DEC-068 §3). This is a closed set: a new member needs a new decision
// record and must not be added during implementation.
enum class InputStage : std::uint8_t {
    Accepted,    // Admitted and registered.
    Routed,      // Bound to a concrete session/task.
    Answered,    // A user-facing reply was produced.
    TaskSettled, // The bound task reached a terminal state.
    Rejected,    // Explicitly rejected (expired / duplicate / denied / closed).
};

[[nodiscard]] std::string_view input_stage_name(InputStage stage);

// The closed set of routing intents (DEC-068 §5). The model may suggest an
// intent; the coordinator validates it against the principal state and the
// permission check before it becomes a decision.
enum class RouteIntent : std::uint8_t {
    ContinueCurrent, // Continue the work that is already running.
    SupplementTask,  // Supplement or correct one specific piece of work.
    NewTask,         // Start a new task.
    ResumeTask,      // Resume earlier work through a new task bound to its summary.
    PauseTask,       // Pause one running task.
    CancelTask,      // Cancel one running task.
    Clarify,         // The reference is ambiguous; ask the user in natural language.
    Reject,          // Denied / expired / over budget / closing.
};

[[nodiscard]] std::string_view route_intent_name(RouteIntent intent);

// The auditable outcome of the deterministic routing decision.
struct RoutingDecision final {
    RouteIntent intent = RouteIntent::Reject;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    // Human-readable, auditable reason for the decision.
    std::string rationale;
    // Natural-language clarification for RouteIntent::Clarify. A clarification
    // is a normal reply, not an error code (DEC-068 R4).
    std::optional<std::string> clarification;
};

// The disposition of one submitted input.
struct InputDisposition final {
    // Mira-internal ingress identity (the pre-reserved ControlIngressId slot).
    ControlIngressId ingress_id;
    // The host-provided idempotency key this disposition belongs to.
    std::string input_id;
    InputStage stage = InputStage::Accepted;
    // True when the input id was already recorded inside the idempotency
    // window; the disposition then mirrors the existing record and no second
    // dispatch happened.
    bool duplicate = false;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    std::optional<Error> error;
    std::optional<std::string> clarification;
};

// ---------------------------------------------------------------------------
// Interaction event family (mira.interaction.*.v1)
// ---------------------------------------------------------------------------

// The closed set of interaction event types. Every member carries its own
// version marker in the schema name; EventEnvelope::schema_version is a dead
// field and must never be relied on (DEC-068 §7).
enum class InteractionEventType : std::uint8_t {
    InputAccepted,
    InputRouted,
    InputClarified,
    InputRejected,
    InputAnswered,
    InputTaskSettled,
};

// The wire schema name of one interaction event ("mira.interaction.<kebab>.v1").
[[nodiscard]] std::string_view interaction_event_schema(InteractionEventType type);
// The event type of a schema name; nullopt for a foreign schema name.
[[nodiscard]] std::optional<InteractionEventType>
parse_interaction_event_schema(std::string_view schema);
// True when the event type name belongs to the interaction family.
[[nodiscard]] bool is_interaction_event_type(std::string_view type);

// ---------------------------------------------------------------------------
// Idempotency ledger
// ---------------------------------------------------------------------------

// One durable record of an accepted input.
struct LedgerEntry final {
    std::string input_id;
    ControlIngressId ingress_id;
    Timestamp admitted_at;
    InputStage stage = InputStage::Accepted;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    // End of the idempotency window. A repeat of the same input id inside the
    // window is a duplicate; outside it the repeat is an explicit expiry
    // rejection and is never executed as a new input.
    //
    // The instant is pinned to nanosecond wall precision (WallTimePoint) for
    // the same reason the rest of the runtime pins it: the default
    // system_clock duration cannot represent nanoseconds on every supported
    // toolchain (MSVC ticks at 100 ns, libc++ differs again).
    WallTimePoint window_expires_at;
    // True when admit() matched an existing record instead of creating one.
    bool duplicate = false;
    // Auditable note of the last transition (routing rationale, rejection
    // reason, delivery state).
    std::string note;
    // The natural-language clarification requested by a Clarify decision.
    std::optional<std::string> clarification;
    // The error of a rejected input, kept so a duplicate replay reports the
    // same reason instead of a generic one.
    std::optional<Error> error;
};

// One lifecycle transition of an accepted input.
struct LedgerTransition final {
    InputStage stage = InputStage::Accepted;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    // The routing intent of a Routed transition; recorded in the event so the
    // decision is auditable after a restart.
    std::optional<RouteIntent> intent;
    std::string note;
    std::optional<std::string> clarification;
    std::optional<Error> error;
};

// The durable idempotency ledger of the principal-level input entry point.
// It is a storage-owner-level narrow interface: it is not part of IMemory and
// not part of IMemoryRetentionSweep (DEC-068 §4).
class IInputLedger {
  public:
    virtual ~IInputLedger() = default;

    // Atomically admits one input. A repeat of the same input id inside the
    // idempotency window returns the existing record with duplicate=true and
    // produces no second dispatch; a repeat outside the window is an explicit
    // expiry rejection.
    [[nodiscard]] virtual Result<LedgerEntry> admit(const InputEnvelope &envelope) = 0;
    // Reads the current record of one input id.
    [[nodiscard]] virtual Result<LedgerEntry> lookup(std::string_view input_id) const = 0;
    // Records one lifecycle transition of an accepted input. The transition is
    // durable and replayable; the stage never regresses.
    [[nodiscard]] virtual Result<LedgerEntry> advance(std::string_view input_id,
                                                      const LedgerTransition &transition) = 0;
};

struct InputLedgerOptions final {
    // The idempotency window. A repeat inside it is a duplicate; outside it
    // the repeat is an explicit expiry rejection.
    std::chrono::seconds idempotency_window{std::chrono::hours(24)};
    // Bounded index (RULE-08). The ledger never grows without a bound; the
    // byte budget of the layer itself is settled by DEC-069 / M31.
    std::size_t max_entries = 4096;
    // The durable partition the ledger stream is written to. When nil, the
    // Mira-owned constant partition is used.
    std::optional<SessionId> partition{};
};

// The Mira-owned durable partition of the interaction ledger stream. It is a
// constant rather than a per-process id so a restarted process finds the same
// durable stream without persisting an id.
[[nodiscard]] SessionId interaction_ledger_partition();

// The durable ledger implementation over the shared event store. The ledger
// stream is a principal-level partition: it is not mixed into a session or
// task stream.
class EventStoreInputLedger final : public IInputLedger {
  public:
    EventStoreInputLedger(std::shared_ptr<IEventStore> events, RuntimeId runtime,
                          InputLedgerOptions options = InputLedgerOptions{});
    ~EventStoreInputLedger() override;
    EventStoreInputLedger(const EventStoreInputLedger &) = delete;
    EventStoreInputLedger &operator=(const EventStoreInputLedger &) = delete;

    [[nodiscard]] Result<LedgerEntry> admit(const InputEnvelope &envelope) override;
    [[nodiscard]] Result<LedgerEntry> lookup(std::string_view input_id) const override;
    [[nodiscard]] Result<LedgerEntry> advance(std::string_view input_id,
                                              const LedgerTransition &transition) override;

    // Rebuilds the index from the durable ledger stream. Called by the
    // coordinator during initialize(); a host that opens a fresh store may
    // call it again after a crash recovery.
    [[nodiscard]] Result<void> rebuild();
    // Reclaims the records whose idempotency window has expired. Records
    // inside their window are never removed. The sweep is bounded by the
    // index size.
    [[nodiscard]] Result<std::size_t> reclaim_expired();

    [[nodiscard]] SessionId partition() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Principal registry
// ---------------------------------------------------------------------------

// The registry key of one principal: (TenantId, UserId). The host instance
// and the grants are attributes of the principal, not identity dimensions
// (DEC-068 §2).
struct PrincipalKey final {
    TenantId tenant_id;
    UserId user_id;
    friend bool operator==(const PrincipalKey &, const PrincipalKey &) noexcept = default;
};

struct PrincipalKeyHash final {
    [[nodiscard]] std::size_t operator()(const PrincipalKey &key) const noexcept;
};

// One task of a principal as the registry sees it.
struct PrincipalTaskEntry final {
    TaskId task_id;
    SessionId session_id;
    TaskState state = TaskState::Idle;
    std::string goal;
    // The input ids routed to this task, in order.
    std::vector<std::string> input_ids;
    // The latest user constraints of this task. A correction updates the
    // target task only; unrelated inputs never touch this list (DEC-068 §6).
    std::vector<std::string> constraints;
    // The prepared context block of this work item: the goal, the latest
    // user constraints and the task state. It is assembled by the bounded
    // context preparation of the coordinator.
    std::string context_block;
    // The task this one resumed, for a ResumeTask decision.
    std::optional<TaskId> resumed_from;
};

// The read-only projection of one principal's registry.
struct PrincipalState final {
    std::vector<SessionId> sessions;
    std::vector<PrincipalTaskEntry> tasks;
};

// One routing suggestion proposed by the host (typically by a model). The
// suggestion is never a decision: the coordinator validates the target
// against the principal state and the permission check before it is adopted
// (DEC-068 R1).
struct RouteSuggestion final {
    RouteIntent intent = RouteIntent::NewTask;
    std::optional<TaskId> task_id;
    std::optional<SessionId> session_id;
    std::string rationale;
    std::optional<std::string> clarification;
};

// The host-side route suggestion seam. Returns nullopt when the host has no
// suggestion; the coordinator then falls back to its structural rules.
using RouteResolver =
    std::function<std::optional<RouteSuggestion>(const InputEnvelope &, const PrincipalState &)>;

// The host-side environment seam. The coordinator creates sessions, so it
// needs an environment for them; the host owns the platform adapter and
// supplies it. The coordinator never creates an environment itself.
using EnvironmentProvider = std::function<Result<std::shared_ptr<IEnvironment>>()>;

// ---------------------------------------------------------------------------
// Input sinks
// ---------------------------------------------------------------------------

// The registration surface of a running loop: the coordinator delivers an
// input to the loop that owns the target task through this seam.
class IUserInputSink {
  public:
    virtual ~IUserInputSink() = default;
    // Delivers one input to the loop of the given task. A failure must be
    // reported; a delivery is never silently dropped (DEC-068 §4.4).
    [[nodiscard]] virtual Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) = 0;
};

// The AgentLoop adapter. Reuses the bounded queue and the ResourceExhausted
// overflow semantics of AgentLoop::enqueue_user_message.
class AgentLoopInputSink final : public IUserInputSink {
  public:
    explicit AgentLoopInputSink(AgentLoop &loop) : loop_(&loop) {}
    [[nodiscard]] Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) override;
    // The input id of the last delivery, for the event-surface association.
    [[nodiscard]] const std::string &last_input_id() const noexcept { return last_input_id_; }

  private:
    AgentLoop *loop_;
    std::string last_input_id_;
};

// The ConversationLoop adapter (M28). ConversationLoop gained the symmetric
// bounded enqueue channel this adapter uses.
class ConversationLoopInputSink final : public IUserInputSink {
  public:
    explicit ConversationLoopInputSink(ConversationLoop &loop) : loop_(&loop) {}
    [[nodiscard]] Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) override;
    [[nodiscard]] const std::string &last_input_id() const noexcept { return last_input_id_; }

  private:
    ConversationLoop *loop_;
    std::string last_input_id_;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

struct CoordinatorConfig final {
    // The capability a principal must hold for the coordinator to admit an
    // input and to create sessions and tasks for it. A missing capability,
    // an expired grant or a mismatched identity version fails closed.
    std::string required_capability = "mira.interaction.input";
    // When set, principals whose identity_version differs are rejected.
    std::optional<GrantVersion> required_identity_version{};
    // The idempotency window handed to the ledger.
    std::chrono::seconds idempotency_window{std::chrono::hours(24)};
    // The bounded ledger index.
    std::size_t max_ledger_entries = 4096;
    // The bounded wait for the MiraRuntime control-plane commands.
    std::chrono::milliseconds command_timeout{5'000};
    // The bounded number of tracked tasks per principal.
    std::size_t max_tasks_per_principal = 64;
    // The bounded number of tracked sessions per principal.
    std::size_t max_sessions_per_principal = 32;
    // The bounded number of registered sinks (running loops).
    std::size_t max_sinks = 256;
    // The bounded number of inputs waiting for a sink of their task.
    std::size_t max_pending_deliveries = 256;
};

class InteractionCoordinator final {
  public:
    // The executor is injected explicitly: MiraRuntime does not expose its
    // executor reference, and the coordinator must not own a second Executor
    // lifetime (DEC-068 §1).
    InteractionCoordinator(CoordinatorConfig config, MiraRuntime &runtime,
                           kairo::Executor &executor);
    ~InteractionCoordinator();
    InteractionCoordinator(const InteractionCoordinator &) = delete;
    InteractionCoordinator &operator=(const InteractionCoordinator &) = delete;

    // Binds the durable ledger and rebuilds the registry projection. Must
    // succeed before the first submit_input().
    [[nodiscard]] Result<void> initialize();
    // Binds the event store that carries the ledger stream.
    void set_event_store(std::shared_ptr<IEventStore> events, RuntimeId runtime);
    // Binds a ledger implementation. Without one, initialize() builds the
    // default EventStoreInputLedger over the bound event store.
    void set_input_ledger(std::shared_ptr<IInputLedger> ledger);
    // Binds the environment provider used to create sessions.
    void set_environment_provider(EnvironmentProvider provider);
    // Binds the route suggestion seam.
    void set_route_resolver(RouteResolver resolver);

    // The single input entry point. The caller does not need to know the
    // session or the task beforehand.
    [[nodiscard]] Result<InputDisposition> submit_input(const InputEnvelope &envelope);

    // The deterministic emergency stop: it never depends on a model routing
    // round (DEC-068 R6).
    [[nodiscard]] Result<CommandHandle> cancel_work(TaskId task_id);

    // Registers the input sink of one running task. When an input is already
    // waiting for that task, it is delivered immediately, so a host that
    // starts the loop after the routing decision does not lose the input.
    [[nodiscard]] Result<void> register_sink(TaskId task_id, std::shared_ptr<IUserInputSink> sink);
    [[nodiscard]] Result<void> unregister_sink(TaskId task_id);

    // Records the user-facing reply of an accepted input.
    [[nodiscard]] Result<InputDisposition> record_answer(std::string_view input_id,
                                                         std::string summary);
    // Records the terminal state of the task behind an accepted input.
    [[nodiscard]] Result<InputDisposition> record_settlement(std::string_view input_id,
                                                             TaskState terminal);
    // Records an explicit rejection of an accepted input.
    [[nodiscard]] Result<InputDisposition> record_rejection(std::string_view input_id,
                                                            std::string reason);

    // Read-only projections.
    [[nodiscard]] Result<PrincipalState> principal_state(const PrincipalContext &principal) const;
    [[nodiscard]] Result<LedgerEntry> input_record(std::string_view input_id) const;

    // Stops accepting inputs and releases the coordinator's own state. The
    // host keeps the ownership of the Runtime and the Executor.
    void shutdown();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
