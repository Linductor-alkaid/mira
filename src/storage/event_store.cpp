#include <mira/event_store.hpp>

#include "event_log_wire.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace mira {
namespace {

Error error(ErrorCode code, std::string message, bool retryable = false) {
    Error result;
    result.code = code;
    result.safe_message = std::move(message);
    result.retryable = retryable;
    result.domain = "mira.event_store";
    return result;
}

bool valid_durability(Durability value) {
    return value == Durability::Buffered || value == Durability::ProcessCrash ||
           value == Durability::PowerLoss;
}

Result<void> file_durability(Durability value) {
    if (!valid_durability(value))
        return error(ErrorCode::InvalidArgument, "event durability is invalid");
    if (value == Durability::PowerLoss)
        return error(ErrorCode::UnsupportedCapability,
                     "file store does not support power-loss durability");
    return {};
}

class Sha256 final {
  public:
    Sha256() = default;

    void update(std::span<const std::byte> input) noexcept {
        for (const auto byte : input) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(byte);
            if (buffer_size_ == 64) {
                transform(buffer_.data());
                bit_count_ += 512;
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] Sha256Digest finish() noexcept {
        const auto original_bits = bit_count_ + static_cast<std::uint64_t>(buffer_size_) * 8;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            while (buffer_size_ < 64)
                buffer_[buffer_size_++] = 0;
            transform(buffer_.data());
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56)
            buffer_[buffer_size_++] = 0;
        for (int shift = 7; shift >= 0; --shift) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(original_bits >> (shift * 8));
        }
        transform(buffer_.data());

        Sha256Digest digest;
        for (std::size_t index = 0; index < state_.size(); ++index) {
            digest.bytes[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
            digest.bytes[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
            digest.bytes[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
            digest.bytes[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

  private:
    static constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

    static constexpr std::array<std::uint32_t, 8> kInitialState{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                                                0xa54ff53a, 0x510e527f, 0x9b05688c,
                                                                0x1f83d9ab, 0x5be0cd19};

    static constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
        return (value >> amount) | (value << (32U - amount));
    }

    void transform(const std::uint8_t *block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                           (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                           (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                           static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = rotate_right(words[index - 15], 7) ^
                            rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const auto s1 = rotate_right(words[index - 2], 17) ^
                            rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto working = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto s1 = rotate_right(working[4], 6) ^ rotate_right(working[4], 11) ^
                            rotate_right(working[4], 25);
            const auto choose = (working[4] & working[5]) ^ ((~working[4]) & working[6]);
            const auto temp1 = working[7] + s1 + choose + kRoundConstants[index] + words[index];
            const auto s0 = rotate_right(working[0], 2) ^ rotate_right(working[0], 13) ^
                            rotate_right(working[0], 22);
            const auto majority =
                (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const auto temp2 = s0 + majority;
            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temp1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temp1 + temp2;
        }
        for (std::size_t index = 0; index < state_.size(); ++index)
            state_[index] += working[index];
    }

    std::array<std::uint32_t, 8> state_ = kInitialState;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

std::string canonical(const EventEnvelope &event) {
    std::ostringstream stream;
    stream << event.event_id.to_string() << '|' << event.runtime_id.to_string() << '|'
           << event.session_id.to_string() << '|';
    if (event.task_id)
        stream << event.task_id->to_string();
    stream << '|' << event.session_sequence << '|';
    if (event.task_sequence)
        stream << *event.task_sequence;
    stream << '|' << event.cause_control_sequence << '|' << event.payload.type << '|'
           << event.payload.data << '|' << static_cast<unsigned int>(event.payload.classification);
    return stream.str();
}

std::string canonical(const AppendRequest &request) {
    EventEnvelope event;
    event.event_id = request.event_id;
    event.runtime_id = request.runtime_id;
    event.session_id = request.session_id;
    event.task_id = request.task_id;
    event.cause_control_sequence = request.cause;
    event.payload = request.payload;
    return canonical(event);
}

bool same_request(const EventEnvelope &event, const AppendRequest &request) {
    return event.event_id == request.event_id && event.runtime_id == request.runtime_id &&
           event.session_id == request.session_id && event.task_id == request.task_id &&
           event.cause_control_sequence == request.cause &&
           event.payload.type == request.payload.type &&
           event.payload.data == request.payload.data &&
           event.payload.classification == request.payload.classification;
}

} // namespace

Result<EventClass> parse_event_class(std::uint8_t value) {
    switch (value) {
    case 0:
        return EventClass::Critical;
    case 1:
        return EventClass::State;
    case 2:
        return EventClass::Diagnostic;
    default: {
        Error result;
        result.code = ErrorCode::UnsupportedVersion;
        result.domain = "mira.event_store";
        result.safe_message = "unknown event classification";
        return result;
    }
    }
}

std::string Sha256Digest::to_string() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}

Sha256Digest digest_bytes(std::span<const std::byte> bytes) noexcept {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

std::optional<Sha256Digest> digest_from_hex(std::string_view text) noexcept {
    if (text.size() != 64) {
        return std::nullopt;
    }
    Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f') {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F') {
                return value - 'A' + 10;
            }
            return -1;
        };
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        digest.bytes[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return digest;
}

class MemoryEventStore::Impl final {
  public:
    explicit Impl(std::size_t max) : max_events(max) {}
    mutable std::mutex mutex;
    std::size_t max_events;
    std::vector<EventEnvelope> events;
    std::unordered_map<EventId, std::size_t, StrongIdHash<EventId>> by_id;
    std::unordered_map<SessionId, SessionSequence, StrongIdHash<SessionId>> next_session;
    std::unordered_map<TaskId, TaskSequence, StrongIdHash<TaskId>> next_task;
    DurableSequence durable_watermark = 0;
};

MemoryEventStore::MemoryEventStore(std::size_t max_events)
    : impl_(std::make_unique<Impl>(max_events)) {}
MemoryEventStore::~MemoryEventStore() = default;

Result<AppendReceipt> MemoryEventStore::append(const AppendRequest &request) {
    return append_stamped(request, Timestamp::now(), EventTimestampOrigin::AppendProcess);
}

Result<AppendReceipt> MemoryEventStore::append_stamped(const AppendRequest &request,
                                                       Timestamp timestamp,
                                                       EventTimestampOrigin origin) {
    std::lock_guard lock(impl_->mutex);
    if (!valid_durability(request.required))
        return error(ErrorCode::InvalidArgument, "event durability is invalid");
    if (request.event_id.is_nil() || request.runtime_id.is_nil() || request.session_id.is_nil()) {
        return error(ErrorCode::InvalidArgument, "event, runtime, and session IDs must be non-nil");
    }
    const auto existing = impl_->by_id.find(request.event_id);
    if (existing != impl_->by_id.end()) {
        const auto &event = impl_->events[existing->second];
        if (same_request(event, request) && event.integrity == digest_string(canonical(request))) {
            return AppendReceipt{event.event_id, event.session_sequence, event.task_sequence,
                                 impl_->durable_watermark, Durability::Buffered};
        }
        return error(ErrorCode::DataLoss, "duplicate event ID has a different payload");
    }
    if (impl_->events.size() >= impl_->max_events) {
        return error(ErrorCode::ResourceExhausted, "event store capacity exhausted", true);
    }
    EventEnvelope event;
    event.event_id = request.event_id;
    event.runtime_id = request.runtime_id;
    event.session_id = request.session_id;
    event.task_id = request.task_id;
    event.session_sequence = ++impl_->next_session[request.session_id];
    if (request.task_id)
        event.task_sequence = ++impl_->next_task[*request.task_id];
    event.cause_control_sequence = request.cause;
    event.timestamp = timestamp;
    event.timestamp_origin = origin;
    event.payload = request.payload;
    event.integrity = digest_string(canonical(request));
    const auto index = impl_->events.size();
    impl_->events.push_back(event);
    impl_->by_id.emplace(event.event_id, index);
    const auto watermark = ++impl_->durable_watermark;
    return AppendReceipt{event.event_id, event.session_sequence, event.task_sequence, watermark,
                         Durability::Buffered};
}

Result<std::vector<AppendReceipt>>
MemoryEventStore::append_batch(std::span<const AppendRequest> requests) {
    std::vector<AppendReceipt> receipts;
    receipts.reserve(requests.size());
    for (const auto &request : requests) {
        auto receipt = append(request);
        if (!receipt)
            return receipt.error();
        receipts.push_back(std::move(receipt).value());
    }
    return receipts;
}

Result<EventPage> MemoryEventStore::read(const EventQuery &query) const {
    std::lock_guard lock(impl_->mutex);
    EventPage page;
    const auto after = query.after_sequence.value_or(0);
    for (const auto &event : impl_->events) {
        if (event.session_id != query.session_id || event.session_sequence <= after)
            continue;
        if (page.events.size() == query.limit) {
            page.has_more = true;
            break;
        }
        page.events.push_back(event);
    }
    return page;
}

Result<StoreRecoveryReport> MemoryEventStore::recover(const RecoveryOptions &) {
    std::lock_guard lock(impl_->mutex);
    return StoreRecoveryReport{RecoveryStatus::Clean, impl_->events.size(), 0,
                               "memory store is consistent"};
}

Result<void> MemoryEventStore::flush(Durability required) {
    if (!valid_durability(required))
        return error(ErrorCode::InvalidArgument, "event durability is invalid");
    if (required != Durability::Buffered)
        return error(ErrorCode::UnsupportedCapability,
                     "memory store only supports buffered durability");
    return {};
}
std::size_t MemoryEventStore::size() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->events.size();
}
std::optional<EventEnvelope> MemoryEventStore::find(EventId id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->by_id.find(id);
    return found == impl_->by_id.end() ? std::nullopt
                                       : std::optional<EventEnvelope>(impl_->events[found->second]);
}

class FileEventStore::Impl final {
  public:
    Impl(std::filesystem::path directory, std::size_t max)
        : root(std::move(directory)), memory(max) {
        std::error_code error_code;
        std::filesystem::create_directories(root, error_code);
        log_path = root / "events.log";
    }
    std::filesystem::path root;
    std::filesystem::path log_path;
    mutable std::mutex mutex;
    MemoryEventStore memory;
    bool loaded = false;
    bool v2 = false;
    bool needs_separator = false;
    bool read_only = false;
    RecoveryStatus recovery_status = RecoveryStatus::Clean;
    std::string recovery_diagnostic;
    std::unordered_map<SessionId, SessionSequence, StrongIdHash<SessionId>> next_session;
    std::unordered_map<TaskId, TaskSequence, StrongIdHash<TaskId>> next_task;
    std::unordered_map<EventId, Sha256Digest, StrongIdHash<EventId>> integrity_by_id;
};

FileEventStore::FileEventStore(std::filesystem::path root, std::size_t max_events)
    : impl_(std::make_unique<Impl>(std::move(root), max_events)) {}
FileEventStore::~FileEventStore() = default;

Result<AppendReceipt> FileEventStore::append(const AppendRequest &request) {
    auto supported = file_durability(request.required);
    if (!supported)
        return supported.error();
    bool loaded;
    {
        std::lock_guard lock(impl_->mutex);
        loaded = impl_->loaded;
        if (impl_->loaded && impl_->read_only) {
            return error(ErrorCode::DataLoss, "event store is read-only after recovery");
        }
    }
    if (!loaded) {
        auto recovery = recover({});
        if (!recovery)
            return recovery.error();
        if (recovery.value().status == RecoveryStatus::ReadOnlyDiagnostic ||
            recovery.value().status == RecoveryStatus::DataLoss) {
            return error(ErrorCode::DataLoss, "event store is read-only after recovery");
        }
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->read_only)
        return error(ErrorCode::DataLoss, "event store is read-only after recovery");
    if (request.event_id.is_nil() || request.runtime_id.is_nil() || request.session_id.is_nil()) {
        return error(ErrorCode::InvalidArgument, "event, runtime, and session IDs must be non-nil");
    }
    if ((request.task_id && request.task_id->is_nil()) ||
        !parse_event_class(static_cast<std::uint8_t>(request.payload.classification))) {
        return error(ErrorCode::InvalidArgument, "event task or classification is invalid");
    }
    const auto existing = impl_->integrity_by_id.find(request.event_id);
    const auto requested_integrity = digest_string(canonical(request));
    if (existing != impl_->integrity_by_id.end()) {
        if (existing->second != requested_integrity) {
            return error(ErrorCode::DataLoss, "duplicate event ID has a different payload");
        }
        const auto event = impl_->memory.find(request.event_id);
        if (!event)
            return error(ErrorCode::DataLoss, "event index is inconsistent");
        if (!same_request(*event, request))
            return error(ErrorCode::DataLoss, "duplicate event ID has different request fields");
        return AppendReceipt{event->event_id, event->session_sequence, event->task_sequence, 0,
                             Durability::ProcessCrash};
    }
    const auto session_sequence = impl_->next_session[request.session_id] + 1;
    std::optional<TaskSequence> task_sequence;
    if (request.task_id)
        task_sequence = impl_->next_task[*request.task_id] + 1;
    std::ofstream output(impl_->log_path, std::ios::binary | std::ios::app);
    if (!output)
        return error(ErrorCode::Unavailable, "event log cannot be opened", true);
    const auto timestamp = Timestamp::now();
    event_log_wire::Record record{
        request, session_sequence, task_sequence, requested_integrity, timestamp, true};
    const auto encoded = event_log_wire::encode(record);
    if (impl_->needs_separator)
        output << '\n';
    if (!impl_->v2)
        output << event_log_wire::header << '\n' << event_log_wire::records << '\n';
    output << encoded << '\n';
    output.flush();
    output.close();
    if (!output) {
        impl_->read_only = true;
        impl_->recovery_status = RecoveryStatus::ReadOnlyDiagnostic;
        impl_->recovery_diagnostic = "event write failed; reopen for recovery";
        return error(ErrorCode::Unavailable, "event log write failed", true);
    }
    impl_->v2 = true;
    impl_->needs_separator = false;
    auto result =
        impl_->memory.append_stamped(request, timestamp, EventTimestampOrigin::AppendProcess);
    if (!result || result.value().session_sequence != session_sequence ||
        result.value().task_sequence != task_sequence) {
        impl_->read_only = true;
        impl_->recovery_status = RecoveryStatus::DataLoss;
        impl_->recovery_diagnostic = "event index update failed after durable append";
        return error(ErrorCode::DataLoss, "event index update failed after durable append");
    }
    impl_->next_session[request.session_id] = session_sequence;
    if (request.task_id && task_sequence)
        impl_->next_task[*request.task_id] = task_sequence.value();
    impl_->integrity_by_id.emplace(request.event_id, requested_integrity);
    result.value().achieved = Durability::ProcessCrash;
    return result;
}

Result<std::vector<AppendReceipt>>
FileEventStore::append_batch(std::span<const AppendRequest> requests) {
    std::vector<AppendReceipt> result;
    result.reserve(requests.size());
    for (const auto &request : requests) {
        auto receipt = append(request);
        if (!receipt)
            return receipt.error();
        result.push_back(std::move(receipt).value());
    }
    return result;
}

Result<EventPage> FileEventStore::read(const EventQuery &query) const {
    bool loaded;
    {
        std::lock_guard lock(impl_->mutex);
        loaded = impl_->loaded;
        if (!impl_->loaded) {
            // Recovery performs the one-time file scan outside this lock.
        } else if (impl_->read_only) {
            return error(ErrorCode::DataLoss, "event store is read-only after recovery");
        }
    }
    if (!loaded) {
        auto recovery = const_cast<FileEventStore *>(this)->recover({});
        if (!recovery)
            return recovery.error();
        if (recovery.value().status == RecoveryStatus::DataLoss ||
            recovery.value().status == RecoveryStatus::ReadOnlyDiagnostic) {
            return error(ErrorCode::DataLoss, "event store is read-only after recovery");
        }
    }
    return impl_->memory.read(query);
}

Result<StoreRecoveryReport> FileEventStore::recover(const RecoveryOptions &options) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->loaded) {
        const auto memory_report = impl_->memory.recover(options);
        if (!memory_report)
            return memory_report.error();
        return StoreRecoveryReport{impl_->recovery_status, memory_report.value().events_recovered,
                                   0, impl_->recovery_diagnostic};
    }
    std::ifstream input(impl_->log_path, std::ios::binary);
    if (!input) {
        impl_->loaded = true;
        impl_->recovery_diagnostic = "new event store";
        return StoreRecoveryReport{RecoveryStatus::Clean, 0, 0, "new event store"};
    }
    std::size_t recovered = 0;
    std::uintmax_t offset = 0, last_good_offset = 0;
    const auto stop = [&](RecoveryStatus status, std::string message, std::size_t bytes = 0) {
        impl_->loaded = true;
        impl_->read_only =
            status == RecoveryStatus::DataLoss || status == RecoveryStatus::ReadOnlyDiagnostic;
        impl_->recovery_status = status;
        impl_->recovery_diagnostic = std::move(message);
        return StoreRecoveryReport{status, recovered, bytes, impl_->recovery_diagnostic};
    };
    const auto malformed = [&](bool has_more) {
        if (has_more || !options.repair_uncommitted_tail)
            return stop(RecoveryStatus::ReadOnlyDiagnostic,
                        "event log contains an incomplete record");
        std::error_code ec;
        const auto size = std::filesystem::file_size(impl_->log_path, ec);
        if (ec || size < last_good_offset)
            return stop(RecoveryStatus::ReadOnlyDiagnostic,
                        "event log size could not be determined");
        const auto discarded = static_cast<std::size_t>(size - last_good_offset);
        std::filesystem::resize_file(impl_->log_path, last_good_offset, ec);
        if (ec)
            return stop(RecoveryStatus::ReadOnlyDiagnostic,
                        "event log tail could not be truncated");
        impl_->needs_separator = false;
        return stop(RecoveryStatus::TailTruncated, "incomplete event log tail truncated",
                    discarded);
    };
    std::string line;
    while (std::getline(input, line)) {
        const bool terminated = !input.eof();
        offset += line.size() + (terminated ? 1U : 0U);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) {
            last_good_offset = offset;
            impl_->needs_separator = !terminated;
            continue;
        }
        if (line.starts_with("MIRA_EVENT_")) {
            if (!impl_->v2 && line != event_log_wire::header &&
                event_log_wire::header.starts_with(line))
                return malformed(input.peek() != std::char_traits<char>::eof());
            if (line != event_log_wire::header)
                return stop(RecoveryStatus::ReadOnlyDiagnostic, "event log version is unsupported");
            if (impl_->v2)
                return stop(RecoveryStatus::DataLoss, "event log repeats a format transition");
            std::string marker;
            if (!std::getline(input, marker))
                return malformed(false);
            const bool marker_terminated = !input.eof();
            offset += marker.size() + (marker_terminated ? 1U : 0U);
            if (!marker.empty() && marker.back() == '\r')
                marker.pop_back();
            if (marker != event_log_wire::records) {
                if (event_log_wire::records.starts_with(marker))
                    return malformed(marker_terminated ||
                                     input.peek() != std::char_traits<char>::eof());
                return stop(RecoveryStatus::ReadOnlyDiagnostic,
                            "event log format marker is invalid");
            }
            impl_->v2 = true;
            impl_->needs_separator = !marker_terminated;
            last_good_offset = offset;
            continue;
        }
        if (!impl_->v2 && line.starts_with("E"))
            return stop(RecoveryStatus::ReadOnlyDiagnostic,
                        "event log record has no version marker");
        auto decoded =
            impl_->v2 ? event_log_wire::decode_v2(line) : event_log_wire::decode_legacy(line);
        if (!decoded) {
            if (decoded.error().code == ErrorCode::InvalidArgument)
                return malformed((impl_->v2 && terminated) ||
                                 input.peek() != std::char_traits<char>::eof());
            return stop(decoded.error().code == ErrorCode::UnsupportedVersion
                            ? RecoveryStatus::ReadOnlyDiagnostic
                            : RecoveryStatus::DataLoss,
                        decoded.error().safe_message);
        }
        const auto &r = decoded.value();
        const auto &request = r.request;
        const auto expected_sequence = impl_->next_session[request.session_id] + 1;
        std::optional<TaskSequence> expected_task_sequence;
        if (request.task_id)
            expected_task_sequence = impl_->next_task[*request.task_id] + 1;
        if (r.sequence != expected_sequence || (r.v2 && r.task_sequence != expected_task_sequence))
            return stop(RecoveryStatus::DataLoss, "event log contains a sequence gap");
        if (r.request_digest != digest_string(canonical(request)))
            return stop(RecoveryStatus::DataLoss, "event request integrity digest mismatch");
        auto appended = impl_->memory.append_stamped(
            request, r.timestamp,
            r.v2 ? EventTimestampOrigin::PersistedWall : EventTimestampOrigin::UnavailableLegacy);
        if (!appended || appended.value().session_sequence != r.sequence ||
            appended.value().task_sequence != expected_task_sequence)
            return stop(RecoveryStatus::DataLoss,
                        "event recovery index could not be reconstructed");
        impl_->next_session[request.session_id] = r.sequence;
        if (request.task_id)
            impl_->next_task[*request.task_id] = *expected_task_sequence;
        impl_->integrity_by_id.emplace(request.event_id, r.request_digest);
        last_good_offset = offset;
        impl_->needs_separator = !terminated;
        ++recovered;
    }
    if (input.bad())
        return stop(RecoveryStatus::ReadOnlyDiagnostic, "event log read failed");
    return stop(RecoveryStatus::Clean, "event log recovered");
}

Result<void> FileEventStore::flush(Durability required) {
    auto supported = file_durability(required);
    if (!supported)
        return supported;
    // Recovery is idempotent and acquires the same mutex as append.
    auto recovered = recover({});
    if (!recovered)
        return recovered.error();
    std::lock_guard lock(impl_->mutex);
    if (impl_->read_only)
        return error(ErrorCode::DataLoss, "event store is read-only after recovery");
    std::ofstream output(impl_->log_path, std::ios::binary | std::ios::app);
    if (!output)
        return error(ErrorCode::Unavailable, "event log cannot be opened", true);
    output.flush();
    output.close();
    return output ? Result<void>{}
                  : Result<void>(error(ErrorCode::Unavailable, "event log flush failed", true));
}
const std::filesystem::path &FileEventStore::root() const noexcept { return impl_->root; }
bool FileEventStore::read_only() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->read_only;
}

} // namespace mira
