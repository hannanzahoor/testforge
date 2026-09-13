#pragma once

#include "testforge/ai/TestSpec.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/net/Url.hpp"

#include <string>
#include <vector>

namespace testforge::ai {

/// One problem found in a specification.
struct ValidationIssue {
    std::string where;  ///< "tests[3].endpoint"
    std::string message;
    bool fatal = true;  ///< fatal issues drop the test; others are warnings

    [[nodiscard]] json::Value toJson() const;
};

struct ValidationReport {
    bool valid = false;  ///< true when at least one test survived
    int accepted = 0;
    int rejected = 0;
    std::vector<ValidationIssue> issues;

    [[nodiscard]] std::string summary() const;

    [[nodiscard]] json::Value toJson() const;
};

/// The gate between model output and execution.
///
/// TestForge treats everything a language model produces as hostile input,
/// because from a security standpoint that is exactly what it is: text from an
/// external service, shaped by a prompt that may itself contain text from a
/// system under test. The validator is the single place that decides what is
/// allowed to run.
///
/// What it enforces:
///   * the HTTP method is one of a fixed allow-list;
///   * the endpoint is a *relative path* — no scheme, no host, no "..", no
///     control characters — so a generated test cannot pick its own target;
///   * the resolved URL passes the configured UrlPolicy;
///   * status codes are in 100-599, latency budgets are sane;
///   * no request header that carries credentials or rewrites the destination
///     (Authorization, Cookie, Host, ...);
///   * names are safe identifiers and unique within the suite;
///   * the suite is within the configured size limits.
///
/// Anything that fails is dropped with a recorded reason. A spec that violates
/// the rules is never "fixed up" and run anyway.
class SpecValidator {
 public:
    SpecValidator(AiConfig config, net::UrlPolicy policy, std::string baseUrl);

    /// Validates in place: rejected tests are removed from `suite`.
    ValidationReport validate(TestSpecSuite& suite) const;

    /// Validates one test without mutating anything.
    [[nodiscard]] std::vector<ValidationIssue> validateOne(const TestSpec& spec,
                                                           std::size_t index) const;

    /// The HTTP methods a generated test may use.
    [[nodiscard]] static const std::vector<std::string>& allowedMethods();

    /// Headers a generated test may never set.
    [[nodiscard]] static const std::vector<std::string>& forbiddenHeaders();

    /// True when `endpoint` is an acceptable relative path.
    [[nodiscard]] static bool isSafeEndpoint(std::string_view endpoint, std::string* reason);

    /// True when `name` is safe to use as a test name (and as part of a
    /// filename, a SQL value and an HTML fragment).
    [[nodiscard]] static bool isSafeName(std::string_view name, std::string* reason);

 private:
    AiConfig config_;
    net::UrlPolicy policy_;
    std::string baseUrl_;
};

}  // namespace testforge::ai
