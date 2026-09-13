#pragma once

#include "testforge/core/CancellationToken.hpp"
#include "testforge/core/Clock.hpp"
#include "testforge/net/HttpTypes.hpp"
#include "testforge/net/Url.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace testforge::net {

struct HttpClientOptions {
    Milliseconds connectTimeout{5'000};
    Milliseconds requestTimeout{10'000};

    /// Responses larger than this are truncated and flagged. Prevents a
    /// misbehaving endpoint from exhausting memory during a long run.
    std::size_t maxResponseBytes = 8u * 1024u * 1024u;

    bool followRedirects = true;
    int maxRedirects = 5;

    /// Sent on every request unless overridden per-request.
    HttpHeaders defaultHeaders;

    /// Applied before every request. A rejected URL produces a transport
    /// error, never a socket.
    UrlPolicy policy;

    std::string userAgent = "TestForge/0.9";
};

/// Minimal HTTP/1.1 client over POSIX sockets.
///
/// Written rather than pulled in because libcurl's development headers are not
/// present on every machine this has to build on, and because the pieces that
/// matter here — connect with a timeout, read with a deadline, decode chunked
/// transfer encoding, honour a cancellation token — are a few hundred lines.
/// See docs/design-decisions.md.
///
/// Supported: GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS, request and response
/// bodies, Content-Length and chunked responses, redirects, per-request
/// timeouts, cooperative cancellation, IPv4 and IPv6.
///
/// NOT supported: TLS. An https:// URL returns a transport error explaining
/// that HTTPS traffic must go through the Python sidecar
/// (docs/ai-architecture.md). Also absent: connection reuse, compression,
/// cookies, and HTTP/2 — none of which the test workloads need.
///
/// Thread safety: an HttpClient holds no per-request state and may be shared
/// across worker threads. Each send() opens and closes its own socket.
class HttpClient {
 public:
    explicit HttpClient(HttpClientOptions options = {});

    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    /// Performs the request. Never throws for network problems: those come
    /// back as HttpResponse::transportError.
    ///
    /// `cancellation`, when valid, aborts a request in progress — this is what
    /// lets a timed-out test stop instead of blocking on a dead socket.
    [[nodiscard]] HttpResponse send(const HttpRequest& request,
                                    const CancellationToken& cancellation = {});

    [[nodiscard]] HttpResponse get(const std::string& url, const HttpHeaders& headers = {});

    [[nodiscard]] HttpResponse post(const std::string& url,
                                    const std::string& body,
                                    const std::string& contentType = "application/json",
                                    const HttpHeaders& headers = {});

    [[nodiscard]] HttpResponse postJson(const std::string& url,
                                        const json::Value& payload,
                                        const HttpHeaders& headers = {});

    [[nodiscard]] HttpResponse put(const std::string& url,
                                   const std::string& body,
                                   const std::string& contentType = "application/json",
                                   const HttpHeaders& headers = {});

    [[nodiscard]] HttpResponse patch(const std::string& url,
                                     const std::string& body,
                                     const std::string& contentType = "application/json",
                                     const HttpHeaders& headers = {});

    [[nodiscard]] HttpResponse del(const std::string& url, const HttpHeaders& headers = {});

    [[nodiscard]] const HttpClientOptions& options() const noexcept;

    void setPolicy(UrlPolicy policy);

    /// Total requests issued by this client, for run statistics.
    [[nodiscard]] std::uint64_t requestCount() const noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace testforge::net
