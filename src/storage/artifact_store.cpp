#include <mira/artifact_store.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mira {
namespace {

Error error(ErrorCode code, std::string message, bool retryable = false) {
    Error result;
    result.code = code;
    result.domain = "mira.artifact_store";
    result.safe_message = std::move(message);
    result.retryable = retryable;
    return result;
}

ArtifactId id_from_digest(const Sha256Digest &digest) noexcept {
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return ArtifactId{Id128{bytes}};
}

std::filesystem::path object_path(const std::filesystem::path &root, const Sha256Digest &digest) {
    const auto text = digest.to_string();
    return root / "objects" / text.substr(0, 2) / text.substr(2, 2) / text;
}

std::filesystem::path manifest_path(const std::filesystem::path &root) {
    return root / "manifest.log";
}

std::string manifest_line(const ArtifactDescriptor &descriptor) {
    std::ostringstream output;
    output << descriptor.id.to_string() << ' ' << descriptor.digest.to_string() << ' '
           << descriptor.byte_size << ' ' << static_cast<unsigned int>(descriptor.encoding) << ' '
           << static_cast<unsigned int>(descriptor.sensitivity) << ' '
           << descriptor.content_schema.major << ' ' << descriptor.content_schema.minor << ' '
           << std::quoted(descriptor.media_type) << '\n';
    return output.str();
}

bool same_descriptor(const ArtifactDescriptor &a, const ArtifactDescriptor &b) {
    return a.id == b.id && a.digest == b.digest && a.byte_size == b.byte_size &&
           a.media_type == b.media_type && a.encoding == b.encoding &&
           a.sensitivity == b.sensitivity && a.content_schema == b.content_schema;
}

bool matches_object(const std::filesystem::path &path, std::span<const std::byte> expected) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) || ec ||
        std::filesystem::file_size(path, ec) != expected.size() || ec)
        return false;
    std::ifstream input(path, std::ios::binary);
    std::array<char, 4096> buffer{};
    if (!input)
        return false;
    while (!expected.empty()) {
        const auto size = std::min(expected.size(), buffer.size());
        if (!input.read(buffer.data(), static_cast<std::streamsize>(size)) ||
            std::memcmp(buffer.data(), expected.data(), size) != 0)
            return false;
        expected = expected.subspan(size);
    }
    return true;
}

Result<FileArtifactCapacity> scan_directory(const std::filesystem::path &root) {
    constexpr std::size_t max_scan_entries = 65'536;
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(root, ec)) || ec)
        return error(ErrorCode::Unavailable, "artifact directory root is unavailable");
    std::filesystem::recursive_directory_iterator entry(root, ec);
    const std::filesystem::recursive_directory_iterator end;
    FileArtifactCapacity result;
    for (; !ec && entry != end; entry.increment(ec)) {
        if (++result.entries_scanned > max_scan_entries)
            return error(ErrorCode::ResourceExhausted, "artifact directory scan limit exceeded");
        const auto status = entry->symlink_status(ec);
        if (ec)
            break;
        if (std::filesystem::is_directory(status))
            continue;
        if (!std::filesystem::is_regular_file(status))
            return error(ErrorCode::DataLoss, "artifact directory contains a non-regular entry");
        const auto bytes = entry->file_size(ec);
        if (ec)
            break;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - result.file_bytes)
            return error(ErrorCode::ResourceExhausted,
                         "artifact directory size is not representable");
        result.file_bytes += bytes;
    }
    if (ec)
        return error(ErrorCode::Unavailable, "artifact directory cannot be measured", true);
    return result;
}

bool parse_digest(std::string_view text, Sha256Digest &digest) {
    if (text.size() != 64)
        return false;
    for (std::size_t i = 0; i < digest.bytes.size(); ++i) {
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const auto high = hex(text[i * 2]);
        const auto low = hex(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        digest.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

} // namespace

Result<void> ArtifactWriter::write(const void *data, std::size_t size) {
    if (!bytes_ || (!data && size != 0))
        return error(ErrorCode::InvalidArgument, "invalid artifact writer");
    if (size > spec_.max_bytes - std::min(spec_.max_bytes, bytes_->size())) {
        return error(ErrorCode::ResourceExhausted, "artifact exceeds configured size limit");
    }
    const auto *begin = static_cast<const std::byte *>(data);
    bytes_->insert(bytes_->end(), begin, begin + size);
    return Result<void>{};
}

Result<void> ArtifactWriter::write(const std::vector<std::byte> &data) {
    return write(data.data(), data.size());
}

std::size_t ArtifactWriter::size() const noexcept { return bytes_ ? bytes_->size() : 0; }

const std::vector<std::byte> &ArtifactReader::bytes() const noexcept {
    static const std::vector<std::byte> empty;
    return bytes_ ? *bytes_ : empty;
}

std::size_t ArtifactReader::size() const noexcept { return bytes_ ? bytes_->size() : 0; }

class MemoryArtifactStore::Impl final {
  public:
    explicit Impl(std::size_t max) : max_total_bytes(max) {}
    mutable std::mutex mutex;
    std::size_t max_total_bytes;
    std::size_t used_bytes = 0;
    struct Record final {
        ArtifactDescriptor descriptor;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        bool erased = false;
    };
    std::unordered_map<ArtifactId, Record, StrongIdHash<ArtifactId>> records;
};

MemoryArtifactStore::MemoryArtifactStore(std::size_t max_total_bytes)
    : impl_(std::make_unique<Impl>(max_total_bytes)) {}
MemoryArtifactStore::~MemoryArtifactStore() = default;

Result<ArtifactWriter> MemoryArtifactStore::begin(const ArtifactWriteSpec &spec) {
    if (spec.max_bytes == 0)
        return error(ErrorCode::InvalidArgument, "artifact max_bytes must be positive");
    return ArtifactWriter(std::make_shared<std::vector<std::byte>>(), spec);
}

Result<ArtifactDescriptor> MemoryArtifactStore::commit(ArtifactWriter &writer) {
    if (!writer.valid())
        return error(ErrorCode::InvalidArgument, "invalid artifact writer");
    const auto digest = digest_bytes(std::span<const std::byte>(*writer.bytes_));
    const auto id = id_from_digest(digest);
    std::lock_guard lock(impl_->mutex);
    const auto existing = impl_->records.find(id);
    if (existing != impl_->records.end() && !existing->second.erased) {
        if (existing->second.descriptor.digest == digest &&
            existing->second.bytes->size() == writer.size()) {
            return existing->second.descriptor;
        }
        return error(ErrorCode::DataLoss, "artifact ID collision with different content");
    }
    if (writer.size() >
        impl_->max_total_bytes - std::min(impl_->max_total_bytes, impl_->used_bytes)) {
        return error(ErrorCode::ResourceExhausted, "artifact store capacity exhausted", true);
    }
    ArtifactDescriptor descriptor;
    descriptor.id = id;
    descriptor.digest = digest;
    descriptor.byte_size = writer.size();
    descriptor.media_type = writer.spec_.media_type;
    descriptor.encoding = writer.spec_.encoding;
    descriptor.sensitivity = writer.spec_.sensitivity;
    descriptor.content_schema = writer.spec_.content_schema;
    auto bytes = std::make_shared<const std::vector<std::byte>>(*writer.bytes_);
    impl_->used_bytes += bytes->size();
    impl_->records[id] = Impl::Record{descriptor, std::move(bytes), false};
    return descriptor;
}

Result<ArtifactReader> MemoryArtifactStore::open(const ArtifactDescriptor &descriptor) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->records.find(descriptor.id);
    if (found == impl_->records.end() || found->second.erased) {
        return error(ErrorCode::NotFound, "artifact is not available");
    }
    if (found->second.descriptor.digest != descriptor.digest ||
        found->second.descriptor.byte_size != descriptor.byte_size) {
        return error(ErrorCode::DataLoss, "artifact descriptor integrity mismatch");
    }
    return ArtifactReader(found->second.bytes);
}

Result<ErasureReceipt> MemoryArtifactStore::erase(const ArtifactErasureRequest &request) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->records.find(request.id);
    if (found == impl_->records.end())
        return error(ErrorCode::NotFound, "artifact is not available");
    if (!found->second.erased) {
        impl_->used_bytes -= found->second.bytes->size();
        found->second.bytes.reset();
        found->second.erased = true;
    }
    return ErasureReceipt{request.id, true, true};
}

std::size_t MemoryArtifactStore::size() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->records.size();
}

class FileArtifactStore::Impl final {
  public:
    Impl(std::filesystem::path directory, std::size_t max,
         FileArtifactDirectoryBudget configured_budget)
        : root(std::move(directory)), memory(max), budget(configured_budget) {
        std::error_code error_code;
        std::filesystem::create_directories(root / "objects", error_code);
        std::filesystem::create_directories(root / "temp", error_code);
        std::filesystem::create_directories(root / "quarantine", error_code);
        std::filesystem::create_directories(root / "tombstones", error_code);
    }
    std::filesystem::path root;
    mutable std::mutex mutex;
    MemoryArtifactStore memory;
    FileArtifactDirectoryBudget budget;
    std::unordered_map<ArtifactId, std::filesystem::path, StrongIdHash<ArtifactId>> paths;
    std::unordered_map<ArtifactId, ArtifactDescriptor, StrongIdHash<ArtifactId>> descriptors;
    std::unordered_set<ArtifactId, StrongIdHash<ArtifactId>> tombstones;
    bool loaded = false;
    bool read_only = false;

    Result<void> check_growth(std::uint64_t growth, bool maintenance) const {
        if (budget.erase_reserve_bytes > budget.limit_bytes)
            return error(ErrorCode::InvalidArgument, "invalid artifact directory reserve");
        if (budget.limit_bytes == 0)
            return {};
        const auto used = scan_directory(root);
        if (!used)
            return used.error();
        const auto limit = budget.limit_bytes - (maintenance ? 0 : budget.erase_reserve_bytes);
        if (used.value().file_bytes > limit || growth > limit - used.value().file_bytes)
            return error(ErrorCode::ResourceExhausted, "artifact directory capacity exhausted",
                         true);
        return {};
    }

    Result<void> ensure_loaded() {
        std::lock_guard lock(mutex);
        if (loaded)
            return {};
        if (const auto admitted = check_growth(0, true); !admitted)
            return admitted.error();
        std::error_code tombstone_error;
        for (const auto &entry :
             std::filesystem::directory_iterator(root / "tombstones", tombstone_error)) {
            if (tombstone_error)
                break;
            if (!entry.is_regular_file())
                continue;
            const auto id = ArtifactId::parse(entry.path().stem().string());
            if (id)
                tombstones.insert(*id);
        }
        std::ifstream manifest(manifest_path(root));
        if (manifest) {
            std::string line;
            while (std::getline(manifest, line)) {
                if (line.empty())
                    continue;
                std::istringstream input(line);
                std::string id_text, digest_text, media_type;
                std::uint64_t byte_size = 0;
                unsigned int encoding = 0;
                unsigned int sensitivity = 0;
                std::uint16_t major = 0;
                std::uint16_t minor = 0;
                if (!(input >> id_text >> digest_text >> byte_size >> encoding >> sensitivity >>
                      major >> minor >> std::quoted(media_type))) {
                    read_only = true;
                    continue;
                }
                const auto id = ArtifactId::parse(id_text);
                Sha256Digest digest;
                if (!id || !parse_digest(digest_text, digest) || encoding > 2 || sensitivity > 3) {
                    read_only = true;
                    continue;
                }
                if (tombstones.contains(*id)) {
                    paths[*id] = object_path(root, digest);
                    continue;
                }
                ArtifactDescriptor descriptor{*id,
                                              digest,
                                              byte_size,
                                              media_type,
                                              static_cast<ArtifactEncoding>(encoding),
                                              static_cast<Sensitivity>(sensitivity),
                                              {major, minor}};
                const auto path = object_path(root, digest);
                std::error_code ec;
                if (!std::filesystem::is_regular_file(path, ec) || ec) {
                    read_only = true;
                    continue;
                }
                std::ifstream payload(path, std::ios::binary);
                std::vector<std::byte> bytes(
                    static_cast<std::size_t>(std::filesystem::file_size(path, ec)));
                if (ec ||
                    !payload.read(reinterpret_cast<char *>(bytes.data()),
                                  static_cast<std::streamsize>(bytes.size())) ||
                    digest_bytes(std::span<const std::byte>(bytes)) != digest ||
                    bytes.size() != byte_size) {
                    read_only = true;
                    continue;
                }
                auto writer = memory.begin({media_type,
                                            static_cast<ArtifactEncoding>(encoding),
                                            static_cast<Sensitivity>(sensitivity),
                                            {major, minor},
                                            std::max<std::size_t>(byte_size, 1)});
                if (!writer || !writer.value().write(bytes)) {
                    read_only = true;
                    continue;
                }
                auto committed = memory.commit(writer.value());
                if (!committed) {
                    read_only = true;
                    continue;
                }
                descriptors[*id] = descriptor;
                paths[*id] = path;
            }
        }
        loaded = true;
        return {};
    }
};

FileArtifactStore::FileArtifactStore(std::filesystem::path root, std::size_t max_total_bytes,
                                     FileArtifactDirectoryBudget budget)
    : impl_(std::make_unique<Impl>(std::move(root), max_total_bytes, budget)) {}
FileArtifactStore::~FileArtifactStore() = default;

Result<ArtifactWriter> FileArtifactStore::begin(const ArtifactWriteSpec &spec) {
    if (const auto loaded = impl_->ensure_loaded(); !loaded)
        return loaded.error();
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->read_only)
            return error(ErrorCode::DataLoss, "artifact store is read-only after recovery");
    }
    return impl_->memory.begin(spec);
}

Result<ArtifactDescriptor>
FileArtifactStore::planned_descriptor(const ArtifactWriter &writer) const {
    if (!writer.valid())
        return error(ErrorCode::InvalidArgument, "invalid artifact writer");
    const auto digest = digest_bytes(std::span<const std::byte>(*writer.bytes_));
    return ArtifactDescriptor{id_from_digest(digest),
                              digest,
                              writer.size(),
                              writer.spec_.media_type,
                              writer.spec_.encoding,
                              writer.spec_.sensitivity,
                              writer.spec_.content_schema};
}

Result<ArtifactDescriptor> FileArtifactStore::commit(ArtifactWriter &writer) {
    if (const auto loaded = impl_->ensure_loaded(); !loaded)
        return loaded.error();
    const auto candidate = planned_descriptor(writer);
    if (!candidate)
        return candidate.error();
    const auto &planned = candidate.value();
    const auto &digest = planned.digest;
    const std::lock_guard lock(impl_->mutex);
    if (impl_->read_only)
        return error(ErrorCode::DataLoss, "artifact store is read-only after recovery");
    if (impl_->tombstones.contains(planned.id))
        return error(ErrorCode::AlreadyExists, "artifact digest has an erasure tombstone");
    if (const auto existing = impl_->descriptors.find(planned.id);
        existing != impl_->descriptors.end()) {
        if (!same_descriptor(existing->second, planned))
            return error(ErrorCode::DataLoss, "artifact identity has conflicting metadata");
        return existing->second;
    }
    const auto line = manifest_line(planned);
    const auto object = object_path(impl_->root, digest);
    std::error_code error_code;
    const bool published = std::filesystem::exists(object, error_code);
    if (error_code)
        return error(ErrorCode::Unavailable, "artifact object cannot be inspected", true);
    const auto payload_growth = published ? 0 : writer.size();
    if (line.size() > std::numeric_limits<std::uint64_t>::max() - payload_growth)
        return error(ErrorCode::ResourceExhausted, "artifact write size is not representable");
    if (const auto admitted = impl_->check_growth(payload_growth + line.size(), false); !admitted)
        return admitted.error();

    const auto &bytes = *writer.bytes_;
    if (published) {
        // An explicit retry may index a previously published, byte-identical object.
        if (!matches_object(object, bytes))
            return error(ErrorCode::DataLoss, "unindexed artifact bytes do not match");
    } else {
        std::filesystem::create_directories(object.parent_path(), error_code);
        if (error_code)
            return error(ErrorCode::Unavailable, "artifact directory cannot be created", true);
        const auto temp_path = impl_->root / "temp" / (digest.to_string() + ".tmp");
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output)
            return error(ErrorCode::Unavailable, "artifact temp cannot be opened", true);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output)
            return error(ErrorCode::Unavailable, "artifact temp write failed", true);
        std::filesystem::rename(temp_path, object, error_code);
        if (error_code)
            return error(ErrorCode::Unavailable, "artifact atomic publish failed", true);
    }
    auto descriptor = impl_->memory.commit(writer);
    if (!descriptor) {
        if (!published) {
            std::error_code cleanup_error;
            std::filesystem::remove(object, cleanup_error);
        }
        return descriptor.error();
    }
    // Binary mode avoids platform newline translation invalidating the byte budget.
    std::ofstream manifest(manifest_path(impl_->root), std::ios::app | std::ios::binary);
    if (!manifest) {
        (void)impl_->memory.erase({descriptor.value().id, "manifest open failed"});
        impl_->read_only = true;
        return error(ErrorCode::Unavailable, "artifact manifest cannot be opened", true);
    }
    manifest << line;
    manifest.close();
    if (!manifest) {
        (void)impl_->memory.erase({descriptor.value().id, "manifest write failed"});
        impl_->read_only = true;
        return error(ErrorCode::Unavailable, "artifact manifest write failed", true);
    }
    impl_->paths[descriptor.value().id] = object;
    impl_->descriptors[descriptor.value().id] = descriptor.value();
    return descriptor;
}

Result<ArtifactReader> FileArtifactStore::open(const ArtifactDescriptor &descriptor) const {
    if (const auto loaded = impl_->ensure_loaded(); !loaded)
        return loaded.error();
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->tombstones.contains(descriptor.id))
            return error(ErrorCode::NotFound, "artifact was erased");
        const auto found = impl_->paths.find(descriptor.id);
        if (found != impl_->paths.end()) {
            const auto metadata = impl_->descriptors.find(descriptor.id);
            if (metadata == impl_->descriptors.end() ||
                metadata->second.digest != descriptor.digest ||
                metadata->second.byte_size != descriptor.byte_size ||
                metadata->second.media_type != descriptor.media_type ||
                metadata->second.encoding != descriptor.encoding ||
                metadata->second.sensitivity != descriptor.sensitivity ||
                metadata->second.content_schema != descriptor.content_schema) {
                return error(ErrorCode::DataLoss, "artifact descriptor metadata mismatch");
            }
            std::ifstream input(found->second, std::ios::binary);
            if (!input)
                return error(ErrorCode::NotFound, "artifact payload is unavailable");
            std::vector<std::byte> bytes(
                static_cast<std::size_t>(std::filesystem::file_size(found->second)));
            input.read(reinterpret_cast<char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!input || digest_bytes(std::span<const std::byte>(bytes)) != descriptor.digest) {
                return error(ErrorCode::DataLoss, "artifact payload integrity mismatch");
            }
            return ArtifactReader(std::make_shared<const std::vector<std::byte>>(std::move(bytes)));
        }
    }
    return error(ErrorCode::NotFound, "artifact is not present in the persistent manifest");
}

Result<ErasureReceipt> FileArtifactStore::erase(const ArtifactErasureRequest &request) {
    if (const auto loaded = impl_->ensure_loaded(); !loaded)
        return loaded.error();
    const std::lock_guard lock(impl_->mutex);
    const auto found = impl_->paths.find(request.id);
    const bool already_erased = impl_->tombstones.contains(request.id);
    if (found == impl_->paths.end() && !already_erased)
        return error(ErrorCode::NotFound, "artifact is not available");
    if (!already_erased) {
        const auto tombstone_text =
            to_json_string(JsonValue::Object{{"artifact_id", request.id.to_string()},
                                             {"reason", request.reason}}) +
            "\n";
        if (const auto admitted = impl_->check_growth(tombstone_text.size(), true); !admitted)
            return admitted.error();
        const std::string filename = request.id.to_string() + ".json";
        const auto temp = impl_->root / "temp" / (filename + ".erase");
        const auto path = impl_->root / "tombstones" / filename;
        std::ofstream tombstone(temp, std::ios::binary | std::ios::trunc);
        if (!tombstone)
            return error(ErrorCode::Unavailable, "artifact tombstone cannot be written", true);
        tombstone << tombstone_text;
        tombstone.close();
        if (!tombstone)
            return error(ErrorCode::Unavailable, "artifact tombstone write failed", true);
        std::error_code publish_error;
        std::filesystem::rename(temp, path, publish_error);
        if (publish_error)
            return error(ErrorCode::Unavailable, "artifact tombstone publish failed", true);
        (void)impl_->memory.erase(request);
        impl_->tombstones.insert(request.id);
        impl_->descriptors.erase(request.id);
    }
    if (found != impl_->paths.end()) {
        std::error_code ec;
        std::filesystem::remove(found->second, ec);
        if (ec)
            return error(ErrorCode::Unavailable,
                         "artifact tombstone committed but payload removal failed", true);
        impl_->paths.erase(found);
    }
    return ErasureReceipt{request.id, true, true};
}

Result<FileArtifactCommitState>
FileArtifactStore::inspect_commit(const ArtifactDescriptor &descriptor) const {
    if (descriptor.id.is_nil() || id_from_digest(descriptor.digest) != descriptor.id)
        return error(ErrorCode::InvalidArgument, "invalid artifact commit identity");
    if (auto loaded = impl_->ensure_loaded(); !loaded)
        return loaded.error();
    const std::lock_guard lock(impl_->mutex);
    if (impl_->tombstones.contains(descriptor.id))
        return FileArtifactCommitState::Erased;
    const auto known = impl_->descriptors.find(descriptor.id);
    if (known != impl_->descriptors.end()) {
        if (!same_descriptor(known->second, descriptor))
            return error(ErrorCode::DataLoss, "artifact commit metadata mismatch");
        return FileArtifactCommitState::Committed;
    }
    if (impl_->read_only)
        return error(ErrorCode::DataLoss, "artifact commit state is unavailable after recovery");
    std::error_code ec;
    const auto path = object_path(impl_->root, descriptor.digest);
    const bool exists = std::filesystem::exists(path, ec);
    if (ec)
        return error(ErrorCode::Unavailable, "artifact commit state cannot be inspected", true);
    return exists ? FileArtifactCommitState::UnindexedObject : FileArtifactCommitState::Missing;
}

Result<FileArtifactCapacity> FileArtifactStore::directory_capacity() const {
    const std::lock_guard lock(impl_->mutex);
    return scan_directory(impl_->root);
}

} // namespace mira
