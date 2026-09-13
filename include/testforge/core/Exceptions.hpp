#pragma once

#include "testforge/core/SourceLocation.hpp"
#include "testforge/core/Status.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace testforge {

/// Base of every exception TestForge throws on purpose.
///
/// Carrying the FailureCategory on the exception is what lets the runner
/// classify a failure without inspecting exception type names at the call
/// site; see FailureClassifier.
class TestForgeError : public std::runtime_error {
 public:
    TestForgeError(std::string message, FailureCategory category)
        : std::runtime_error(std::move(message)), category_(category) {}

    [[nodiscard]] FailureCategory category() const noexcept { return category_; }

 private:
    FailureCategory category_;
};

/// An assertion did not hold. This is an *expected* outcome of running tests
/// and is never treated as a framework fault.
class AssertionFailure : public TestForgeError {
 public:
    AssertionFailure(std::string message,
                     std::string expression,
                     std::string expected,
                     std::string actual,
                     SourceLocation where)
        : TestForgeError(std::move(message), FailureCategory::AssertionFailure),
          expression_(std::move(expression)),
          expected_(std::move(expected)),
          actual_(std::move(actual)),
          where_(where) {}

    [[nodiscard]] const std::string& expression() const noexcept { return expression_; }

    [[nodiscard]] const std::string& expected() const noexcept { return expected_; }

    [[nodiscard]] const std::string& actual() const noexcept { return actual_; }

    [[nodiscard]] const SourceLocation& where() const noexcept { return where_; }

    /// Multi-line, human-oriented rendering used by the reporters.
    [[nodiscard]] std::string detail() const;

 private:
    std::string expression_;
    std::string expected_;
    std::string actual_;
    SourceLocation where_;
};

/// Thrown by a test that has determined it cannot meaningfully run here
/// (missing hardware, disabled feature). Produces TestStatus::Skipped.
class SkipTest : public TestForgeError {
 public:
    explicit SkipTest(std::string reason)
        : TestForgeError(std::move(reason), FailureCategory::None) {}
};

/// Thrown out of a test body when cooperative cancellation is observed.
class TestCancelled : public TestForgeError {
 public:
    explicit TestCancelled(std::string reason)
        : TestForgeError(std::move(reason), FailureCategory::Timeout) {}
};

class ConfigurationError : public TestForgeError {
 public:
    explicit ConfigurationError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::ConfigurationFailure) {}
};

class EnvironmentError : public TestForgeError {
 public:
    explicit EnvironmentError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::EnvironmentFailure) {}
};

class DependencyError : public TestForgeError {
 public:
    explicit DependencyError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::DependencyFailure) {}
};

class NetworkError : public TestForgeError {
 public:
    explicit NetworkError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::NetworkFailure) {}
};

class ResourceError : public TestForgeError {
 public:
    explicit ResourceError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::ResourceFailure) {}
};

class PersistenceError : public TestForgeError {
 public:
    explicit PersistenceError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::FrameworkError) {}
};

/// A rule in the security model was violated: an unsafe URL, rejected AI
/// output, or an argument that looks like an injection attempt. Always fatal
/// for the operation that raised it.
class SecurityError : public TestForgeError {
 public:
    explicit SecurityError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::ConfigurationFailure) {}
};

/// A bug in TestForge itself, as opposed to a problem with the system under
/// test. Kept distinct so CI can alert on it differently.
class FrameworkError : public TestForgeError {
 public:
    explicit FrameworkError(std::string message)
        : TestForgeError(std::move(message), FailureCategory::FrameworkError) {}
};

}  // namespace testforge
