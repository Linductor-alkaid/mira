#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_profile.hpp>
#include <mira/model_transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace executor {
class Executor;
} // namespace executor

namespace mira {

// One provider-side temporary file. The opaque ID is never treated as a URL
// and is validated before being placed into a request path.
struct RemoteFileRef final {
    ArtifactRef source;
    std::string provider_file_id;
};

enum class RemoteFileAuditKind : std::uint8_t {
    Uploaded,
    DeleteScheduled,
    Deleted,
    DeleteFailed,
    DeleteCancelled,
};

struct RemoteFileAudit final {
    RemoteFileAuditKind kind = RemoteFileAuditKind::Uploaded;
    ArtifactId artifact_id;
    // A digest of the provider ID is recorded instead of the opaque value.
    Hash provider_id_digest{};
    std::string safe_detail;
};

// Upload/delete boundary injected into a Provider. Implementations must keep
// cleanup work under Executor ownership and expose failures through audit().
class IRemoteFileStore {
  public:
    virtual ~IRemoteFileStore() = default;
    [[nodiscard]] virtual Result<RemoteFileRef> upload(const ArtifactRef &source,
                                                       const std::string &display_name,
                                                       const ModelDataPolicy &policy,
                                                       const OperationContext &context) = 0;
    [[nodiscard]] virtual Result<void> retire(const RemoteFileRef &file,
                                              std::chrono::seconds retention) = 0;
    [[nodiscard]] virtual std::vector<RemoteFileAudit> audit() const = 0;
};

// OpenAI-compatible /files implementation. Upload is synchronous over the
// injected Executor-managed transport. Delayed deletes use Executor timer
// handles retained by this explicit owner; shutdown cancels and consumes all
// pending futures before returning.
class OpenAiRemoteFileStore final : public IRemoteFileStore {
  public:
    OpenAiRemoteFileStore(executor::Executor &executor, std::shared_ptr<const ModelProfile> profile,
                          std::shared_ptr<IHttpTransport> transport,
                          std::shared_ptr<IArtifactSource> artifacts);
    ~OpenAiRemoteFileStore() override;

    OpenAiRemoteFileStore(const OpenAiRemoteFileStore &) = delete;
    OpenAiRemoteFileStore &operator=(const OpenAiRemoteFileStore &) = delete;

    [[nodiscard]] Result<RemoteFileRef> upload(const ArtifactRef &source,
                                               const std::string &display_name,
                                               const ModelDataPolicy &policy,
                                               const OperationContext &context) override;
    [[nodiscard]] Result<void> retire(const RemoteFileRef &file,
                                      std::chrono::seconds retention) override;
    [[nodiscard]] std::vector<RemoteFileAudit> audit() const override;
    void shutdown();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace mira
