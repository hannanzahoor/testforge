#include "testforge/testing/ApiTestCase.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/net/Url.hpp"

#include <utility>

namespace testforge {
namespace {

net::HttpClientOptions optionsFrom(const Config& config) {
    net::HttpClientOptions options;
    options.requestTimeout = Milliseconds{config.api.timeoutMs};
    options.connectTimeout = Milliseconds{std::min<std::int64_t>(config.api.timeoutMs, 5'000)};
    options.userAgent = "TestForge/0.9 (api-test)";
    for (const json::Member& header : config.api.defaultHeaders) {
        if (header.second.isString()) {
            options.defaultHeaders.set(header.first, header.second.asString());
        }
    }
    // Pin the client to the configured host. A test that wants a different
    // host has to say so in configuration, not in a path string.
    options.policy = net::UrlPolicy::restrictedTo(config.api.baseUrl);
    return options;
}

}  // namespace

ApiClient::ApiClient(TestContext& context)
    : context_(&context),
      client_(std::make_unique<net::HttpClient>(optionsFrom(context.config()))),
      baseUrl_(context.config().api.baseUrl),
      slaMs_(context.config().api.slaMs) {}

ApiClient::~ApiClient() = default;

ApiClient::ApiClient(ApiClient&&) noexcept = default;

ApiClient& ApiClient::operator=(ApiClient&&) noexcept = default;

std::string ApiClient::resolve(std::string_view path) const {
    if (strings::startsWith(path, "http://") || strings::startsWith(path, "https://")) {
        return std::string(path);
    }
    const std::optional<std::string> joined = net::joinUrl(baseUrl_, path);
    if (!joined.has_value()) {
        throw ConfigurationError("cannot resolve '" + std::string(path) + "' against base URL '" +
                                 baseUrl_ + "'");
    }
    return *joined;
}

void ApiClient::record(const net::HttpResponse& response) {
    ++exchangeCount_;

    // Two views of the same thing, because two consumers need different
    // shapes: FailureClassifier and the reporters read flat, stable keys
    // describing the *latest* exchange, while a human reading a multi-step
    // test needs the whole sequence.
    const json::Value metadata = response.toMetadataJson();
    for (const json::Member& member : metadata.asObject()) {
        context_->addMetadata(member.first, member.second);
    }

    exchanges_.push(json::Value::object()
                        .set("n", exchangeCount_)
                        .set("method", response.requestMethod)
                        .set("url", response.requestUrl)
                        .set("status", response.statusCode)
                        .set("ms", millisOf(response.elapsed)));
    if (exchangeCount_ > 1) {
        context_->addMetadata("exchanges", exchanges_);
    }

    if (response.transportError) {
        context_->log().warn("request failed at the transport layer",
                             {{"url", response.requestUrl}, {"error", response.errorMessage}});
    } else {
        context_->log().debug("http exchange",
                              {{"method", response.requestMethod},
                               {"url", response.requestUrl},
                               {"status", response.statusCode},
                               {"ms", millisOf(response.elapsed)}});
    }
}

net::HttpResponse ApiClient::send(net::HttpRequest request) {
    context_->throwIfCancelled();

    // Never let one request outlive the test's remaining budget.
    const Milliseconds remaining = context_->remaining();
    if (remaining != Milliseconds::max() && remaining.count() > 0 &&
        (request.timeout.count() == 0 || request.timeout > remaining)) {
        request.timeout = remaining;
    }

    net::HttpResponse response = client_->send(request, context_->cancellation());
    record(response);
    return response;
}

net::HttpResponse ApiClient::get(std::string_view path, const net::HttpHeaders& headers) {
    net::HttpRequest request = net::HttpRequest::get(resolve(path));
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

net::HttpResponse ApiClient::postRaw(std::string_view path,
                                     const std::string& body,
                                     const std::string& contentType,
                                     const net::HttpHeaders& headers) {
    net::HttpRequest request;
    request.method = "POST";
    request.url = resolve(path);
    request.body = body;
    request.headers.set("Content-Type", contentType);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

net::HttpResponse ApiClient::post(std::string_view path,
                                  const json::Value& body,
                                  const net::HttpHeaders& headers) {
    net::HttpRequest request = net::HttpRequest::jsonPost(resolve(path), body);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

net::HttpResponse ApiClient::put(std::string_view path,
                                 const json::Value& body,
                                 const net::HttpHeaders& headers) {
    net::HttpRequest request;
    request.method = "PUT";
    request.url = resolve(path);
    request.body = body.dump();
    request.headers.set("Content-Type", "application/json");
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

net::HttpResponse ApiClient::patch(std::string_view path,
                                   const json::Value& body,
                                   const net::HttpHeaders& headers) {
    net::HttpRequest request;
    request.method = "PATCH";
    request.url = resolve(path);
    request.body = body.dump();
    request.headers.set("Content-Type", "application/json");
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

net::HttpResponse ApiClient::del(std::string_view path, const net::HttpHeaders& headers) {
    net::HttpRequest request;
    request.method = "DELETE";
    request.url = resolve(path);
    for (const auto& [name, value] : headers.items()) {
        request.headers.set(name, value);
    }
    return send(std::move(request));
}

void ApiClient::requireReachable(std::string_view healthPath) {
    net::HttpRequest request = net::HttpRequest::get(resolve(healthPath));
    request.timeout = Milliseconds{std::min<std::int64_t>(3'000, context_->config().api.timeoutMs)};

    const net::HttpResponse response = client_->send(request, context_->cancellation());
    if (response.transportError) {
        // DependencyError, not an assertion failure: the service being absent
        // is not the system under test behaving incorrectly.
        throw DependencyError("the system under test is not reachable at " + baseUrl_ + " (" +
                              response.errorMessage +
                              "). Start it with: uvicorn app.main:app --port 8000 "
                              "--app-dir sample-service");
    }
    context_->log().debug("service reachable",
                          {{"url", response.requestUrl}, {"status", response.statusCode}});
}

void ApiClient::skipUnlessReachable(std::string_view healthPath) {
    net::HttpRequest request = net::HttpRequest::get(resolve(healthPath));
    request.timeout = Milliseconds{2'000};
    const net::HttpResponse response = client_->send(request, context_->cancellation());
    if (response.transportError) {
        context_->skip("the system under test is not reachable at " + baseUrl_);
    }
}

// ---------------------------------------------------------------------------
// ApiTestCase
// ---------------------------------------------------------------------------

ApiTestCase::ApiTestCase(TestMetadata metadata) : metadata_(std::move(metadata)) {}

void ApiTestCase::setUp(TestContext& context) {
    api_ = std::make_unique<ApiClient>(context);
    if (requiresService()) {
        api_->requireReachable(healthPath());
    }
}

void ApiTestCase::tearDown(TestContext& /*context*/) noexcept {
    // Releasing the client here rather than in the destructor keeps the socket
    // lifetime tied to the test, not to whenever the object is collected.
    api_.reset();
}

ApiClient& ApiTestCase::api() {
    if (!api_) {
        throw FrameworkError("ApiTestCase::api() called outside setUp/execute/tearDown");
    }
    return *api_;
}

}  // namespace testforge
