#pragma once

#include <mira/core_contracts.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

using SessionSequence = std::uint64_t;
using TaskSequence = std::uint64_t;
using ControlSequence = std::uint64_t;
using DurableSequence = std::uint64_t;

struct Sha256Digest final {
    std::array<std::uint8_t, 32> bytes{};
    friend constexpr bool operator==(const Sha256Digest &, const Sha256Digest &) noexcept = default;
    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] Sha256Digest digest_bytes(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] inline Sha256Digest digest_string(std::string_view value) noexcept {
    return digest_bytes(std::as_bytes(std::span(value.data(), value.size())));
}

enum class Durability : std::uint8_t { Buffered, ProcessCrash, PowerLoss };
enum class EventClass : std::uint8_t { Critical, State, Diagnostic };

[[nodiscard]] Result<EventClass> parse_event_class(std::uint8_t value);

struct EventPayload final {
    std::string type;
    std::string data;
    EventClass classification = EventClass::State;
};

struct EventEnvelope final {
    EventId event_id;
    RuntimeId runtime_id;
    SessionId session_id;
    std::optional<TaskId> task_id;
    SessionSequence session_sequence = 0;
    std::optional<TaskSequence> task_sequence;
    ControlSequence cause_control_sequence = 0;
    Timestamp timestamp;
    SchemaVersion schema_version;
    EventPayload payload;
    Sha256Digest integrity;
};

struct AppendRequest final {
    EventId event_id;
    RuntimeId runtime_id;
    SessionId session_id;
    std::optional<TaskId> task_id;
    EventPayload payload;
    ControlSequence cause = 0;
    Durability required = Durability::ProcessCrash;
};

struct AppendReceipt final {
    EventId event_id;
    SessionSequence session_sequence = 0;
    std::optional<TaskSequence> task_sequence;
    DurableSequence durable_watermark = 0;
    Durability achieved = Durability::Buffered;
};

struct EventQuery final {
    SessionId session_id;
    std::optional<SessionSequence> after_sequence;
    std::size_t limit = 1024;
};

struct EventPage final {
    std::vector<EventEnvelope> events;
    bool has_more = false;
};

enum class RecoveryStatus : std::uint8_t { Clean, TailTruncated, ReadOnlyDiagnostic, DataLoss };

struct StoreRecoveryReport final {
    RecoveryStatus status = RecoveryStatus::Clean;
    std::size_t events_recovered = 0;
    std::size_t bytes_discarded = 0;
    std::string diagnostic;
};

struct RecoveryOptions final {
    bool repair_uncommitted_tail = true;
};

class IEventStore {
  public:
    virtual ~IEventStore() = default;
    virtual Result<AppendReceipt> append(const AppendRequest &) = 0;
    virtual Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest>) = 0;
    virtual Result<EventPage> read(const EventQuery &) const = 0;
    virtual Result<StoreRecoveryReport> recover(const RecoveryOptions &) = 0;
    virtual Result<void> flush(Durability) = 0;
};

class MemoryEventStore final : public IEventStore {
  public:
    explicit MemoryEventStore(std::size_t max_events = 100'000);
    ~MemoryEventStore() override;
    MemoryEventStore(const MemoryEventStore &) = delete;
    MemoryEventStore &operator=(const MemoryEventStore &) = delete;

    Result<AppendReceipt> append(const AppendRequest &) override;
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest>) override;
    Result<EventPage> read(const EventQuery &) const override;
    Result<StoreRecoveryReport> recover(const RecoveryOptions &) override;
    Result<void> flush(Durability) override;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<EventEnvelope> find(EventId) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FileEventStore final : public IEventStore {
  public:
    FileEventStore(std::filesystem::path root, std::size_t max_events = 100'000);
    ~FileEventStore() override;
    FileEventStore(const FileEventStore &) = delete;
    FileEventStore &operator=(const FileEventStore &) = delete;

    Result<AppendReceipt> append(const AppendRequest &) override;
    Result<std::vector<AppendReceipt>> append_batch(std::span<const AppendRequest>) override;
    Result<EventPage> read(const EventQuery &) const override;
    Result<StoreRecoveryReport> recover(const RecoveryOptions &) override;
    Result<void> flush(Durability) override;
    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    [[nodiscard]] bool read_only() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
