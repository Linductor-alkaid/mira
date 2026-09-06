#pragma once

// Portable socket-based HTTP/1.1 transport for Mira model providers. This is
// a platform adapter: it compiles against POSIX sockets and Winsock and is
// the only place in Mira (outside other adapters) allowed to touch them.

#include <mira/model_transport.hpp>

#include <executor/executor.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mira::adapters::net {

struct SocketTransportConfig final {
    std::string worker_name = "mira-provider-io";
    std::size_t worker_count = 1;
    std::size_t max_queued_exchanges = 64;
};

// IHttpTransport over blocking BSD/Winsock sockets, executed on Executor
// blocking I/O workers. Cancellation is cooperative: socket waits poll with
// short timeouts and observe the operation context, so cancel and deadline
// latency is bounded without cross-thread socket closes.
class SocketHttpTransport final : public IHttpTransport {
  public:
    SocketHttpTransport(executor::Executor &executor, std::shared_ptr<ISecretResolver> secrets,
                        std::shared_ptr<ITlsChannelFactory> tls = nullptr,
                        SocketTransportConfig config = SocketTransportConfig{});
    ~SocketHttpTransport() override;

    SocketHttpTransport(const SocketHttpTransport &) = delete;
    SocketHttpTransport &operator=(const SocketHttpTransport &) = delete;

    // Registers and starts the blocking workers. Must be called once before
    // execute(); returns false when the Executor rejects the workers.
    [[nodiscard]] bool start();
    // Idempotent: settles queued exchanges as cancelled and joins the workers
    // through their WorkerHandles.
    void shutdown();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t queued_exchanges() const;

    Result<HttpResponseInfo> execute(const HttpRequest &request, const TransportLimits &limits,
                                     const OperationContext &context,
                                     const HttpChunkCallback &on_chunk,
                                     TransportTrace &trace) override;

    // Internal exchange state shared between the transport implementation
    // units. UrlParts is the validated endpoint of one hop; Exchange owns
    // exactly one connected socket for the duration of one request/response.
    struct UrlParts final {
        std::string scheme;
        std::string host;
        std::uint16_t port = 0;
        std::string path_query;
        bool tls = false;
        [[nodiscard]] std::string origin() const;
    };
    struct Exchange final {
        std::intptr_t socket = -1;
        std::unique_ptr<ITlsChannel> tls;

        Exchange(std::intptr_t handle, std::unique_ptr<ITlsChannel> channel);
        Exchange(Exchange &&other) noexcept;
        Exchange &operator=(Exchange &&) = delete;
        Exchange(const Exchange &) = delete;
        Exchange &operator=(const Exchange &) = delete;
        ~Exchange();

        [[nodiscard]] Result<void> wait(bool readable, std::chrono::steady_clock::time_point until,
                                        const std::function<bool()> &cancelled) const;
        [[nodiscard]] Result<void> handshake(std::chrono::steady_clock::time_point until,
                                             const std::function<bool()> &cancelled) const;
        [[nodiscard]] Result<std::size_t> write_all(const std::string &data,
                                                    std::chrono::steady_clock::time_point until,
                                                    const std::function<bool()> &cancelled) const;
        [[nodiscard]] Result<std::size_t> read_some(std::byte *data, std::size_t capacity,
                                                    std::chrono::steady_clock::time_point until,
                                                    const std::function<bool()> &cancelled) const;
        void close() noexcept;
    };

  private:
    struct Job final {
        HttpRequest request;
        TransportLimits limits;
        std::function<bool()> cancelled;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        HttpChunkCallback on_chunk;
        std::promise<Result<HttpResponseInfo>> settlement;
        std::uint32_t redirects_followed = 0;
        std::string original_origin;
        // Shared so the caller can read the stage trace after settlement,
        // even once the worker dropped its job reference.
        std::shared_ptr<TransportTrace> trace = std::make_shared<TransportTrace>();
    };
    class Worker;

    void run_job(Job &job);
    [[nodiscard]] Result<HttpResponseInfo> perform_exchange(Job &job, TransportTrace &trace);
    [[nodiscard]] Result<HttpResponseInfo>
    finish_exchange(Exchange exchange, Job &job, const UrlParts &parts,
                    const std::optional<std::string> &authorization, bool cross_origin,
                    bool forward_proxy, const std::optional<std::string> &proxy_authorization,
                    std::chrono::steady_clock::time_point total_deadline,
                    std::chrono::steady_clock::time_point started_at, TransportTrace &trace);

    executor::Executor &executor_;
    std::shared_ptr<ISecretResolver> secrets_;
    std::shared_ptr<ITlsChannelFactory> tls_;
    SocketTransportConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::deque<std::shared_ptr<Job>> queue_;
    bool stopping_ = false;
    std::size_t active_workers_ = 0;
    std::vector<executor::WorkerHandle> handles_;
    bool started_ = false;
};

} // namespace mira::adapters::net
