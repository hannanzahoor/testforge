#pragma once

#include "testforge/core/Clock.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/SourceLocation.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace testforge {

namespace detail {

/// Detects whether `T` can be streamed, so the assertion machinery can print
/// a useful value for user types without requiring one.
template<typename T, typename = void>
struct IsStreamable : std::false_type {};

template<typename T>
struct IsStreamable<
    T,
    std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders a value for an assertion message.
///
/// Strings are quoted so that trailing whitespace and empty strings are
/// visible — "expected 'abc ' got 'abc'" is useless without the quotes.
template<typename T>
std::string display(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        std::string_view text(value);
        std::string out;
        out.reserve(text.size() + 2);
        out.push_back('"');
        out.append(text);
        out.push_back('"');
        return out;
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::nullptr_t>) {
        return "nullptr";
    } else if constexpr (std::is_pointer_v<std::decay_t<T>>) {
        if (value == nullptr) {
            return "nullptr";
        }
        std::ostringstream os;
        os << static_cast<const void*>(value);
        return os.str();
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
        std::ostringstream os;
        os << std::setprecision(10) << value;
        return os.str();
    } else if constexpr (IsStreamable<T>::value) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<value of type " + std::string(typeid(T).name()) + ">";
    }
}

/// Comparison that is correct across signed/unsigned boundaries.
///
/// `-1 < 1u` is false in plain C++; std::cmp_less gives the mathematically
/// correct answer, which is what a test author expects an assertion to use.
template<typename A, typename B>
bool equalValues(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> &&
                  !std::is_same_v<B, bool>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}

template<typename A, typename B>
bool lessValues(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> &&
                  !std::is_same_v<B, bool>) {
        return std::cmp_less(a, b);
    } else {
        return a < b;
    }
}

}  // namespace detail

/// Central point through which every assertion passes.
///
/// Having one choke point buys three things that scattered `if (!x) throw`
/// would not: a global count of checks performed (a real coverage signal),
/// uniform failure messages, and the soft-assertion mode below.
class AssertionEngine {
 public:
    /// Records the outcome of one check. On failure, either throws
    /// AssertionFailure (default) or records it in the active soft-assertion
    /// scope.
    static void report(bool succeeded,
                       std::string expression,
                       std::string expected,
                       std::string actual,
                       std::string message,
                       SourceLocation where);

    /// Number of assertions executed in this process. Exposed as a run
    /// statistic — "1 200 assertions across 60 tests" says more about a suite
    /// than the test count alone.
    static std::uint64_t checksPerformed() noexcept;

    static void resetCounters() noexcept;

 private:
    static std::atomic<std::uint64_t> checkCount_;
};

/// Collects assertion failures instead of throwing on the first one.
///
/// Useful for API tests, where status code, body shape and latency are three
/// independent facts and an engineer wants to see all three verdicts, not just
/// the first. Failures are raised as one combined AssertionFailure by
/// verify(), which the test must call (or which happens automatically at scope
/// exit if the scope is left normally).
class SoftAssertionScope {
 public:
    SoftAssertionScope();
    ~SoftAssertionScope();

    SoftAssertionScope(const SoftAssertionScope&) = delete;
    SoftAssertionScope& operator=(const SoftAssertionScope&) = delete;
    SoftAssertionScope(SoftAssertionScope&&) = delete;
    SoftAssertionScope& operator=(SoftAssertionScope&&) = delete;

    /// Throws a combined AssertionFailure when anything failed. Safe to call
    /// more than once; only the first call throws.
    void verify();

    [[nodiscard]] std::size_t failureCount() const noexcept { return failures_.size(); }

 private:
    friend class AssertionEngine;

    void record(const AssertionFailure& failure);

    std::vector<std::string> failures_;
    SourceLocation firstLocation_;
    bool verified_ = false;
};

// ---------------------------------------------------------------------------
// Assertion implementation helpers.
//
// These are functions rather than macro bodies so that the macros stay one
// line each and the logic is unit-testable on its own.
// ---------------------------------------------------------------------------

namespace assertions {

void isTrue(bool value,
            std::string_view expression,
            SourceLocation where,
            std::string_view message = {});

void isFalse(bool value,
             std::string_view expression,
             SourceLocation where,
             std::string_view message = {});

template<typename A, typename B>
void areEqual(const A& actual,
              const B& expected,
              std::string_view expression,
              SourceLocation where,
              std::string_view message = {}) {
    AssertionEngine::report(detail::equalValues(actual, expected),
                            std::string(expression),
                            detail::display(expected),
                            detail::display(actual),
                            std::string(message),
                            where);
}

template<typename A, typename B>
void areNotEqual(const A& actual,
                 const B& forbidden,
                 std::string_view expression,
                 SourceLocation where,
                 std::string_view message = {}) {
    AssertionEngine::report(!detail::equalValues(actual, forbidden),
                            std::string(expression),
                            "anything except " + detail::display(forbidden),
                            detail::display(actual),
                            std::string(message),
                            where);
}

template<typename A, typename B>
void isLess(const A& actual,
            const B& bound,
            std::string_view expression,
            SourceLocation where,
            std::string_view message = {}) {
    AssertionEngine::report(detail::lessValues(actual, bound),
                            std::string(expression),
                            "< " + detail::display(bound),
                            detail::display(actual),
                            std::string(message),
                            where);
}

template<typename A, typename B>
void isLessOrEqual(const A& actual,
                   const B& bound,
                   std::string_view expression,
                   SourceLocation where,
                   std::string_view message = {}) {
    const bool ok = detail::lessValues(actual, bound) || detail::equalValues(actual, bound);
    AssertionEngine::report(ok,
                            std::string(expression),
                            "<= " + detail::display(bound),
                            detail::display(actual),
                            std::string(message),
                            where);
}

template<typename A, typename B>
void isGreater(const A& actual,
               const B& bound,
               std::string_view expression,
               SourceLocation where,
               std::string_view message = {}) {
    AssertionEngine::report(detail::lessValues(bound, actual),
                            std::string(expression),
                            "> " + detail::display(bound),
                            detail::display(actual),
                            std::string(message),
                            where);
}

template<typename A, typename B>
void isGreaterOrEqual(const A& actual,
                      const B& bound,
                      std::string_view expression,
                      SourceLocation where,
                      std::string_view message = {}) {
    const bool ok = detail::lessValues(bound, actual) || detail::equalValues(actual, bound);
    AssertionEngine::report(ok,
                            std::string(expression),
                            ">= " + detail::display(bound),
                            detail::display(actual),
                            std::string(message),
                            where);
}

void containsSubstring(std::string_view haystack,
                       std::string_view needle,
                       std::string_view expression,
                       SourceLocation where,
                       std::string_view message = {});

void doesNotContainSubstring(std::string_view haystack,
                             std::string_view needle,
                             std::string_view expression,
                             SourceLocation where,
                             std::string_view message = {});

void matchesGlob(std::string_view text,
                 std::string_view pattern,
                 std::string_view expression,
                 SourceLocation where,
                 std::string_view message = {});

void isNear(double actual,
            double expected,
            double tolerance,
            std::string_view expression,
            SourceLocation where,
            std::string_view message = {});

/// HTTP status assertion with a message that names the endpoint, because a
/// bare "expected 200 got 500" costs the reader a trip to the logs.
void statusCode(int actual,
                int expected,
                std::string_view endpoint,
                SourceLocation where,
                std::string_view message = {});

/// Accepts any of several statuses, e.g. {200, 204}.
void statusCodeIn(int actual,
                  const std::vector<int>& allowed,
                  std::string_view endpoint,
                  SourceLocation where,
                  std::string_view message = {});

/// Latency budget. Reported in milliseconds with the overshoot spelled out.
void responseTime(Milliseconds actual,
                  Milliseconds budget,
                  std::string_view endpoint,
                  SourceLocation where,
                  std::string_view message = {});

/// Asserts that a JSON document has a member at `dottedPath`.
void jsonHasField(const json::Value& document,
                  std::string_view dottedPath,
                  SourceLocation where,
                  std::string_view message = {});

/// Asserts a JSON field exists and equals `expected`.
void jsonFieldEquals(const json::Value& document,
                     std::string_view dottedPath,
                     const json::Value& expected,
                     SourceLocation where,
                     std::string_view message = {});

[[noreturn]] void fail(std::string_view message, SourceLocation where);

}  // namespace assertions
}  // namespace testforge

// ---------------------------------------------------------------------------
// Macros
//
// The canonical spelling is TF_ASSERT_*. GoogleTest owns ASSERT_* in any
// translation unit that includes gtest, and silently shadowing those would be
// a trap; the unprefixed aliases live in ShortAssertions.hpp for suites that
// do not use GoogleTest.
// ---------------------------------------------------------------------------

#define TF_ASSERT_TRUE(expr) \
    ::testforge::assertions::isTrue((expr), #expr, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_FALSE(expr) \
    ::testforge::assertions::isFalse((expr), #expr, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_EQ(actual, expected) \
    ::testforge::assertions::areEqual( \
        (actual), (expected), #actual " == " #expected, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_NE(actual, forbidden)   \
    ::testforge::assertions::areNotEqual( \
        (actual), (forbidden), #actual " != " #forbidden, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_LT(actual, bound)  \
    ::testforge::assertions::isLess( \
        (actual), (bound), #actual " < " #bound, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_LE(actual, bound)         \
    ::testforge::assertions::isLessOrEqual( \
        (actual), (bound), #actual " <= " #bound, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_GT(actual, bound)     \
    ::testforge::assertions::isGreater( \
        (actual), (bound), #actual " > " #bound, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_GE(actual, bound)            \
    ::testforge::assertions::isGreaterOrEqual( \
        (actual), (bound), #actual " >= " #bound, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_CONTAINS(haystack, needle)    \
    ::testforge::assertions::containsSubstring( \
        (haystack), (needle), #haystack " contains " #needle, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_NOT_CONTAINS(haystack, needle)      \
    ::testforge::assertions::doesNotContainSubstring( \
        (haystack), (needle), #haystack " excludes " #needle, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_MATCHES(text, pattern)  \
    ::testforge::assertions::matchesGlob( \
        (text), (pattern), #text " matches " #pattern, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_NEAR(actual, expected, tolerance) \
    ::testforge::assertions::isNear(                \
        (actual), (expected), (tolerance), #actual " ~= " #expected, TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_STATUS_CODE(response, expected)                    \
    ::testforge::assertions::statusCode((response).statusCode,       \
                                        (expected),                  \
                                        (response).describeTarget(), \
                                        TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_STATUS_CODE_IN(response, allowed)                    \
    ::testforge::assertions::statusCodeIn((response).statusCode,       \
                                          (allowed),                   \
                                          (response).describeTarget(), \
                                          TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_RESPONSE_TIME(response, budgetMs)                              \
    ::testforge::assertions::responseTime((response).elapsed,                    \
                                          ::testforge::Milliseconds{(budgetMs)}, \
                                          (response).describeTarget(),           \
                                          TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_JSON_FIELD(document, path) \
    ::testforge::assertions::jsonHasField((document), (path), TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_JSON_EQ(document, path, expected) \
    ::testforge::assertions::jsonFieldEquals(       \
        (document), (path), ::testforge::json::Value(expected), TESTFORGE_CURRENT_LOCATION())

#define TF_FAIL(message) ::testforge::assertions::fail((message), TESTFORGE_CURRENT_LOCATION())

#define TF_ASSERT_THROWS(statement, ExceptionType)                                         \
    do {                                                                                   \
        bool tfCaught = false;                                                             \
        try {                                                                              \
            statement;                                                                     \
        } catch (const ExceptionType&) {                                                   \
            tfCaught = true;                                                               \
        } catch (...) {                                                                    \
            tfCaught = false;                                                              \
        }                                                                                  \
        ::testforge::assertions::isTrue(                                                   \
            tfCaught, #statement " throws " #ExceptionType, TESTFORGE_CURRENT_LOCATION()); \
    } while (false)

#define TF_ASSERT_NO_THROW(statement)                                                     \
    do {                                                                                  \
        bool tfThrew = false;                                                             \
        std::string tfWhat;                                                               \
        try {                                                                             \
            statement;                                                                    \
        } catch (const std::exception& tfError) {                                         \
            tfThrew = true;                                                               \
            tfWhat = tfError.what();                                                      \
        } catch (...) {                                                                   \
            tfThrew = true;                                                               \
            tfWhat = "unknown exception";                                                 \
        }                                                                                 \
        ::testforge::assertions::isFalse(                                                 \
            tfThrew, #statement " does not throw", TESTFORGE_CURRENT_LOCATION(), tfWhat); \
    } while (false)
