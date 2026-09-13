#pragma once

#include "testforge/core/TestCase.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/net/HttpClient.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace testforge {

/// HTTP helper bound to one TestContext.
///
/// Three things it does that a bare HttpClient does not:
///   * resolves paths against `api.base_url`, so tests say "/health";
///   * passes the test's cancellation token into every call, so a timed-out
///     test stops waiting on a socket instead of blocking its worker;
///   * records the exchange onto the TestResult, which is what makes the
///     failure report show the request, the status and the body without the
///     test author writing any logging.
///
/// Construct one per test. It is not thread-safe and is not meant to be.
class ApiClient {
 public:
    explicit ApiClient(TestContext& context);

    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;
    ApiClient(ApiClient&&) noexcept;
    ApiClient& operator=(ApiClient&&) noexcept;

    net::HttpResponse get(std::string_view path, const net::HttpHeaders& headers = {});

    net::HttpResponse post(std::string_view path,
                           const json::Value& body,
                           const net::HttpHeaders& headers = {});

    net::HttpResponse postRaw(std::string_view path,
                              const std::string& body,
                              const std::string& contentType,
                              const net::HttpHeaders& headers = {});

    net::HttpResponse put(std::string_view path,
                          const json::Value& body,
                          const net::HttpHeaders& headers = {});

    net::HttpResponse patch(std::string_view path,
                            const json::Value& body,
                            const net::HttpHeaders& headers = {});

    net::HttpResponse del(std::string_view path, const net::HttpHeaders& headers = {});

    net::HttpResponse send(net::HttpRequest request);

    /// Absolute URL for a path, resolved against the configured base URL.
    [[nodiscard]] std::string resolve(std::string_view path) const;

    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

    /// Default latency budget from configuration, for TF_ASSERT_RESPONSE_TIME.
    [[nodiscard]] std::int64_t slaMs() const noexcept { return slaMs_; }

    /// Throws DependencyError when the service is not reachable, with a
    /// message that names the URL. Call it in setUp to turn "20 confusing
    /// assertion failures" into one clear "the service is not running".
    void requireReachable(std::string_view healthPath = "/health");

    /// Like requireReachable, but skips the test instead of failing it. The
    /// right choice when the service is genuinely optional.
    void skipUnlessReachable(std::string_view healthPath = "/health");

    [[nodiscard]] net::HttpClient& client() noexcept { return *client_; }

 private:
    void record(const net::HttpResponse& response);

    TestContext* context_;
    std::unique_ptr<net::HttpClient> client_;
    std::string baseUrl_;
    std::int64_t slaMs_ = 0;
    int exchangeCount_ = 0;
    json::Value exchanges_ = json::Value::array();
};

/// Base class for API tests written as classes rather than functions.
///
/// Most tests in this repository use the TESTFORGE_TEST macro with a local
/// ApiClient — composition, and less ceremony. This class exists for suites
/// that share setup across many cases: subclass once, override execute() many
/// times.
class ApiTestCase : public TestCase {
 public:
    explicit ApiTestCase(TestMetadata metadata);

    void setUp(TestContext& context) override;

    void tearDown(TestContext& context) noexcept override;

    [[nodiscard]] const TestMetadata& metadata() const override { return metadata_; }

 protected:
    /// Valid only between setUp and tearDown.
    [[nodiscard]] ApiClient& api();

    /// Override to require the service before the body runs. Default is true,
    /// because an API test against a dead service produces noise.
    [[nodiscard]] virtual bool requiresService() const { return true; }

    /// Health endpoint used by the reachability check.
    [[nodiscard]] virtual std::string healthPath() const { return "/health"; }

 private:
    TestMetadata metadata_;
    std::unique_ptr<ApiClient> api_;
};

}  // namespace testforge
