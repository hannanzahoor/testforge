#include "testforge/net/HttpTypes.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <utility>

namespace testforge::net {

HttpHeaders::HttpHeaders(std::initializer_list<std::pair<std::string, std::string>> items)
    : items_(items) {}

void HttpHeaders::add(std::string name, std::string value) {
    items_.emplace_back(std::move(name), std::move(value));
}

void HttpHeaders::set(std::string name, std::string value) {
    remove(name);
    items_.emplace_back(std::move(name), std::move(value));
}

void HttpHeaders::remove(std::string_view name) {
    items_.erase(std::remove_if(items_.begin(),
                                items_.end(),
                                [name](const std::pair<std::string, std::string>& item) {
                                    return strings::equalsIgnoreCase(item.first, name);
                                }),
                 items_.end());
}

std::optional<std::string> HttpHeaders::get(std::string_view name) const {
    for (const auto& [key, value] : items_) {
        if (strings::equalsIgnoreCase(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

std::string HttpHeaders::getOr(std::string_view name, std::string_view fallback) const {
    return get(name).value_or(std::string(fallback));
}

bool HttpHeaders::contains(std::string_view name) const {
    return get(name).has_value();
}

std::vector<std::string> HttpHeaders::getAll(std::string_view name) const {
    std::vector<std::string> values;
    for (const auto& [key, value] : items_) {
        if (strings::equalsIgnoreCase(key, name)) {
            values.push_back(value);
        }
    }
    return values;
}

json::Value HttpHeaders::toRedactedJson() const {
    json::Value out = json::Value::object();
    for (const auto& [key, value] : items_) {
        out.set(key,
                strings::isSensitiveName(key) ? std::string("***REDACTED***")
                                              : strings::redactSecrets(value));
    }
    return out;
}

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

HttpRequest HttpRequest::jsonPost(std::string url, const json::Value& payload) {
    HttpRequest request;
    request.method = "POST";
    request.url = std::move(url);
    request.body = payload.dump();
    request.headers.set("Content-Type", "application/json");
    request.headers.set("Accept", "application/json");
    return request;
}

HttpRequest HttpRequest::get(std::string url) {
    HttpRequest request;
    request.method = "GET";
    request.url = std::move(url);
    request.headers.set("Accept", "application/json");
    return request;
}

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

std::string HttpResponse::describeTarget() const {
    if (requestMethod.empty() && requestUrl.empty()) {
        return "<unknown request>";
    }
    return requestMethod + " " + requestUrl;
}

std::optional<json::Value> HttpResponse::json() const {
    if (body.empty()) {
        return std::nullopt;
    }
    return json::tryParse(body);
}

json::Value HttpResponse::jsonOrThrow() const {
    if (body.empty()) {
        return json::Value{};
    }
    std::string error;
    std::optional<json::Value> parsed = json::tryParse(body, &error);
    if (!parsed.has_value()) {
        throw AssertionFailure(
            "response body is not valid JSON for " + describeTarget() + ": " + error,
            "response.json()",
            "valid JSON",
            strings::truncate(body, 400),
            SourceLocation{});
    }
    return *parsed;
}

std::string HttpResponse::contentType() const {
    std::string value = headers.getOr("Content-Type", "");
    if (const std::size_t semicolon = value.find(';'); semicolon != std::string::npos) {
        value = value.substr(0, semicolon);
    }
    return strings::trim(value);
}

json::Value HttpResponse::toMetadataJson() const {
    json::Value out = json::Value::object();
    out.set("http_method", requestMethod);
    out.set("http_url", requestUrl);
    out.set("http_status", statusCode);
    out.set("http_status_text", statusText);
    out.set("response_time_ms", millisOf(elapsed));
    out.set("content_type", contentType());
    out.set("response_bytes", static_cast<std::int64_t>(body.size()));
    if (transportError) {
        out.set("transport_error", errorMessage);
    }
    // The body is the single most useful thing when triaging, but it can be
    // large; keep a bounded, redacted excerpt.
    out.set("response_excerpt", strings::truncate(strings::redactSecrets(body), 2000));
    out.set("response_headers", headers.toRedactedJson());
    return out;
}

std::string_view reasonPhrase(int statusCode) {
    switch (statusCode) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 415:
            return "Unsupported Media Type";
        case 422:
            return "Unprocessable Entity";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            break;
    }
    if (statusCode >= 200 && statusCode < 300) {
        return "Success";
    }
    if (statusCode >= 400 && statusCode < 500) {
        return "Client Error";
    }
    if (statusCode >= 500) {
        return "Server Error";
    }
    return "Unknown";
}

}  // namespace testforge::net
