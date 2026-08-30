#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mira {

enum class BaselineCommandKind : std::uint8_t {
    Command,
    Completion,
    CompleteTask,
    DiagnosticFailure,
};

struct BaselineCommand final {
    std::uint64_t command_id = 0;
    std::uint64_t task_id = 0;
    std::uint64_t epoch = 0;
    BaselineCommandKind kind = BaselineCommandKind::Command;
};

enum class BaselineResultCode : std::uint8_t {
    Applied,
    StaleCompletionIgnored,
    Cancelled,
    Rejected,
    Failed,
    ContextStopped,
    NotFound,
    TimedOut,
};

struct BaselineResult final {
    BaselineResultCode code = BaselineResultCode::Failed;
    std::uint64_t command_id = 0;
    std::uint64_t control_sequence = 0;
    bool task_terminal = false;
    std::string safe_message;
};

struct BaselineSubmission final {
    std::uint64_t command_id = 0;
    bool admitted = false;
    std::optional<BaselineResult> rejection;
};

enum class BaselineRuntimeState : std::uint8_t {
    Constructed,
    Running,
    Stopping,
    Quiesced,
    Stopped,
    Failed,
};

struct BaselineRuntimeConfig final {
    std::size_t worker_threads = 2;
    std::size_t executor_queue_capacity = 64;
    std::size_t max_in_flight = 16'384;
};

struct BaselineRuntimeStatus final {
    BaselineRuntimeState state = BaselineRuntimeState::Constructed;
    std::size_t in_flight = 0;
    std::size_t admission_rejections = 0;
    std::size_t unobserved_results = 0;
    std::uint64_t last_control_sequence = 0;
};

// M0 engineering surface. It proves ownership and failure semantics without
// pre-empting the stable Task/Session API delivered by M1.
class RuntimeBaseline final {
public:
    explicit RuntimeBaseline(BaselineRuntimeConfig config = {});
    ~RuntimeBaseline();

    RuntimeBaseline(const RuntimeBaseline &) = delete;
    RuntimeBaseline &operator=(const RuntimeBaseline &) = delete;
    RuntimeBaseline(RuntimeBaseline &&) = delete;
    RuntimeBaseline &operator=(RuntimeBaseline &&) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] BaselineSubmission submit(BaselineCommand command);
    [[nodiscard]] BaselineResult wait(
        std::uint64_t command_id, std::chrono::milliseconds timeout);
    [[nodiscard]] BaselineResult cancel(std::uint64_t command_id);
    [[nodiscard]] bool request_shutdown();
    void finish_shutdown();

    [[nodiscard]] BaselineRuntimeStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
