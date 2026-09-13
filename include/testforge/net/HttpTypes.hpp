#pragma once

#include "testforge/core/Clock.hpp"
#include "testforge/core/Json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace testforge::net {

/// Case-insensitive, order-preserving header collection.
///
/// A vector rather than a map because HTTP allows repeated headers (Set-Cookie)
/// and because preserving the order the server sent them makes captured
/// responses easier to compare.
class HttpHeaders {
 public:
    HttpHeaders() = default;

    HttpHeaders(std::initializer_list<std::pair<std::string, std::string>> items);

    /// Appends; does not replace an existing header of the same name.
    void add(std::string name, std::string value);

    /// Replaces every header of this name, or appends if there is none.
    void set(std::string name, std::string value);

    void remove(std::string_view name);

    [[nodiscard]] std::optional<std::string> get(std::string_view name) const;

    [[nodiscard]] std::string getOr(std::string_view name, std::string_view fallback) const;

    [[nodiscard]] bool contains(std::string_view name) const;

    [[nodiscard]] std::vector<std::string> getAll(std::string_view name) const;

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& items() const noexcept {
        return items_;
    }

    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    /// Serialised form, with values that look like credentials redacted.
    /// Used for logging and reports — never for the wire.
    [[nodiscard]] json::Value toRedactedJson() const;

 private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    HttpHeaders headers;
    std::string body;

    /// 0 uses the client's default.
    Milliseconds timeout{0};

    /// Convenience: sets body and Content-Type: application/json.
    static HttpRequest jsonPost(std::string url, const json::Value& payload);

    static HttpRequest get(std::string url);
};

/// A response, or the reason there isn't one.
///
/// A transport error (connection refused, DNS failure, timeout) is represented
/// on the same object rather than by an exception, because the distinction
/// between "the server said 500" and "there was no server" is something tests
/// routinely need to assert on, not something they want to catch.
struct HttpResponse {
    int statusCode = 0;
    std::string statusText;
    HttpHeaders headers;
    std::string body;
    Milliseconds elapsed{0};

    bool transportError = false;
    std::string errorMessage;

    /// Echo of what was requested, so an assertion failure can name it.
    std::string requestMethod;
    std::string requestUrl;

    /// True for 2xx.
    [[nodiscard]] bool ok() const noexcept { return statusCode >= 200 && statusCode < 300; }

    [[nodiscard]] bool isClientError() const noexcept {
        return statusCode >= 400 && statusCode < 500;
    }

    [[nodiscard]] bool isServerError() const noexcept { return statusCode >= 500; }

    /// "GET http://127.0.0.1:8000/health" — used in assertion messages.
    [[nodiscard]] std::string describeTarget() const;

    /// Parses the body as JSON. Returns nullopt when the body is not valid
    /// JSON; the caller decides whether that is a failure.
    [[nodiscard]] std::optional<json::Value> json() const;

    /// Parses the body as JSON, throwing AssertionFailure-friendly context on
    /// error. Returns a null value when the body is empty.
    [[nodiscard]] json::Value jsonOrThrow() const;

    [[nodiscard]] std::string contentType() const;

    /// Compact record of the exchange, attached to TestResult::metadata.
    [[nodiscard]] json::Value toMetadataJson() const;
};

/// Renders "200 OK", "404 Not Found", ... for a numeric status.
std::string_view reasonPhrase(int statusCode);

}  // namespace testforge::net
