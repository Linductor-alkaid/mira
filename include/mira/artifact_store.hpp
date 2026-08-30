#pragma once

#include <mira/event_store.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mira {

enum class ArtifactEncoding : std::uint8_t { Raw, Utf8, Binary };
enum class Sensitivity : std::uint8_t { Public, Internal, Sensitive, Secret };

struct ArtifactDescriptor final {
    ArtifactId id;
    Sha256Digest digest;
    std::uint64_t byte_size = 0;
    std::string media_type;
    ArtifactEncoding encoding = ArtifactEncoding::Binary;
    Sensitivity sensitivity = Sensitivity::Internal;
    SchemaVersion content_schema;
};

struct ArtifactWriteSpec final {
    std::string media_type = "application/octet-stream";
    ArtifactEncoding encoding = ArtifactEncoding::Binary;
    Sensitivity sensitivity = Sensitivity::Internal;
    SchemaVersion content_schema;
    std::size_t max_bytes = 16ULL * 1024ULL * 1024ULL;
};

struct ArtifactErasureRequest final {
    ArtifactId id;
    std::string reason;
};

struct ErasureReceipt final {
    ArtifactId id;
    bool payload_erased = false;
    bool tombstone_written = false;
};

class ArtifactWriter final {
  public:
    ArtifactWriter() = default;
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(bytes_); }
    Result<void> write(const void *data, std::size_t size);
    Result<void> write(const std::vector<std::byte> &data);
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    friend class MemoryArtifactStore;
    friend class FileArtifactStore;
    ArtifactWriter(std::shared_ptr<std::vector<std::byte>> bytes, ArtifactWriteSpec spec)
        : bytes_(std::move(bytes)), spec_(std::move(spec)) {}
    std::shared_ptr<std::vector<std::byte>> bytes_;
    ArtifactWriteSpec spec_;
};

class ArtifactReader final {
  public:
    ArtifactReader() = default;
    explicit ArtifactReader(std::shared_ptr<const std::vector<std::byte>> bytes)
        : bytes_(std::move(bytes)) {}
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(bytes_); }
    [[nodiscard]] const std::vector<std::byte> &bytes() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
};

class IArtifactStore {
  public:
    virtual ~IArtifactStore() = default;
    virtual Result<ArtifactWriter> begin(const ArtifactWriteSpec &) = 0;
    virtual Result<ArtifactDescriptor> commit(ArtifactWriter &) = 0;
    virtual Result<ArtifactReader> open(const ArtifactDescriptor &) const = 0;
    virtual Result<ErasureReceipt> erase(const ArtifactErasureRequest &) = 0;
};

class MemoryArtifactStore final : public IArtifactStore {
  public:
    explicit MemoryArtifactStore(std::size_t max_total_bytes = 256ULL * 1024ULL * 1024ULL);
    ~MemoryArtifactStore() override;
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &) override;
    Result<ArtifactDescriptor> commit(ArtifactWriter &) override;
    Result<ArtifactReader> open(const ArtifactDescriptor &) const override;
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &) override;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FileArtifactStore final : public IArtifactStore {
  public:
    FileArtifactStore(std::filesystem::path root,
                      std::size_t max_total_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL);
    ~FileArtifactStore() override;
    Result<ArtifactWriter> begin(const ArtifactWriteSpec &) override;
    Result<ArtifactDescriptor> commit(ArtifactWriter &) override;
    Result<ArtifactReader> open(const ArtifactDescriptor &) const override;
    Result<ErasureReceipt> erase(const ArtifactErasureRequest &) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira
