#include "testforge/net/HttpClient.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sstream>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace testforge::net {
namespace {

constexpr std::size_t kReadChunk = 16384;

/// Ceiling on the response head, independent of the body cap. A server that
/// never sends the blank line separating headers from body must not be able
/// to make the client buffer without limit.
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

#if !defined(_WIN32)

/// EAGAIN and EWOULDBLOCK have the same value on Linux, but POSIX does not
/// require that, so both must be checked for portability. Writing the
/// comparison inline makes GCC warn about a tautology; keeping it here
/// documents the intent and compiles to the right thing on either kind of
/// platform.
constexpr bool wouldBlock(int error) noexcept {
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    return error == EAGAIN || error == EWOULDBLOCK;
#else
    return error == EAGAIN;
#endif
}

/// RAII socket handle. Every early return in this file relies on it.
class Socket {
 public:
    Socket() = default;

    explicit Socket(int fd) : fd_(fd) {}

    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

 private:
    int fd_ = -1;
};

/// addrinfo list wrapper, so an early return cannot leak it.
class AddressList {
 public:
    ~AddressList() {
        if (head_ != nullptr) {
            ::freeaddrinfo(head_);
        }
    }

    AddressList() = default;
    AddressList(const AddressList&) = delete;
    AddressList& operator=(const AddressList&) = delete;
    AddressList(AddressList&&) = delete;
    AddressList& operator=(AddressList&&) = delete;

    int resolve(const std::string& host, int port) {
        ::addrinfo hints{};
        hints.ai_family = AF_UNSPEC;  // IPv4 or IPv6, whichever resolves
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        return ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &head_);
    }

    [[nodiscard]] const ::addrinfo* head() const noexcept { return head_; }

 private:
    ::addrinfo* head_ = nullptr;
};

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/// Connects with a deadline, honouring cancellation.
///
/// A blocking connect() can sit for over a minute on a dropped packet, which
/// would blow through any test timeout. Non-blocking connect plus poll() puts
/// the deadline under our control.
bool connectWithTimeout(Socket& socket,
                        const ::addrinfo* address,
                        Milliseconds timeout,
                        const CancellationToken& cancellation,
                        std::string& error) {
    Socket candidate(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (!candidate.valid()) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }
    setNonBlocking(candidate.fd());

    const int connectResult = ::connect(candidate.fd(), address->ai_addr, address->ai_addrlen);
    if (connectResult == 0) {
        socket = std::move(candidate);
        return true;
    }
    if (errno != EINPROGRESS) {
        error = std::string("connect() failed: ") + std::strerror(errno);
        return false;
    }

    const auto deadline = SteadyClock::now() + timeout;
    while (true) {
        if (cancellation.isCancelled()) {
            error = "connect aborted: " + cancellation.reason();
            return false;
        }
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            error = "connect timed out after " + formatDuration(timeout);
            return false;
        }
        const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - now);
        // Cap each poll so cancellation is noticed promptly.
        const int slice = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));

        ::pollfd descriptor{candidate.fd(), POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, slice);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("poll() failed during connect: ") + std::strerror(errno);
            return false;
        }
        if (ready == 0) {
            continue;  // slice expired; loop re-checks the real deadline
        }

        // poll() reporting writability does not mean success: the actual
        // outcome is in SO_ERROR.
        int socketError = 0;
        ::socklen_t length = sizeof(socketError);
        if (::getsockopt(candidate.fd(), SOL_SOCKET, SO_ERROR, &socketError, &length) < 0) {
            error = std::string("getsockopt() failed: ") + std::strerror(errno);
            return false;
        }
        if (socketError != 0) {
            error = std::string("connect failed: ") + std::strerror(socketError);
            return false;
        }
        socket = std::move(candidate);
        return true;
    }
}

bool sendAll(int fd,
             std::string_view data,
             SteadyClock::time_point deadline,
             const CancellationToken& cancellation,
             std::string& error) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (cancellation.isCancelled()) {
            error = "send aborted: " + cancellation.reason();
            return false;
        }
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            error = "timed out while sending the request";
            return false;
        }

        // MSG_NOSIGNAL: without it, writing to a socket the peer already
        // closed raises SIGPIPE and kills the whole process.
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && wouldBlock(errno)) {
            const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - now);
            const int slice = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));
            ::pollfd descriptor{fd, POLLOUT, 0};
            ::poll(&descriptor, 1, slice);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        error = std::string("send() failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

enum class ReadOutcome { Data, Eof, Timeout, Cancelled, Error };

ReadOutcome readSome(int fd,
                     std::string& buffer,
                     SteadyClock::time_point deadline,
                     const CancellationToken& cancellation,
                     std::string& error) {
    while (true) {
        if (cancellation.isCancelled()) {
            error = "read aborted: " + cancellation.reason();
            return ReadOutcome::Cancelled;
        }
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            error = "timed out while reading the response";
            return ReadOutcome::Timeout;
        }

        std::array<char, kReadChunk> chunk{};
        const ssize_t got = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (got > 0) {
            buffer.append(chunk.data(), static_cast<std::size_t>(got));
            return ReadOutcome::Data;
        }
        if (got == 0) {
            return ReadOutcome::Eof;
        }
        if (errno == EINTR) {
            continue;
        }
        if (wouldBlock(errno)) {
            const auto remaining = std::chrono::duration_cast<Milliseconds>(deadline - now);
            const int slice = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));
            ::pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, slice);
            if (ready < 0 && errno != EINTR) {
                error = std::string("poll() failed during read: ") + std::strerror(errno);
                return ReadOutcome::Error;
            }
            continue;
        }
        error = std::string("recv() failed: ") + std::strerror(errno);
        return ReadOutcome::Error;
    }
}

/// Decodes chunked transfer encoding. Returns false on a malformed stream.
bool decodeChunked(std::string_view raw, std::string& out, bool& complete) {
    std::size_t pos = 0;
    complete = false;
    while (pos < raw.size()) {
        const std::size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) {
            return true;  // need more data
        }
        std::string_view sizeLine = raw.substr(pos, lineEnd - pos);
        // A chunk-size line may carry extensions after a ';'.
        if (const std::size_t semicolon = sizeLine.find(';'); semicolon != std::string_view::npos) {
            sizeLine = sizeLine.substr(0, semicolon);
        }

        std::size_t chunkSize = 0;
        if (sizeLine.empty()) {
            return false;
        }
        for (const char c : sizeLine) {
            int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else if (c == ' ' || c == '\t') {
                continue;
            } else {
                return false;
            }
            chunkSize = chunkSize * 16 + static_cast<std::size_t>(digit);
        }

        const std::size_t dataStart = lineEnd + 2;
        if (chunkSize == 0) {
            complete = true;
            return true;
        }
        if (dataStart + chunkSize + 2 > raw.size()) {
            return true;  // need more data
        }
        out.append(raw.substr(dataStart, chunkSize));
        pos = dataStart + chunkSize + 2;
    }
    return true;
}

#endif  // !_WIN32

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct HttpClient::Impl {
    explicit Impl(HttpClientOptions opts) : options(std::move(opts)), logger("http") {}

    HttpClientOptions options;
    Logger logger;
    std::atomic<std::uint64_t> requests{0};

    HttpResponse perform(const HttpRequest& request,
                         const CancellationToken& cancellation,
                         int redirectsRemaining);
};

#if defined(_WIN32)

HttpResponse HttpClient::Impl::perform(const HttpRequest& request, const CancellationToken&, int) {
    HttpResponse response;
    response.requestMethod = request.method;
    response.requestUrl = request.url;
    response.transportError = true;
    response.errorMessage = "the POSIX socket HTTP client is not available on this platform";
    return response;
}

#else

HttpResponse HttpClient::Impl::perform(const HttpRequest& request,
                                       const CancellationToken& cancellation,
                                       int redirectsRemaining) {
    HttpResponse response;
    response.requestMethod = request.method;
    response.requestUrl = request.url;

    Stopwatch watch;
    auto fail = [&response, &watch](std::string message) {
        response.transportError = true;
        response.errorMessage = std::move(message);
        response.elapsed = watch.elapsed();
        return response;
    };

    const std::optional<Url> url = parseUrl(request.url);
    if (!url.has_value()) {
        return fail("not a valid absolute http(s) URL: " + request.url);
    }
    if (const std::string rejection = options.policy.reject(*url); !rejection.empty()) {
        return fail("request blocked by URL policy: " + rejection);
    }
    if (url->isHttps()) {
        // Being explicit beats a confusing handshake failure. See the header.
        return fail(
            "HTTPS is not supported by the built-in client (no TLS stack is linked). Point "
            "TestForge at an http:// endpoint, or route the call through the Python AI sidecar.");
    }

    const Milliseconds requestTimeout =
        request.timeout.count() > 0 ? request.timeout : options.requestTimeout;
    const auto deadline = SteadyClock::now() + requestTimeout;

    AddressList addresses;
    if (const int status = addresses.resolve(url->host, url->port); status != 0) {
        return fail("could not resolve '" + url->host + "': " + ::gai_strerror(status));
    }

    Socket socket;
    std::string connectError = "no addresses returned for " + url->host;
    for (const ::addrinfo* candidate = addresses.head(); candidate != nullptr;
         candidate = candidate->ai_next) {
        const auto now = SteadyClock::now();
        const Milliseconds budget =
            now >= deadline ? Milliseconds{0}
                            : std::min(options.connectTimeout,
                                       std::chrono::duration_cast<Milliseconds>(deadline - now));
        if (connectWithTimeout(socket, candidate, budget, cancellation, connectError)) {
            break;
        }
    }
    if (!socket.valid()) {
        return fail(connectError);
    }

    // Nagle batches small writes; for a request/response protocol that just
    // adds latency to every call.
    int one = 1;
    ::setsockopt(socket.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // ----- build the request -----
    HttpHeaders headers = options.defaultHeaders;
    for (const auto& [name, value] : request.headers.items()) {
        headers.set(name, value);
    }
    headers.set("Host", url->authority());
    if (!headers.contains("User-Agent")) {
        headers.set("User-Agent", options.userAgent);
    }
    if (!headers.contains("Accept")) {
        headers.set("Accept", "*/*");
    }
    // No connection reuse, so say so and let the server close promptly.
    headers.set("Connection", "close");
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
        request.method == "PATCH") {
        headers.set("Content-Length", std::to_string(request.body.size()));
    }

    std::ostringstream wire;
    wire << request.method << ' ' << url->requestTarget() << " HTTP/1.1\r\n";
    for (const auto& [name, value] : headers.items()) {
        wire << name << ": " << value << "\r\n";
    }
    wire << "\r\n";
    wire << request.body;

    std::string sendError;
    if (!sendAll(socket.fd(), wire.str(), deadline, cancellation, sendError)) {
        return fail(sendError);
    }

    // ----- read the response -----
    std::string raw;
    std::string readError;
    std::size_t headerEnd = std::string::npos;
    bool eof = false;

    while (headerEnd == std::string::npos) {
        const ReadOutcome outcome = readSome(socket.fd(), raw, deadline, cancellation, readError);
        if (outcome == ReadOutcome::Eof) {
            eof = true;
            break;
        }
        if (outcome != ReadOutcome::Data) {
            return fail(readError);
        }
        headerEnd = raw.find("\r\n\r\n");
        // maxResponseBytes bounds the *body*; applying it here would reject a
        // large response before the truncation logic below could run.
        if (headerEnd == std::string::npos && raw.size() > kMaxHeaderBytes) {
            return fail("response headers exceeded " + std::to_string(kMaxHeaderBytes) + " bytes");
        }
    }
    if (headerEnd == std::string::npos) {
        return fail(eof ? "connection closed before the response headers were complete"
                        : "malformed response");
    }

    const std::string headerBlock = raw.substr(0, headerEnd);
    std::string bodyBuffer = raw.substr(headerEnd + 4);

    const std::vector<std::string> headerLines = strings::splitLines(headerBlock);
    if (headerLines.empty()) {
        return fail("empty response");
    }

    // Status line: HTTP/1.1 200 OK
    {
        const std::string& statusLine = headerLines.front();
        const std::size_t firstSpace = statusLine.find(' ');
        if (firstSpace == std::string::npos) {
            return fail("malformed status line: " + strings::truncate(statusLine, 120));
        }
        const std::size_t secondSpace = statusLine.find(' ', firstSpace + 1);
        const std::string codeText = statusLine.substr(
            firstSpace + 1,
            secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
        std::int64_t code = 0;
        if (!strings::parseInt(codeText, code)) {
            return fail("malformed status code: " + codeText);
        }
        response.statusCode = static_cast<int>(code);
        response.statusText = secondSpace == std::string::npos
                                  ? std::string(reasonPhrase(response.statusCode))
                                  : strings::trim(statusLine.substr(secondSpace + 1));
    }

    for (std::size_t i = 1; i < headerLines.size(); ++i) {
        const std::string& line = headerLines[i];
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        response.headers.add(strings::trim(line.substr(0, colon)),
                             strings::trim(line.substr(colon + 1)));
    }

    const bool chunked =
        strings::equalsIgnoreCase(response.headers.getOr("Transfer-Encoding", ""), "chunked");
    std::int64_t contentLength = -1;
    if (const std::optional<std::string> value = response.headers.get("Content-Length");
        value.has_value()) {
        std::int64_t parsed = 0;
        if (strings::parseInt(*value, parsed) && parsed >= 0) {
            contentLength = parsed;
        }
    }
    // A HEAD response and 204/304 never carry a body regardless of headers.
    const bool bodyless =
        request.method == "HEAD" || response.statusCode == 204 || response.statusCode == 304;

    bool truncated = false;
    if (!bodyless) {
        while (true) {
            if (chunked) {
                std::string decoded;
                bool complete = false;
                if (!decodeChunked(bodyBuffer, decoded, complete)) {
                    return fail("malformed chunked response body");
                }
                if (complete) {
                    response.body = std::move(decoded);
                    break;
                }
            } else if (contentLength >= 0) {
                if (bodyBuffer.size() >= static_cast<std::size_t>(contentLength)) {
                    response.body = bodyBuffer.substr(0, static_cast<std::size_t>(contentLength));
                    break;
                }
            }

            if (eof) {
                // No Content-Length and no chunking: the body is whatever
                // arrived before the connection closed (HTTP/1.0 style).
                response.body = bodyBuffer;
                break;
            }
            if (bodyBuffer.size() > options.maxResponseBytes) {
                truncated = true;
                response.body = bodyBuffer.substr(0, options.maxResponseBytes);
                break;
            }

            const ReadOutcome outcome =
                readSome(socket.fd(), bodyBuffer, deadline, cancellation, readError);
            if (outcome == ReadOutcome::Eof) {
                eof = true;
                continue;
            }
            if (outcome != ReadOutcome::Data) {
                return fail(readError);
            }
        }
    }

    if (truncated) {
        response.headers.set("X-TestForge-Truncated", "true");
    }

    response.elapsed = watch.elapsed();

    // ----- redirects -----
    if (options.followRedirects && redirectsRemaining > 0 &&
        (response.statusCode == 301 || response.statusCode == 302 || response.statusCode == 303 ||
         response.statusCode == 307 || response.statusCode == 308)) {
        if (const std::optional<std::string> location = response.headers.get("Location");
            location.has_value() && !location->empty()) {
            // Resolving a Location is not the same operation as joinUrl().
            //
            // joinUrl() appends, because its job is base + endpoint:
            // ("http://h/api", "/users") -> "http://h/api/users". A Location
            // header is the opposite: RFC 3986 says a root-relative reference
            // *replaces* the path, so "/landing" from "http://h/redirect" is
            // "http://h/landing" and not "http://h/redirect/landing". Using
            // joinUrl here sent every root-relative redirect to a path that
            // did not exist.
            std::optional<std::string> next;
            if (strings::startsWith(*location, "http://") ||
                strings::startsWith(*location, "https://")) {
                next = *location;
            } else if (location->front() == '/') {
                Url resolved = *url;
                std::string target = *location;
                if (const std::size_t question = target.find('?'); question != std::string::npos) {
                    resolved.query = target.substr(question + 1);
                    target = target.substr(0, question);
                } else {
                    resolved.query.clear();
                }
                resolved.path = target;
                resolved.fragment.clear();
                next = resolved.toString();
            } else {
                // A reference with no leading slash. Rare from real servers,
                // and joinUrl's append is close enough for the shapes that do
                // occur; a full RFC 3986 merge is not worth the code here.
                next = joinUrl(url->toString(), *location);
            }
            if (next.has_value()) {
                HttpRequest followUp = request;
                followUp.url = *next;

                // Credentials do not cross an origin boundary.
                //
                // The follow-up is a copy of the original request, headers
                // included. If the redirect points at a different host, that
                // copy would hand the target's operator whatever Authorization
                // or Cookie the caller attached for the *original* host — the
                // classic credential leak that curl gates behind
                // --location-trusted. The URL policy is re-applied on the next
                // hop, but a permissive policy is the default, so the check
                // has to be here as well.
                //
                // Same origin means identical scheme, host and effective port.
                // Anything else, including http -> https on the same host, is
                // a different origin.
                const std::optional<Url> target = parseUrl(*next);
                const bool sameOrigin = target.has_value() && target->scheme == url->scheme &&
                                        strings::equalsIgnoreCase(target->host, url->host) &&
                                        target->port == url->port;
                if (!sameOrigin) {
                    for (const std::string_view credential :
                         {"Authorization", "Cookie", "Proxy-Authorization"}) {
                        followUp.headers.remove(credential);
                    }
                }

                // 303, and 301/302 in practice, degrade to GET without a body.
                if (response.statusCode == 303 ||
                    ((response.statusCode == 301 || response.statusCode == 302) &&
                     request.method != "GET" && request.method != "HEAD")) {
                    followUp.method = "GET";
                    followUp.body.clear();
                }
                HttpResponse followed = perform(followUp, cancellation, redirectsRemaining - 1);
                followed.elapsed += response.elapsed;
                return followed;
            }
        }
    }

    return response;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

HttpClient::HttpClient(HttpClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

HttpClient::~HttpClient() = default;

HttpClient::HttpClient(HttpClient&&) noexcept = default;

HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

HttpResponse HttpClient::send(const HttpRequest& request, const CancellationToken& cancellation) {
    impl_->requests.fetch_add(1, std::memory_order_relaxed);
    HttpResponse response = impl_->perform(request, cancellation, impl_->options.maxRedirects);

    if (response.transportError) {
        impl_->logger.debug(
            "request failed at the transport layer",
            {{"method", request.method}, {"url", request.url}, {"error", response.errorMessage}});
    } else {
        impl_->logger.debug("request completed",
                            {{"method", request.method},
                             {"url", request.url},
                             {"status", response.statusCode},
                             {"ms", millisOf(response.elapsed)}});
    }
    return response;
}

HttpResponse HttpClient::get(const std::string& url, const HttpHeaders& headers) {
    HttpRequest request = HttpRequest::get(url);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

HttpResponse HttpClient::post(const std::string& url,
                              const std::string& body,
                              const std::string& contentType,
                              const HttpHeaders& headers) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.body = body;
    request.headers.set("Content-Type", contentType);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

HttpResponse HttpClient::postJson(const std::string& url,
                                  const json::Value& payload,
                                  const HttpHeaders& headers) {
    HttpRequest request = HttpRequest::jsonPost(url, payload);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

HttpResponse HttpClient::put(const std::string& url,
                             const std::string& body,
                             const std::string& contentType,
                             const HttpHeaders& headers) {
    HttpRequest request;
    request.method = "PUT";
    request.url = url;
    request.body = body;
    request.headers.set("Content-Type", contentType);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

HttpResponse HttpClient::patch(const std::string& url,
                               const std::string& body,
                               const std::string& contentType,
                               const HttpHeaders& headers) {
    HttpRequest request;
    request.method = "PATCH";
    request.url = url;
    request.body = body;
    request.headers.set("Content-Type", contentType);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

HttpResponse HttpClient::del(const std::string& url, const HttpHeaders& headers) {
    HttpRequest request;
    request.method = "DELETE";
    request.url = url;
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(request);
}

const HttpClientOptions& HttpClient::options() const noexcept {
    return impl_->options;
}

void HttpClient::setPolicy(UrlPolicy policy) {
    impl_->options.policy = std::move(policy);
}

std::uint64_t HttpClient::requestCount() const noexcept {
    return impl_->requests.load(std::memory_order_relaxed);
}

}  // namespace testforge::net
