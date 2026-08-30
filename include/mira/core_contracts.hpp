#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mira {

class Id128 final {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    constexpr Id128() noexcept = default;
    explicit constexpr Id128(Bytes bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] static Id128 generate() {
        std::random_device device;
        Bytes bytes{};
        for (std::size_t index = 0; index < bytes.size(); index += sizeof(std::uint32_t)) {
            const auto value = device();
            for (std::size_t offset = 0; offset < sizeof(std::uint32_t) && index + offset < bytes.size();
                 ++offset) {
                bytes[index + offset] = static_cast<std::uint8_t>(value >> (offset * 8));
            }
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
        return Id128(bytes);
    }

    [[nodiscard]] static std::optional<Id128> parse(std::string_view text) noexcept {
        if (text.size() != 32) {
            return std::nullopt;
        }
        Bytes bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto high = hex_value(text[index * 2]);
            const auto low = hex_value(text[index * 2 + 1]);
            if (!high || !low) {
                return std::nullopt;
            }
            bytes[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
        }
        return Id128(bytes);
    }

    [[nodiscard]] constexpr bool is_nil() const noexcept { return bytes_ == Bytes{}; }
    [[nodiscard]] constexpr const Bytes &bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::string to_string() const {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (const auto byte : bytes_) {
            stream << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return stream.str();
    }

    friend constexpr bool operator==(const Id128 &, const Id128 &) noexcept = default;
    // libc++ in the Android NDK does not provide a three-way comparison for
    // std::array<uint8_t, 16>; compare bytes explicitly for all toolchains.
    friend constexpr std::strong_ordering operator<=>(const Id128 &lhs,
                                                       const Id128 &rhs) noexcept {
        for (std::size_t index = 0; index < lhs.bytes_.size(); ++index) {
            if (lhs.bytes_[index] < rhs.bytes_[index]) {
                return std::strong_ordering::less;
            }
            if (lhs.bytes_[index] > rhs.bytes_[index]) {
                return std::strong_ordering::greater;
            }
        }
        return std::strong_ordering::equal;
    }

private:
    [[nodiscard]] static std::optional<std::uint8_t> hex_value(char value) noexcept {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<std::uint8_t>(value - 'A' + 10);
        }
        return std::nullopt;
    }

    Bytes bytes_{};
};

#define MIRA_DEFINE_ID(name)                                                                    \
    struct name final {                                                                         \
        Id128 value{};                                                                          \
        static name generate() { return name{Id128::generate()}; }                              \
        static std::optional<name> parse(std::string_view text) noexcept {                     \
            const auto parsed = Id128::parse(text);                                            \
            return parsed ? std::optional<name>(name{*parsed}) : std::nullopt;                  \
        }                                                                                        \
        [[nodiscard]] bool is_nil() const noexcept { return value.is_nil(); }                   \
        [[nodiscard]] std::string to_string() const { return value.to_string(); }               \
        friend constexpr bool operator==(const name &, const name &) noexcept = default;        \
        friend constexpr auto operator<=>(const name &, const name &) noexcept = default;       \
    }

MIRA_DEFINE_ID(RuntimeId);
MIRA_DEFINE_ID(SessionId);
MIRA_DEFINE_ID(TaskId);
MIRA_DEFINE_ID(CommandId);
MIRA_DEFINE_ID(StepId);
MIRA_DEFINE_ID(OperationId);
MIRA_DEFINE_ID(ObservationId);
MIRA_DEFINE_ID(ActionId);
MIRA_DEFINE_ID(EventId);
MIRA_DEFINE_ID(ArtifactId);
MIRA_DEFINE_ID(ConfirmationId);
MIRA_DEFINE_ID(ModelPackageId);
MIRA_DEFINE_ID(LeaseId);
MIRA_DEFINE_ID(FrameId);
MIRA_DEFINE_ID(EvidenceId);
MIRA_DEFINE_ID(ControllerId);
MIRA_DEFINE_ID(PointerSessionId);
MIRA_DEFINE_ID(TenantId);
MIRA_DEFINE_ID(UserId);
MIRA_DEFINE_ID(HostInstanceId);
MIRA_DEFINE_ID(ControlIngressId);

#undef MIRA_DEFINE_ID

struct Timestamp final {
    std::chrono::system_clock::time_point wall{};
    std::chrono::steady_clock::time_point monotonic{};
    [[nodiscard]] static Timestamp now() noexcept {
        return {std::chrono::system_clock::now(), std::chrono::steady_clock::now()};
    }
};

struct SchemaVersion final {
    std::uint16_t major = 1;
    std::uint16_t minor = 0;
    friend constexpr bool operator==(const SchemaVersion &, const SchemaVersion &) noexcept = default;
};

enum class ErrorCode : std::uint16_t {
    Cancelled,
    DeadlineExceeded,
    ResourceExhausted,
    Unavailable,
    PermissionDenied,
    InvalidArgument,
    InvalidState,
    NotFound,
    AlreadyExists,
    UnsupportedCapability,
    UnsupportedVersion,
    InvalidObservation,
    StaleObservation,
    InvalidModelOutput,
    ContextOverflow,
    SafetyRejected,
    ConfirmationRequired,
    ExecutionUncertain,
    DataLoss,
    PlatformError,
    Internal,
};

struct Error final {
    ErrorCode code = ErrorCode::Internal;
    std::string domain = "mira";
    std::int32_t domain_code = 0;
    bool retryable = false;
    std::string safe_message;
    std::optional<ArtifactId> diagnostic_artifact;
    std::optional<OperationId> operation_id;
};

template <typename T>
class Result final {
public:
    Result(const T &value) : value_(value) {}
    Result(T &&value) : value_(std::move(value)) {}
    Result(const Error &error) : error_(error) {}
    Result(Error &&error) : error_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const T &value() const & { return *value_; }
    [[nodiscard]] T &value() & { return *value_; }
    [[nodiscard]] T &&value() && { return std::move(*value_); }
    [[nodiscard]] const Error &error() const & { return *error_; }
    [[nodiscard]] Error &error() & { return *error_; }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

template <>
class Result<void> final {
public:
    Result() noexcept = default;
    Result(const Error &error) : error_(error) {}
    Result(Error &&error) : error_(std::move(error)) {}
    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const Error &error() const & { return *error_; }

private:
    std::optional<Error> error_;
};

[[nodiscard]] inline Result<void> validate_schema_version(
    SchemaVersion incoming, SchemaVersion current = {}) {
    const bool newer = incoming.major > current.major;
    const bool older = current.major > incoming.major && (current.major - incoming.major > 1);
    if (newer || older) {
        Error error;
        error.code = ErrorCode::UnsupportedVersion;
        error.domain = "mira.schema";
        error.safe_message = "schema major is not supported";
        return error;
    }
    return Result<void>{};
}

enum class RuntimeState : std::uint8_t {
    Constructed,
    Initializing,
    Running,
    Stopping,
    Quiesced,
    Stopped,
    Failed,
};

enum class SessionState : std::uint8_t {
    Opening,
    Autonomous,
    TakeoverPending,
    HumanControlled,
    Resuming,
    Closing,
    Closed,
    Failed,
};

enum class TaskState : std::uint8_t {
    Idle,
    Observing,
    Reasoning,
    Planning,
    Acting,
    Verifying,
    Recovering,
    Pausing,
    Paused,
    TakeoverSettling,
    SuspendedForTakeover,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
};

[[nodiscard]] inline bool is_terminal(TaskState state) noexcept {
    return state == TaskState::Completed || state == TaskState::Failed || state == TaskState::Cancelled;
}

[[nodiscard]] inline bool is_terminal(SessionState state) noexcept {
    return state == SessionState::Closed || state == SessionState::Failed;
}

[[nodiscard]] inline bool is_terminal(RuntimeState state) noexcept {
    return state == RuntimeState::Stopped || state == RuntimeState::Failed;
}

[[nodiscard]] inline bool valid_runtime_transition(RuntimeState from, RuntimeState to) noexcept {
    if (from == to && (from == RuntimeState::Constructed || from == RuntimeState::Stopping ||
                       from == RuntimeState::Quiesced || from == RuntimeState::Stopped ||
                       from == RuntimeState::Failed)) {
        return true;
    }
    switch (from) {
    case RuntimeState::Constructed:
        return to == RuntimeState::Initializing || to == RuntimeState::Stopping;
    case RuntimeState::Initializing:
        return to == RuntimeState::Running || to == RuntimeState::Failed || to == RuntimeState::Stopping;
    case RuntimeState::Running:
        return to == RuntimeState::Stopping || to == RuntimeState::Failed;
    case RuntimeState::Stopping:
        return to == RuntimeState::Quiesced || to == RuntimeState::Failed;
    case RuntimeState::Quiesced:
        return to == RuntimeState::Stopped;
    case RuntimeState::Stopped:
    case RuntimeState::Failed:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool valid_session_transition(SessionState from, SessionState to) noexcept {
    if (from == to && (from == SessionState::Autonomous || from == SessionState::HumanControlled ||
                       from == SessionState::Closing || from == SessionState::Closed ||
                       from == SessionState::Failed)) {
        return true;
    }
    switch (from) {
    case SessionState::Opening:
        return to == SessionState::Autonomous || to == SessionState::Failed || to == SessionState::Closing;
    case SessionState::Autonomous:
        return to == SessionState::TakeoverPending || to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::TakeoverPending:
        return to == SessionState::HumanControlled || to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::HumanControlled:
        return to == SessionState::Resuming || to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::Resuming:
        return to == SessionState::Autonomous || to == SessionState::Failed || to == SessionState::Closing;
    case SessionState::Closing:
        return to == SessionState::Closed || to == SessionState::Failed;
    case SessionState::Closed:
    case SessionState::Failed:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool valid_task_transition(TaskState from, TaskState to) noexcept {
    if (from == to && is_terminal(from)) {
        return true;
    }
    switch (from) {
    case TaskState::Idle:
        return to == TaskState::Observing || to == TaskState::Pausing || to == TaskState::Cancelling ||
               to == TaskState::TakeoverSettling;
    case TaskState::Observing:
        return to == TaskState::Reasoning || to == TaskState::Recovering || to == TaskState::Pausing ||
               to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Reasoning:
        return to == TaskState::Planning || to == TaskState::Recovering || to == TaskState::Pausing ||
               to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Planning:
        return to == TaskState::Acting || to == TaskState::Observing || to == TaskState::Recovering ||
               to == TaskState::Pausing || to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Acting:
        return to == TaskState::Verifying || to == TaskState::Recovering || to == TaskState::Pausing ||
               to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Verifying:
        return to == TaskState::Observing || to == TaskState::Completed || to == TaskState::Recovering ||
               to == TaskState::Pausing || to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Recovering:
        return to == TaskState::Observing || to == TaskState::Failed || to == TaskState::Pausing ||
               to == TaskState::Cancelling || to == TaskState::TakeoverSettling;
    case TaskState::Pausing:
        return to == TaskState::Paused || to == TaskState::Cancelling;
    case TaskState::Paused:
        return to == TaskState::Observing || to == TaskState::TakeoverSettling || to == TaskState::Cancelling;
    case TaskState::TakeoverSettling:
        return to == TaskState::SuspendedForTakeover || to == TaskState::Cancelling;
    case TaskState::SuspendedForTakeover:
        return to == TaskState::Observing || to == TaskState::Cancelling;
    case TaskState::Cancelling:
        return to == TaskState::Cancelled;
    case TaskState::Completed:
    case TaskState::Failed:
    case TaskState::Cancelled:
        return false;
    }
    return false;
}

enum class CommandKind : std::uint8_t {
    OpenSession,
    CloseSession,
    SubmitTask,
    PauseTask,
    ResumeTask,
    CancelTask,
    RequestTakeover,
    ReleaseTakeover,
    OperationCompletion,
    ShutdownRuntime,
};

enum class ReceiptStatus : std::uint8_t { Accepted, Rejected };
enum class SettlementStatus : std::uint8_t { Applied, NoOp, Failed, Superseded };

struct CommandReceipt final {
    CommandId id;
    CommandKind kind = CommandKind::SubmitTask;
    ReceiptStatus status = ReceiptStatus::Rejected;
    std::uint64_t control_sequence = 0;
    std::optional<Error> error;
};

struct TaskOutcome final {
    TaskState terminal_state = TaskState::Failed;
    std::optional<Error> error;
};

struct TaskSnapshot final {
    TaskId id;
    SessionId session_id;
    TaskState state = TaskState::Idle;
    std::uint64_t epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::optional<StepId> active_step;
    std::optional<OperationId> active_operation;
    std::uint32_t completed_steps = 0;
    Timestamp updated_at;
    std::uint64_t control_sequence = 0;
    std::optional<TaskOutcome> terminal_outcome;
};

struct CommandOutcome final {
    CommandId id;
    SettlementStatus status = SettlementStatus::Failed;
    std::optional<Error> error;
    std::optional<TaskSnapshot> task;
};

struct OperationKey final {
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    StepId step_id;
    OperationId operation_id;
    friend constexpr bool operator==(const OperationKey &, const OperationKey &) noexcept = default;
};

enum class OperationState : std::uint8_t {
    Created,
    Submitted,
    Running,
    CancelRequested,
    CompletionReceived,
    Settled,
};

enum class CompletionDisposition : std::uint8_t {
    Accepted,
    Stale,
    Duplicate,
    NotFound,
};

struct Id128Hash final {
    std::size_t operator()(const Id128 &id) const noexcept {
        std::uint64_t result = 1469598103934665603ULL;
        for (const auto byte : id.bytes()) {
            result ^= byte;
            result *= 1099511628211ULL;
        }
        // Android arm32 has a 32-bit size_t; truncate only at the hash API
        // boundary instead of narrowing the FNV-1a state during initialization.
        return static_cast<std::size_t>(result);
    }
};

template <typename Id>
struct StrongIdHash final {
    std::size_t operator()(const Id &id) const noexcept { return Id128Hash{}(id.value); }
};

} // namespace mira
