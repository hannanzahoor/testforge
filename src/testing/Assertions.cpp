#include "testforge/testing/Assertions.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"

#include <sstream>
#include <vector>

namespace testforge {
namespace {

/// Stack of soft-assertion scopes for the current thread. Same reasoning as
/// ScopedLogCapture: a test runs on one thread, so thread-local state gives
/// correct per-test behaviour with no locking.
std::vector<SoftAssertionScope*>& softScopes() {
    static thread_local std::vector<SoftAssertionScope*> scopes;
    return scopes;
}

std::string buildMessage(const std::string& message, const std::string& expression) {
    if (!message.empty()) {
        return message;
    }
    if (!expression.empty()) {
        return "assertion failed: " + expression;
    }
    return "assertion failed";
}

}  // namespace

std::atomic<std::uint64_t> AssertionEngine::checkCount_{0};

void AssertionEngine::report(bool succeeded,
                             std::string expression,
                             std::string expected,
                             std::string actual,
                             std::string message,
                             SourceLocation where) {
    checkCount_.fetch_add(1, std::memory_order_relaxed);
    if (succeeded) {
        return;
    }

    AssertionFailure failure(buildMessage(message, expression),
                             std::move(expression),
                             std::move(expected),
                             std::move(actual),
                             where);

    std::vector<SoftAssertionScope*>& scopes = softScopes();
    if (!scopes.empty()) {
        scopes.back()->record(failure);
        return;
    }
    throw failure;
}

std::uint64_t AssertionEngine::checksPerformed() noexcept {
    return checkCount_.load(std::memory_order_relaxed);
}

void AssertionEngine::resetCounters() noexcept {
    checkCount_.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// SoftAssertionScope
// ---------------------------------------------------------------------------

SoftAssertionScope::SoftAssertionScope() {
    softScopes().push_back(this);
}

SoftAssertionScope::~SoftAssertionScope() {
    std::vector<SoftAssertionScope*>& scopes = softScopes();
    if (!scopes.empty() && scopes.back() == this) {
        scopes.pop_back();
    }
    // Deliberately does NOT throw from the destructor. A test that forgets to
    // call verify() gets the failures reported through the log instead of the
    // process terminating via std::terminate during stack unwinding.
    if (!verified_ && !failures_.empty()) {
        Logger("assertions")
            .error("soft assertion scope destroyed without verify()",
                   {{"failures", static_cast<std::int64_t>(failures_.size())}});
    }
}

void SoftAssertionScope::record(const AssertionFailure& failure) {
    if (failures_.empty()) {
        firstLocation_ = failure.where();
    }
    failures_.push_back(failure.detail());
}

void SoftAssertionScope::verify() {
    if (verified_ || failures_.empty()) {
        verified_ = true;
        return;
    }
    verified_ = true;

    std::ostringstream os;
    os << failures_.size() << " assertion(s) failed";
    std::ostringstream detail;
    for (std::size_t i = 0; i < failures_.size(); ++i) {
        detail << "\n[" << (i + 1) << "] " << failures_[i];
    }

    // Popped before throwing so the failure is not recaptured by this scope.
    std::vector<SoftAssertionScope*>& scopes = softScopes();
    if (!scopes.empty() && scopes.back() == this) {
        scopes.pop_back();
    }

    throw AssertionFailure(os.str() + detail.str(),
                           "soft assertions",
                           "all checks pass",
                           std::to_string(failures_.size()) + " failed",
                           firstLocation_);
}

// ---------------------------------------------------------------------------
// assertions
// ---------------------------------------------------------------------------

namespace assertions {

void isTrue(bool value,
            std::string_view expression,
            SourceLocation where,
            std::string_view message) {
    AssertionEngine::report(value,
                            std::string(expression),
                            "true",
                            value ? "true" : "false",
                            std::string(message),
                            where);
}

void isFalse(bool value,
             std::string_view expression,
             SourceLocation where,
             std::string_view message) {
    AssertionEngine::report(!value,
                            std::string(expression),
                            "false",
                            value ? "true" : "false",
                            std::string(message),
                            where);
}

void containsSubstring(std::string_view haystack,
                       std::string_view needle,
                       std::string_view expression,
                       SourceLocation where,
                       std::string_view message) {
    const bool ok = haystack.find(needle) != std::string_view::npos;
    // The haystack can be a whole HTTP body; truncate it so the failure stays
    // readable while still showing enough to recognise what came back.
    AssertionEngine::report(ok,
                            std::string(expression),
                            "text containing " + detail::display(needle),
                            detail::display(strings::truncate(haystack, 400)),
                            std::string(message),
                            where);
}

void doesNotContainSubstring(std::string_view haystack,
                             std::string_view needle,
                             std::string_view expression,
                             SourceLocation where,
                             std::string_view message) {
    const bool ok = haystack.find(needle) == std::string_view::npos;
    AssertionEngine::report(ok,
                            std::string(expression),
                            "text NOT containing " + detail::display(needle),
                            detail::display(strings::truncate(haystack, 400)),
                            std::string(message),
                            where);
}

void matchesGlob(std::string_view text,
                 std::string_view pattern,
                 std::string_view expression,
                 SourceLocation where,
                 std::string_view message) {
    AssertionEngine::report(strings::globMatch(pattern, text),
                            std::string(expression),
                            "text matching " + detail::display(pattern),
                            detail::display(strings::truncate(text, 400)),
                            std::string(message),
                            where);
}

void isNear(double actual,
            double expected,
            double tolerance,
            std::string_view expression,
            SourceLocation where,
            std::string_view message) {
    const double difference = actual > expected ? actual - expected : expected - actual;
    std::ostringstream expectedText;
    expectedText << detail::display(expected) << " +/- " << detail::display(tolerance);
    std::ostringstream actualText;
    actualText << detail::display(actual) << " (difference " << detail::display(difference) << ")";
    AssertionEngine::report(difference <= tolerance,
                            std::string(expression),
                            expectedText.str(),
                            actualText.str(),
                            std::string(message),
                            where);
}

void statusCode(int actual,
                int expected,
                std::string_view endpoint,
                SourceLocation where,
                std::string_view message) {
    std::string note(message);
    if (note.empty() && !endpoint.empty()) {
        note = "unexpected HTTP status for " + std::string(endpoint);
    }
    AssertionEngine::report(actual == expected,
                            "response.statusCode == " + std::to_string(expected),
                            std::to_string(expected),
                            std::to_string(actual),
                            note,
                            where);
}

void statusCodeIn(int actual,
                  const std::vector<int>& allowed,
                  std::string_view endpoint,
                  SourceLocation where,
                  std::string_view message) {
    bool ok = false;
    std::ostringstream expected;
    expected << "one of [";
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        if (i > 0) {
            expected << ", ";
        }
        expected << allowed[i];
        if (allowed[i] == actual) {
            ok = true;
        }
    }
    expected << ']';

    std::string note(message);
    if (note.empty() && !endpoint.empty()) {
        note = "unexpected HTTP status for " + std::string(endpoint);
    }
    AssertionEngine::report(ok,
                            "response.statusCode in allowed set",
                            expected.str(),
                            std::to_string(actual),
                            note,
                            where);
}

void responseTime(Milliseconds actual,
                  Milliseconds budget,
                  std::string_view endpoint,
                  SourceLocation where,
                  std::string_view message) {
    const bool ok = actual <= budget;
    std::string note(message);
    if (note.empty() && !endpoint.empty()) {
        note = "response time budget exceeded for " + std::string(endpoint);
    }
    std::ostringstream actualText;
    actualText << formatDuration(actual);
    if (!ok) {
        actualText << " (over budget by " << formatDuration(actual - budget) << ")";
    }
    AssertionEngine::report(ok,
                            "response.elapsed <= budget",
                            "<= " + formatDuration(budget),
                            actualText.str(),
                            note,
                            where);
}

void jsonHasField(const json::Value& document,
                  std::string_view dottedPath,
                  SourceLocation where,
                  std::string_view message) {
    const json::Value* found = document.path(dottedPath);
    AssertionEngine::report(found != nullptr,
                            "json has field " + std::string(dottedPath),
                            "field '" + std::string(dottedPath) + "' present",
                            found != nullptr
                                ? "present"
                                : "absent; document = " + strings::truncate(document.dump(), 400),
                            std::string(message),
                            where);
}

void jsonFieldEquals(const json::Value& document,
                     std::string_view dottedPath,
                     const json::Value& expected,
                     SourceLocation where,
                     std::string_view message) {
    const json::Value* found = document.path(dottedPath);
    if (found == nullptr) {
        AssertionEngine::report(
            false,
            "json." + std::string(dottedPath) + " == expected",
            expected.dump(),
            "field absent; document = " + strings::truncate(document.dump(), 400),
            std::string(message),
            where);
        return;
    }
    AssertionEngine::report(*found == expected,
                            "json." + std::string(dottedPath) + " == expected",
                            expected.dump(),
                            found->dump(),
                            std::string(message),
                            where);
}

void fail(std::string_view message, SourceLocation where) {
    AssertionEngine::report(
        false, "TF_FAIL", "no failure", "explicit failure", std::string(message), where);
    // AssertionEngine::report throws unless a soft-assertion scope is active;
    // in that case the test author still asked to fail here, so unwind.
    throw AssertionFailure(
        std::string(message), "TF_FAIL", "no failure", "explicit failure", where);
}

}  // namespace assertions
}  // namespace testforge
