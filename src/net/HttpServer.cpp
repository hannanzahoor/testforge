#include "testforge/net/HttpServer.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/execution/ThreadPool.hpp"
#include "testforge/net/Url.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace testforge::net {
namespace {

std::string_view mimeTypeFor(std::string_view path) {
    const auto endsWith = [path](std::string_view suffix) {
        return strings::endsWith(path, suffix);
    };
    if (endsWith(".html") || endsWith(".htm"))
        return "text/html; charset=utf-8";
    if (endsWith(".css"))
        return "text/css; charset=utf-8";
    if (endsWith(".js") || endsWith(".mjs"))
        return "application/javascript; charset=utf-8";
    if (endsWith(".json"))
        return "application/json";
    if (endsWith(".svg"))
        return "image/svg+xml";
    if (endsWith(".png"))
        return "image/png";
    if (endsWith(".jpg") || endsWith(".jpeg"))
        return "image/jpeg";
    if (endsWith(".ico"))
        return "image/x-icon";
    if (endsWith(".woff2"))
        return "font/woff2";
    if (endsWith(".txt") || endsWith(".log"))
        return "text/plain; charset=utf-8";
    if (endsWith(".xml"))
        return "application/xml";
    return "application/octet-stream";
}

/// True when `path` is safe to append to a served directory.
///
/// Rejects rather than normalises: any '..' segment, absolute path, NUL byte
/// or backslash means the request does not get a file. Normalising would work
/// too, but refusing is easier to reason about and impossible to get subtly
/// wrong.
bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (const std::string& segment : strings::split(path, '/', true)) {
        if (segment == ".." || segment == ".") {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// ServerRequest / ServerResponse
// ---------------------------------------------------------------------------

std::optional<json::Value> ServerRequest::json() const {
    if (body.empty()) {
        return std::nullopt;
    }
    return json::tryParse(body);
}

std::string ServerRequest::queryParameter(std::string_view name, std::string_view fallback) const {
    for (const auto& [key, value] : parseQuery(query)) {
        if (key == name) {
            return value;
        }
    }
    return std::string(fallback);
}

int ServerRequest::queryParameterInt(std::string_view name, int fallback) const {
    const std::string raw = queryParameter(name);
    std::int64_t parsed = 0;
    if (!raw.empty() && strings::parseInt(raw, parsed)) {
        return static_cast<int>(parsed);
    }
    return fallback;
}

std::string ServerRequest::pathParameter(std::string_view name) const {
    const auto found = pathParameters.find(std::string(name));
    return found == pathParameters.end() ? std::string{} : found->second;
}

ServerResponse ServerResponse::json(int status, const json::Value& payload) {
    ServerResponse response;
    response.status = status;
    response.body = payload.dump(2);
    response.headers.set("Content-Type", "application/json; charset=utf-8");
    return response;
}

ServerResponse ServerResponse::text(int status, std::string body) {
    ServerResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers.set("Content-Type", "text/plain; charset=utf-8");
    return response;
}

ServerResponse ServerResponse::html(int status, std::string body) {
    ServerResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers.set("Content-Type", "text/html; charset=utf-8");
    return response;
}

ServerResponse ServerResponse::error(int status, std::string message, std::string code) {
    json::Value payload = json::Value::object();
    json::Value error = json::Value::object();
    error.set("code", code.empty() ? std::string(reasonPhrase(status)) : code);
    error.set("message", std::move(message));
    error.set("status", status);
    payload.set("error", error);
    return json(status, payload);
}

ServerResponse ServerResponse::noContent() {
    ServerResponse response;
    response.status = 204;
    return response;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HttpServer::Impl {
    explicit Impl(ServerConfig cfg) : config(std::move(cfg)), logger("http.server") {}

    struct Route {
        std::string method;
        std::vector<std::string> segments;  ///< ":name" marks a capture
        Handler handler;
    };

    ServerConfig config;
    Logger logger;

    std::vector<Route> routes;
    std::string staticDirectory;
    std::string staticPrefix = "/";

    int listenFd = -1;
    int boundPort = 0;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> handled{0};

    /// Connections accepted but not yet finished, i.e. queued in the pool or
    /// running in a worker. Bounds how many descriptors the process can be
    /// holding on behalf of clients at once.
    std::atomic<int> inFlight{0};
    std::atomic<std::uint64_t> rejected{0};

    std::thread acceptor;
    std::unique_ptr<ThreadPool> pool;

    std::mutex stopMutex;
    std::condition_variable stopSignal;

    [[nodiscard]] static std::vector<std::string> splitPath(std::string_view path) {
        return strings::split(path, '/', true);
    }

    /// Finds the handler for a request, filling in path parameters.
    const Route* match(const std::string& method,
                       const std::string& path,
                       std::map<std::string, std::string>& parameters) const {
        const std::vector<std::string> segments = splitPath(path);
        const Route* methodMismatch = nullptr;

        for (const Route& route : routes) {
            if (route.segments.size() != segments.size()) {
                continue;
            }
            std::map<std::string, std::string> captured;
            bool matched = true;
            for (std::size_t i = 0; i < segments.size(); ++i) {
                const std::string& pattern = route.segments[i];
                if (!pattern.empty() && pattern.front() == ':') {
                    captured[pattern.substr(1)] = segments[i];
                } else if (pattern != segments[i]) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
            if (route.method != method) {
                // Remember it: the correct answer is 405, not 404.
                methodMismatch = &route;
                continue;
            }
            parameters = std::move(captured);
            return &route;
        }

        if (methodMismatch != nullptr) {
            parameters.clear();
            parameters["__method_not_allowed"] = methodMismatch->method;
        }
        return nullptr;
    }

    std::optional<ServerResponse> serveStatic(const ServerRequest& request) const;

    void handleConnection(int clientFd, std::string clientAddress);

    void acceptLoop();
};

#if defined(_WIN32)

std::optional<ServerResponse> HttpServer::Impl::serveStatic(const ServerRequest&) const {
    return std::nullopt;
}

void HttpServer::Impl::handleConnection(int, std::string) {}

void HttpServer::Impl::acceptLoop() {}

HttpServer::HttpServer(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

HttpServer::~HttpServer() = default;

void HttpServer::route(std::string method, std::string pattern, Handler handler) {
    impl_->routes.push_back(
        {strings::toUpper(method), Impl::splitPath(pattern), std::move(handler)});
}

void HttpServer::serveStaticFiles(std::string directory, std::string urlPrefix) {
    impl_->staticDirectory = std::move(directory);
    impl_->staticPrefix = std::move(urlPrefix);
}

bool HttpServer::start() {
    impl_->logger.error("the HTTP server requires POSIX sockets and is unavailable here");
    return false;
}

void HttpServer::stop() {}

void HttpServer::wait() {}

bool HttpServer::waitFor(Milliseconds) {
    return true;
}

bool HttpServer::running() const noexcept {
    return false;
}

int HttpServer::boundPort() const noexcept {
    return 0;
}

std::uint64_t HttpServer::requestsHandled() const noexcept {
    return 0;
}

std::uint64_t HttpServer::connectionsRejected() const noexcept {
    return 0;
}

#else

std::optional<ServerResponse> HttpServer::Impl::serveStatic(const ServerRequest& request) const {
    if (staticDirectory.empty() || (request.method != "GET" && request.method != "HEAD")) {
        return std::nullopt;
    }

    std::string relative = request.path;
    if (!staticPrefix.empty() && staticPrefix != "/" &&
        strings::startsWith(relative, staticPrefix)) {
        relative = relative.substr(staticPrefix.size());
    }
    while (!relative.empty() && relative.front() == '/') {
        relative.erase(relative.begin());
    }
    if (relative.empty()) {
        relative = "index.html";
    }

    if (!isSafeRelativePath(relative)) {
        return ServerResponse::error(400, "invalid path", "INVALID_PATH");
    }

    std::string full = staticDirectory;
    if (!full.empty() && full.back() != '/') {
        full.push_back('/');
    }
    full += relative;

    struct ::stat info {};

    if (::stat(full.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
    }

    std::ifstream file(full, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();

    ServerResponse response;
    response.status = 200;
    response.body = contents.str();
    response.headers.set("Content-Type", std::string(mimeTypeFor(relative)));
    response.headers.set("Cache-Control", "no-cache");
    return response;
}

void HttpServer::Impl::handleConnection(int clientFd, std::string clientAddress) {
    // The descriptor is owned by this function from here on.
    struct FdGuard {
        int fd;

        ~FdGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } guard{clientFd};

    ::timeval timeout{};
    timeout.tv_sec = 10;
    ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string raw;
    std::size_t headerEnd = std::string::npos;
    const auto maxRequest = static_cast<std::size_t>(config.maxRequestBytes);

    // ----- read the head -----
    while (headerEnd == std::string::npos) {
        std::array<char, 8192> chunk{};
        const ssize_t got = ::recv(clientFd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            return;  // client vanished or timed out; nothing to answer
        }
        raw.append(chunk.data(), static_cast<std::size_t>(got));
        headerEnd = raw.find("\r\n\r\n");
        if (raw.size() > maxRequest) {
            const ServerResponse tooLarge =
                ServerResponse::error(413, "request exceeds the configured size limit");
            std::ostringstream out;
            out << "HTTP/1.1 413 Payload Too Large\r\nContent-Length: " << tooLarge.body.size()
                << "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                << tooLarge.body;
            const std::string text = out.str();
            ::send(clientFd, text.data(), text.size(), MSG_NOSIGNAL);
            return;
        }
    }

    ServerRequest request;
    request.clientAddress = std::move(clientAddress);

    const std::string head = raw.substr(0, headerEnd);
    std::string body = raw.substr(headerEnd + 4);
    const std::vector<std::string> lines = strings::splitLines(head);
    if (lines.empty()) {
        return;
    }

    {
        const std::vector<std::string> parts = strings::split(lines.front(), ' ', true);
        if (parts.size() < 2) {
            return;
        }
        request.method = strings::toUpper(parts[0]);
        std::string target = parts[1];
        if (const std::size_t question = target.find('?'); question != std::string::npos) {
            request.query = target.substr(question + 1);
            target = target.substr(0, question);
        }
        request.path = urlDecode(target);
    }

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers.add(strings::trim(lines[i].substr(0, colon)),
                            strings::trim(lines[i].substr(colon + 1)));
    }

    // ----- read the body -----
    //
    // Chunked request bodies are not supported, and saying so is the point.
    // Only Content-Length is honoured below, so a chunked POST used to arrive
    // at the handler with an empty body and no indication anything was wrong —
    // the handler would reject it as malformed JSON, or worse, act on the
    // absence. An explicit 400 naming the reason is a far better failure.
    if (const std::optional<std::string> encoding = request.headers.get("Transfer-Encoding");
        encoding.has_value() && !strings::trim(*encoding).empty() &&
        !strings::equalsIgnoreCase(strings::trim(*encoding), "identity")) {
        const ServerResponse unsupported = ServerResponse::error(
            400,
            "chunked request bodies are not supported; send a Content-Length instead",
            "UNSUPPORTED_TRANSFER_ENCODING");
        std::ostringstream out;
        out << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << unsupported.body.size()
            << "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            << unsupported.body;
        const std::string text = out.str();
        ::send(clientFd, text.data(), text.size(), MSG_NOSIGNAL);
        return;
    }

    std::int64_t contentLength = 0;
    if (const std::optional<std::string> value = request.headers.get("Content-Length");
        value.has_value()) {
        if (!strings::parseInt(*value, contentLength) || contentLength < 0) {
            contentLength = 0;
        }
    }
    if (contentLength > config.maxRequestBytes) {
        const ServerResponse tooLarge = ServerResponse::error(413, "request body is too large");
        std::ostringstream out;
        out << "HTTP/1.1 413 Payload Too Large\r\nContent-Length: " << tooLarge.body.size()
            << "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            << tooLarge.body;
        const std::string text = out.str();
        ::send(clientFd, text.data(), text.size(), MSG_NOSIGNAL);
        return;
    }
    while (body.size() < static_cast<std::size_t>(contentLength)) {
        std::array<char, 8192> chunk{};
        const ssize_t got = ::recv(clientFd, chunk.data(), chunk.size(), 0);
        if (got <= 0) {
            break;
        }
        body.append(chunk.data(), static_cast<std::size_t>(got));
    }
    request.body = body.substr(0, static_cast<std::size_t>(contentLength));

    // ----- dispatch -----
    ServerResponse response;
    std::map<std::string, std::string> parameters;
    const Route* route = match(request.method, request.path, parameters);

    if (route != nullptr) {
        request.pathParameters = std::move(parameters);
        try {
            response = route->handler(request);
        } catch (const std::exception& error) {
            // A handler that throws must produce a 500, not kill the server.
            logger.error("handler threw",
                         {{"path", request.path}, {"error", std::string(error.what())}});
            response = ServerResponse::error(500, "internal error handling the request");
        } catch (...) {
            logger.error("handler threw a non-std exception", {{"path", request.path}});
            response = ServerResponse::error(500, "internal error handling the request");
        }
    } else if (std::optional<ServerResponse> staticResponse = serveStatic(request);
               staticResponse.has_value()) {
        response = std::move(*staticResponse);
    } else if (parameters.count("__method_not_allowed") != 0) {
        response = ServerResponse::error(
            405, "method " + request.method + " is not allowed for " + request.path);
    } else {
        response =
            ServerResponse::error(404, "no route for " + request.method + " " + request.path);
    }

    // ----- write -----
    response.headers.set("Content-Length", std::to_string(response.body.size()));
    response.headers.set("Connection", "close");
    if (!response.headers.contains("Content-Type")) {
        response.headers.set("Content-Type", "application/json; charset=utf-8");
    }
    // The dashboard is served from the same origin, but a developer poking at
    // the API from a file:// page or another port is a normal thing to do.
    response.headers.set("Access-Control-Allow-Origin", "*");
    response.headers.set("X-Content-Type-Options", "nosniff");

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << reasonPhrase(response.status) << "\r\n";
    for (const auto& [name, value] : response.headers.items()) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    if (request.method != "HEAD") {
        out << response.body;
    }

    const std::string text = out.str();
    std::size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t written =
            ::send(clientFd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(written);
    }

    handled.fetch_add(1, std::memory_order_relaxed);
}

void HttpServer::Impl::acceptLoop() {
    while (running.load(std::memory_order_acquire)) {
        ::pollfd descriptor{listenFd, POLLIN, 0};
        // Poll rather than block in accept() so stop() is noticed within 200ms
        // without needing a self-pipe.
        const int ready = ::poll(&descriptor, 1, 200);
        if (ready <= 0) {
            continue;
        }

        ::sockaddr_storage address{};
        ::socklen_t addressLength = sizeof(address);
        const int clientFd =
            ::accept(listenFd, reinterpret_cast<::sockaddr*>(&address), &addressLength);
        if (clientFd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (running.load(std::memory_order_acquire)) {
                logger.warn("accept() failed", {{"error", std::string(std::strerror(errno))}});
            }
            continue;
        }

        std::array<char, INET6_ADDRSTRLEN> text{};
        std::string clientAddress = "unknown";
        if (address.ss_family == AF_INET) {
            const auto* v4 = reinterpret_cast<const ::sockaddr_in*>(&address);
            if (::inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size()) != nullptr) {
                clientAddress = text.data();
            }
        } else if (address.ss_family == AF_INET6) {
            const auto* v6 = reinterpret_cast<const ::sockaddr_in6*>(&address);
            if (::inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), text.size()) != nullptr) {
                clientAddress = text.data();
            }
        }

        // Refuse rather than queue without limit. The pool's queue has no
        // depth bound, so every accepted-but-unhandled connection is a
        // descriptor this process holds for up to the socket read timeout. A
        // client opening sockets faster than the workers drain them would
        // otherwise exhaust the descriptor table; a 503 is a bounded and
        // visible failure instead.
        const int cap = std::max(1, config.maxPendingConnections);
        if (inFlight.load(std::memory_order_acquire) >= cap) {
            rejected.fetch_add(1, std::memory_order_relaxed);
            static constexpr std::string_view kBusyBody =
                R"({"error":{"code":"SERVER_BUSY","message":"too many connections in flight"}})";
            // Length computed rather than written out: a hand-counted
            // Content-Length that drifts leaves the client hanging.
            const std::string busy =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Content-Length: " +
                std::to_string(kBusyBody.size()) +
                "\r\n"
                "Connection: close\r\n"
                "Retry-After: 1\r\n"
                "\r\n" +
                std::string(kBusyBody);
            ::send(clientFd, busy.data(), busy.size(), MSG_NOSIGNAL);
            ::close(clientFd);
            continue;
        }

        inFlight.fetch_add(1, std::memory_order_release);
        try {
            pool->submit([this, clientFd, clientAddress] {
                // Decrement on every exit path, including a throwing handler.
                struct InFlightGuard {
                    std::atomic<int>* counter;
                    ~InFlightGuard() { counter->fetch_sub(1, std::memory_order_release); }
                } guard{&inFlight};
                handleConnection(clientFd, clientAddress);
            });
        } catch (const std::exception&) {
            // Pool is shutting down; close rather than leak the descriptor.
            inFlight.fetch_sub(1, std::memory_order_release);
            ::close(clientFd);
        }
    }
}

HttpServer::HttpServer(ServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::route(std::string method, std::string pattern, Handler handler) {
    Impl::Route entry;
    entry.method = strings::toUpper(method);
    entry.segments = Impl::splitPath(pattern);
    entry.handler = std::move(handler);

    // Replacing keeps registration idempotent, which matters when the service
    // layer re-registers routes after a configuration reload.
    for (Impl::Route& existing : impl_->routes) {
        if (existing.method == entry.method && existing.segments == entry.segments) {
            existing.handler = std::move(entry.handler);
            return;
        }
    }
    impl_->routes.push_back(std::move(entry));
}

void HttpServer::serveStaticFiles(std::string directory, std::string urlPrefix) {
    impl_->staticDirectory = std::move(directory);
    impl_->staticPrefix = std::move(urlPrefix);
}

bool HttpServer::start() {
    Impl& impl = *impl_;
    if (impl.running.load(std::memory_order_acquire)) {
        return true;
    }

    const bool ipv6 = impl.config.host.find(':') != std::string::npos;
    impl.listenFd = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (impl.listenFd < 0) {
        impl.logger.error("socket() failed", {{"error", std::string(std::strerror(errno))}});
        return false;
    }

    int one = 1;
    // Without SO_REUSEADDR a restart within the TIME_WAIT window fails to bind.
    ::setsockopt(impl.listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int bindResult = -1;
    if (ipv6) {
        ::sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(static_cast<std::uint16_t>(impl.config.port));
        if (::inet_pton(AF_INET6, impl.config.host.c_str(), &address.sin6_addr) != 1) {
            address.sin6_addr = in6addr_loopback;
        }
        bindResult =
            ::bind(impl.listenFd, reinterpret_cast<::sockaddr*>(&address), sizeof(address));
    } else {
        ::sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(impl.config.port));
        if (::inet_pton(AF_INET, impl.config.host.c_str(), &address.sin_addr) != 1) {
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        bindResult =
            ::bind(impl.listenFd, reinterpret_cast<::sockaddr*>(&address), sizeof(address));
    }

    if (bindResult != 0) {
        impl.logger.error("bind() failed",
                          {{"host", impl.config.host},
                           {"port", impl.config.port},
                           {"error", std::string(std::strerror(errno))}});
        ::close(impl.listenFd);
        impl.listenFd = -1;
        return false;
    }

    if (::listen(impl.listenFd, 64) != 0) {
        impl.logger.error("listen() failed", {{"error", std::string(std::strerror(errno))}});
        ::close(impl.listenFd);
        impl.listenFd = -1;
        return false;
    }

    // Report the port actually bound, which matters when 0 was requested.
    ::sockaddr_storage bound{};
    ::socklen_t boundLength = sizeof(bound);
    if (::getsockname(impl.listenFd, reinterpret_cast<::sockaddr*>(&bound), &boundLength) == 0) {
        impl.boundPort = bound.ss_family == AF_INET6
                             ? ntohs(reinterpret_cast<::sockaddr_in6*>(&bound)->sin6_port)
                             : ntohs(reinterpret_cast<::sockaddr_in*>(&bound)->sin_port);
    } else {
        impl.boundPort = impl.config.port;
    }

    impl.pool = std::make_unique<ThreadPool>(
        static_cast<std::size_t>(std::max(1, impl.config.workers)), "http");
    impl.running.store(true, std::memory_order_release);
    impl.acceptor = std::thread([&impl] { impl.acceptLoop(); });

    impl.logger.info("listening", {{"host", impl.config.host}, {"port", impl.boundPort}});
    return true;
}

void HttpServer::stop() {
    Impl& impl = *impl_;
    if (!impl.running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Join the acceptor BEFORE touching the listening descriptor.
    //
    // The obvious ordering — close the socket to wake the acceptor, then join —
    // is a data race and worse: the acceptor may be between its running-flag
    // check and its poll()/accept(), so it can operate on a descriptor this
    // thread has already closed. On a busy process that number may by then
    // refer to a completely different file. ThreadSanitizer found this.
    //
    // No wake-up is needed: acceptLoop polls with a 200 ms timeout and checks
    // the flag each time round, so it exits on its own. The cost is up to
    // 200 ms on shutdown, which is a fine price for the descriptor being owned
    // by exactly one thread at a time.
    if (impl.acceptor.joinable()) {
        impl.acceptor.join();
    }

    // The acceptor has exited, so this thread is now the only owner.
    if (impl.listenFd >= 0) {
        ::shutdown(impl.listenFd, SHUT_RDWR);
        ::close(impl.listenFd);
        impl.listenFd = -1;
    }

    if (impl.pool) {
        impl.pool->shutdown();
        impl.pool.reset();
    }

    impl.stopSignal.notify_all();
    impl.logger.info(
        "stopped",
        {{"requests", static_cast<std::int64_t>(impl.handled.load(std::memory_order_relaxed))}});
}

void HttpServer::wait() {
    std::unique_lock<std::mutex> lock(impl_->stopMutex);
    impl_->stopSignal.wait(lock,
                           [this] { return !impl_->running.load(std::memory_order_acquire); });
}

bool HttpServer::waitFor(Milliseconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->stopMutex);
    return impl_->stopSignal.wait_for(
        lock, timeout, [this] { return !impl_->running.load(std::memory_order_acquire); });
}

bool HttpServer::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

int HttpServer::boundPort() const noexcept {
    return impl_->boundPort;
}

std::uint64_t HttpServer::requestsHandled() const noexcept {
    return impl_->handled.load(std::memory_order_relaxed);
}

std::uint64_t HttpServer::connectionsRejected() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

#endif  // _WIN32

}  // namespace testforge::net
