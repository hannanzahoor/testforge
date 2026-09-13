#pragma once

#include "testforge/core/Clock.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/net/HttpTypes.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace testforge::net {

/// An inbound request, already parsed.
struct ServerRequest {
    std::string method;
    std::string path;   ///< decoded, without the query string
    std::string query;  ///< raw, without the leading '?'
    HttpHeaders headers;
    std::string body;
    std::string clientAddress;

    /// Values captured from a route pattern such as "/runs/:id".
    std::map<std::string, std::string> pathParameters;

    [[nodiscard]] std::optional<json::Value> json() const;

    [[nodiscard]] std::string queryParameter(std::string_view name,
                                             std::string_view fallback = {}) const;

    [[nodiscard]] int queryParameterInt(std::string_view name, int fallback) const;

    [[nodiscard]] std::string pathParameter(std::string_view name) const;
};

struct ServerResponse {
    int status = 200;
    HttpHeaders headers;
    std::string body;

    static ServerResponse json(int status, const json::Value& payload);
    static ServerResponse text(int status, std::string body);
    static ServerResponse html(int status, std::string body);

    /// RFC 7807-ish error body: {"error": {"code": ..., "message": ...}}
    static ServerResponse error(int status, std::string message, std::string code = {});

    static ServerResponse noContent();
};

/// A small HTTP/1.1 server.
///
/// Written in C++ rather than delegated to Python so that the REST API and the
/// test engine live in one process: a run started over HTTP shares the same
/// registry, the same repository handle and the same in-memory state as one
/// started from the CLI, with no serialisation boundary in between.
///
/// Scope: HTTP/1.1 with Connection: close, one request per connection, a
/// bounded thread pool, request-size limits and a static file handler. No
/// keep-alive, no TLS, no HTTP/2. It binds to loopback by default and is
/// intended for a developer machine or a CI container, not the public
/// internet — see docs/security.md.
class HttpServer {
 public:
    using Handler = std::function<ServerResponse(const ServerRequest&)>;

    explicit HttpServer(ServerConfig config);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    /// Registers a handler. `pattern` may contain ":name" segments, which are
    /// captured into ServerRequest::pathParameters. Later registrations for
    /// the same method+pattern replace earlier ones.
    void route(std::string method, std::string pattern, Handler handler);

    /// Serves files from `directory` for any GET that no route matched.
    /// Traversal outside the directory is refused.
    void serveStaticFiles(std::string directory, std::string urlPrefix = "/");

    /// Binds and starts accepting. Returns false when the port is unavailable.
    bool start();

    /// Stops accepting, waits for in-flight requests, closes the socket.
    void stop();

    /// Blocks until stop() is called from another thread.
    ///
    /// Prefer waitFor() in a process that must also respond to a signal: this
    /// overload cannot be woken by anything but stop().
    void wait();

    /// Waits up to \p timeout for the server to stop.
    ///
    /// Returns true if it has stopped, false if it is still running. Lets a
    /// caller interleave waiting with polling something else — an interrupt
    /// flag, most usefully — without a second thread.
    [[nodiscard]] bool waitFor(Milliseconds timeout);

    [[nodiscard]] bool running() const noexcept;

    /// The port actually bound. Differs from the configured port when 0 was
    /// requested, which the tests use to avoid port collisions.
    [[nodiscard]] int boundPort() const noexcept;

    [[nodiscard]] std::uint64_t requestsHandled() const noexcept;

    /// Connections refused because maxPendingConnections was already
    /// reached. Exposed so the cap is observable in a test rather than
    /// only inferable from a client-side 503.
    [[nodiscard]] std::uint64_t connectionsRejected() const noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace testforge::net
