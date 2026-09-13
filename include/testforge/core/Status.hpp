#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace testforge {

/// Outcome of a single test execution.
///
/// The set is deliberately small and closed: every execution maps onto exactly
/// one of these, which is what makes aggregate reporting and SQL analytics
/// straightforward.
enum class TestStatus : std::uint8_t {
    Passed = 0,
    Failed,   ///< the test ran and an assertion did not hold
    Skipped,  ///< preconditions not met (e.g. no NVIDIA GPU present)
    Error,    ///< could not complete for a reason that is not a test failure
    Timeout,  ///< the deadline expired before the test finished
};

/// Why a test did not pass.
///
/// This is the primary triage axis: it answers "whose problem is this?" before
/// anyone reads a log. Categories are assigned by FailureClassifier from the
/// exception type, the transport-level error, and the diagnostics collected at
/// failure time.
enum class FailureCategory : std::uint8_t {
    None = 0,              ///< the test passed
    AssertionFailure,      ///< the system under test behaved incorrectly
    Timeout,               ///< deadline exceeded
    NetworkFailure,        ///< connect/DNS/reset — the SUT was unreachable
    EnvironmentFailure,    ///< the machine, not the SUT, is at fault
    ResourceFailure,       ///< out of memory / disk / file descriptors
    ApplicationFailure,    ///< the SUT returned 5xx or crashed
    DependencyFailure,     ///< a required service or binary is missing
    ConfigurationFailure,  ///< invalid or missing configuration
    FrameworkError,        ///< a bug in TestForge itself
    Unknown,
};

std::string_view toString(TestStatus status) noexcept;

std::string_view toString(FailureCategory category) noexcept;

std::optional<TestStatus> testStatusFromString(std::string_view text) noexcept;

std::optional<FailureCategory> failureCategoryFromString(std::string_view text) noexcept;

/// All enumerators in declaration order. Reporting and the CLI's help output
/// iterate these so the lists can never drift from the enum.
const std::vector<TestStatus>& allTestStatuses();

const std::vector<FailureCategory>& allFailureCategories();

/// True when the status represents a run a developer must look at.
constexpr bool isFailure(TestStatus status) noexcept {
    return status == TestStatus::Failed || status == TestStatus::Error ||
           status == TestStatus::Timeout;
}

/// ANSI colour escape for terminal reporting. Callers decide whether to use it.
std::string_view ansiColor(TestStatus status) noexcept;

inline constexpr std::string_view kAnsiReset = "\033[0m";
inline constexpr std::string_view kAnsiBold = "\033[1m";
inline constexpr std::string_view kAnsiDim = "\033[2m";

}  // namespace testforge
