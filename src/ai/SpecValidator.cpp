#include "testforge/ai/SpecValidator.hpp"

#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace testforge::ai {
namespace {

constexpr std::size_t kMaxNameLength = 80;
constexpr std::size_t kMaxEndpointLength = 512;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxHeaderCount = 16;
constexpr std::size_t kMaxHeaderValueLength = 1024;
constexpr std::size_t kMaxAssertionsPerTest = 20;

bool hasControlCharacter(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

}  // namespace

json::Value ValidationIssue::toJson() const {
    json::Value out = json::Value::object();
    out.set("where", where);
    out.set("message", message);
    out.set("fatal", fatal);
    return out;
}

std::string ValidationReport::summary() const {
    std::ostringstream os;
    os << accepted << " test(s) accepted, " << rejected << " rejected";
    if (!issues.empty()) {
        os << ", " << issues.size() << " issue(s) recorded";
    }
    return os.str();
}

json::Value ValidationReport::toJson() const {
    json::Value out = json::Value::object();
    out.set("valid", valid);
    out.set("accepted", accepted);
    out.set("rejected", rejected);
    out.set("summary", summary());
    json::Value list = json::Value::array();
    for (const ValidationIssue& issue : issues) {
        list.push(issue.toJson());
    }
    out.set("issues", list);
    return out;
}

SpecValidator::SpecValidator(AiConfig config, net::UrlPolicy policy, std::string baseUrl)
    : config_(std::move(config)), policy_(std::move(policy)), baseUrl_(std::move(baseUrl)) {}

const std::vector<std::string>& SpecValidator::allowedMethods() {
    // No TRACE (cross-site tracing), no CONNECT (tunnelling). A generated test
    // has no legitimate need for either.
    static const std::vector<std::string> kMethods = {
        "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
    return kMethods;
}

const std::vector<std::string>& SpecValidator::forbiddenHeaders() {
    // Credentials must come from configuration, not from a model. Host and the
    // forwarding headers are excluded because they change where the request
    // actually goes or how the server thinks it was reached.
    static const std::vector<std::string> kHeaders = {"authorization",
                                                      "cookie",
                                                      "set-cookie",
                                                      "proxy-authorization",
                                                      "host",
                                                      "x-forwarded-for",
                                                      "x-forwarded-host",
                                                      "x-real-ip",
                                                      "content-length",
                                                      "transfer-encoding",
                                                      "connection",
                                                      "upgrade"};
    return kHeaders;
}

bool SpecValidator::isSafeName(std::string_view name, std::string* reason) {
    const auto reject = [reason](const std::string& message) {
        if (reason != nullptr) {
            *reason = message;
        }
        return false;
    };

    if (name.empty()) {
        return reject("name is empty");
    }
    if (name.size() > kMaxNameLength) {
        return reject("name exceeds " + std::to_string(kMaxNameLength) + " characters");
    }
    for (const char c : name) {
        const bool acceptable =
            std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
        if (!acceptable) {
            return reject(std::string("name contains an unsupported character '") + c +
                          "' (allowed: letters, digits, '_', '-', '.')");
        }
    }
    // A leading dot would make the qualified name ambiguous, and '..' is the
    // traversal token this project rejects everywhere on principle.
    if (name.front() == '.' || strings::contains(name, "..")) {
        return reject("name must not start with '.' or contain '..'");
    }
    return true;
}

bool SpecValidator::isSafeEndpoint(std::string_view endpoint, std::string* reason) {
    const auto reject = [reason](const std::string& message) {
        if (reason != nullptr) {
            *reason = message;
        }
        return false;
    };

    if (endpoint.empty()) {
        return reject("endpoint is empty");
    }
    if (endpoint.size() > kMaxEndpointLength) {
        return reject("endpoint exceeds " + std::to_string(kMaxEndpointLength) + " characters");
    }
    if (hasControlCharacter(endpoint)) {
        // CR or LF here would be HTTP request splitting.
        return reject("endpoint contains a control character");
    }
    if (endpoint.front() != '/') {
        return reject(
            "endpoint must be a path beginning with '/' — absolute URLs are not allowed, so a "
            "generated test cannot choose its own target host");
    }
    if (strings::startsWith(endpoint, "//")) {
        return reject("endpoint must not start with '//' (that is a protocol-relative URL)");
    }
    if (strings::contains(endpoint, "..")) {
        return reject("endpoint must not contain '..'");
    }
    const std::string lowered = strings::toLower(endpoint);
    for (const std::string_view scheme :
         {"http:", "https:", "file:", "ftp:", "gopher:", "data:", "javascript:"}) {
        if (strings::contains(lowered, scheme)) {
            return reject("endpoint must not contain a URL scheme");
        }
    }
    if (strings::contains(endpoint, "@")) {
        // "/@evil.com/" style tricks against sloppy URL joiners.
        return reject("endpoint must not contain '@'");
    }
    return true;
}

std::vector<ValidationIssue> SpecValidator::validateOne(const TestSpec& spec,
                                                        std::size_t index) const {
    std::vector<ValidationIssue> issues;
    const std::string prefix = "tests[" + std::to_string(index) + "]";

    const auto add = [&issues](std::string where, std::string message, bool fatal = true) {
        issues.push_back({std::move(where), std::move(message), fatal});
    };

    std::string reason;
    if (!isSafeName(spec.name, &reason)) {
        add(prefix + ".name", reason);
    }

    const std::vector<std::string>& methods = allowedMethods();
    if (std::find(methods.begin(), methods.end(), spec.method) == methods.end()) {
        add(prefix + ".method",
            "method '" + spec.method +
                "' is not allowed (allowed: " + strings::join(methods, ", ") + ")");
    }

    if (!isSafeEndpoint(spec.endpoint, &reason)) {
        add(prefix + ".endpoint", reason);
    } else {
        // Resolve against the base URL and re-check under the URL policy. This
        // catches a base URL that the operator has since restricted.
        const std::optional<std::string> resolved = net::joinUrl(baseUrl_, spec.endpoint);
        if (!resolved.has_value()) {
            add(prefix + ".endpoint",
                "endpoint could not be resolved against the configured base URL '" + baseUrl_ +
                    "'");
        } else if (const std::string rejection = policy_.reject(*resolved); !rejection.empty()) {
            add(prefix + ".endpoint", "resolved URL is not permitted: " + rejection);
        }
    }

    if (spec.expectedStatus < 100 || spec.expectedStatus > 599) {
        add(prefix + ".expected_status",
            "expected status " + std::to_string(spec.expectedStatus) + " is outside 100-599");
    }
    for (const int status : spec.acceptableStatuses) {
        if (status < 100 || status > 599) {
            add(prefix + ".acceptable_statuses",
                "status " + std::to_string(status) + " is outside 100-599");
        }
    }

    if (spec.maxResponseTimeMs < 0 || spec.maxResponseTimeMs > 600'000) {
        add(prefix + ".max_response_time_ms", "latency budget must be between 0 and 600000 ms");
    }

    if (spec.headers.size() > kMaxHeaderCount) {
        add(prefix + ".headers", "more than " + std::to_string(kMaxHeaderCount) + " headers");
    }
    for (const json::Member& header : spec.headers) {
        const std::string lowered = strings::toLower(header.first);
        const std::vector<std::string>& forbidden = forbiddenHeaders();
        if (std::find(forbidden.begin(), forbidden.end(), lowered) != forbidden.end()) {
            add(prefix + ".headers",
                "header '" + header.first +
                    "' may not be set by a generated test; credentials and routing headers come "
                    "from configuration");
        }
        if (hasControlCharacter(header.first)) {
            add(prefix + ".headers", "header name contains a control character");
        }
        if (header.second.isString()) {
            const std::string& value = header.second.asString();
            if (hasControlCharacter(value)) {
                add(prefix + ".headers",
                    "value of header '" + header.first + "' contains a control character");
            }
            if (value.size() > kMaxHeaderValueLength) {
                add(prefix + ".headers", "value of header '" + header.first + "' is too long");
            }
        }
    }

    if (!spec.body.isNull()) {
        const std::string encoded = spec.body.dump();
        if (encoded.size() > kMaxBodyBytes) {
            add(prefix + ".body",
                "request body exceeds " + std::to_string(kMaxBodyBytes) + " bytes");
        }
        if (spec.method == "GET" || spec.method == "HEAD") {
            // Not dangerous, just wrong: flag it without dropping the test.
            add(prefix + ".body", "a " + spec.method + " request should not carry a body", false);
        }
    }

    if (spec.assertions.size() > kMaxAssertionsPerTest) {
        add(prefix + ".assertions",
            "more than " + std::to_string(kMaxAssertionsPerTest) + " assertions");
    }
    for (std::size_t i = 0; i < spec.assertions.size(); ++i) {
        const SpecAssertion& assertion = spec.assertions[i];
        const std::string where = prefix + ".assertions[" + std::to_string(i) + "]";
        if (hasControlCharacter(assertion.target)) {
            add(where, "assertion target contains a control character");
        }
        if (assertion.target.size() > 200) {
            add(where, "assertion target is too long");
        }
        if (assertion.kind == SpecAssertion::Kind::ResponseTimeUnder &&
            (!assertion.expected.isNumber() || assertion.expected.intOr(0) <= 0)) {
            add(where, "response_time_under_ms requires a positive number");
        }
        if (assertion.kind == SpecAssertion::Kind::StatusCodeIn && !assertion.expected.isArray()) {
            add(where, "status_code_in requires an array of status codes");
        }
    }

    return issues;
}

ValidationReport SpecValidator::validate(TestSpecSuite& suite) const {
    ValidationReport report;

    std::string reason;
    if (!isSafeName(suite.suite, &reason)) {
        report.issues.push_back({"suite", "suite name is not usable: " + reason, true});
        report.valid = false;
        report.rejected = static_cast<int>(suite.tests.size());
        suite.tests.clear();
        return report;
    }

    const auto maxTests = static_cast<std::size_t>(std::max(1, config_.maxGeneratedTests));
    if (suite.tests.size() > maxTests) {
        report.issues.push_back({"tests",
                                 "the specification contains " +
                                     std::to_string(suite.tests.size()) +
                                     " tests; keeping the first " + std::to_string(maxTests) +
                                     " (ai.max_generated_tests)",
                                 false});
        report.rejected += static_cast<int>(suite.tests.size() - maxTests);
        suite.tests.resize(maxTests);
    }

    std::vector<TestSpec> accepted;
    accepted.reserve(suite.tests.size());
    std::set<std::string> seenNames;

    for (std::size_t i = 0; i < suite.tests.size(); ++i) {
        TestSpec& spec = suite.tests[i];
        std::vector<ValidationIssue> issues = validateOne(spec, i);

        if (!seenNames.insert(strings::toLower(spec.name)).second) {
            // Duplicates would collide in the registry, where the second
            // registration silently replaces the first.
            issues.push_back({"tests[" + std::to_string(i) + "].name",
                              "duplicate test name '" + spec.name + "' within the suite",
                              true});
        }

        const bool fatal = std::any_of(
            issues.begin(), issues.end(), [](const ValidationIssue& issue) { return issue.fatal; });

        report.issues.insert(report.issues.end(), issues.begin(), issues.end());
        if (fatal) {
            ++report.rejected;
        } else {
            accepted.push_back(std::move(spec));
            ++report.accepted;
        }
    }

    suite.tests = std::move(accepted);
    report.valid = !suite.tests.empty();
    if (!report.valid && report.issues.empty()) {
        report.issues.push_back({"tests", "the specification produced no usable tests", true});
    }
    return report;
}

}  // namespace testforge::ai
