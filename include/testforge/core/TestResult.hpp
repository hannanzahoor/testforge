#pragma once

#include "testforge/core/Clock.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/Status.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace testforge {

/// The record of one test execution.
///
/// This is the central data structure of the whole system: the runner produces
/// it, the classifier annotates it, the repository persists it, the reporters
/// render it and the AI layer explains it. Everything downstream of execution
/// consumes only this — which is what keeps those layers independent of how
/// tests are actually run.
struct TestResult {
    std::string testId;     ///< stable hash of suite+name; survives across runs
    std::string testName;   ///< "health_check"
    std::string suiteName;  ///< "api"
    std::string runId;

    TestStatus status = TestStatus::Error;
    FailureCategory failureCategory = FailureCategory::Unknown;

    TimePoint startTime;
    TimePoint endTime;
    Milliseconds duration{0};

    std::string errorMessage;  ///< one line, suitable for a summary table
    std::string errorDetail;   ///< multi-line: expected/actual, location, body

    std::vector<std::string> logs;  ///< log tail captured during this test
    std::vector<std::string> tags;

    /// Test-supplied facts: request URL, status code, response time, GPU name.
    json::Value metadata = json::Value::object();

    /// System state captured at failure time by DiagnosticCollector.
    json::Value diagnostics = json::Value::object();

    int attempt = 1;              ///< 1-based; > 1 means this was a retry
    bool flakyCandidate = false;  ///< passed on a retry after failing
    std::string worker;           ///< which worker thread executed it

    [[nodiscard]] std::string qualifiedName() const;

    [[nodiscard]] bool passed() const noexcept { return status == TestStatus::Passed; }

    [[nodiscard]] json::Value toJson() const;

    static TestResult fromJson(const json::Value& value);

    /// Convenience constructor used by the runner and by tests.
    static TestResult make(std::string suite, std::string name, TestStatus status);
};

/// Aggregate numbers for a set of results.
struct RunStatistics {
    int total = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    int errors = 0;
    int timeouts = 0;

    Milliseconds totalDuration{0};  ///< sum of test durations (not wall clock)
    Milliseconds wallDuration{0};   ///< actual elapsed time of the run
    Milliseconds averageDuration{0};
    Milliseconds medianDuration{0};
    Milliseconds p95Duration{0};
    Milliseconds maxDuration{0};
    std::string slowestTest;

    std::map<FailureCategory, int> failureCategoryCounts;

    /// Passed / (total - skipped), as a percentage. Skipped tests are excluded
    /// because counting them as failures punishes correct behaviour on a
    /// machine without a GPU, and counting them as passes overstates coverage.
    [[nodiscard]] double successRate() const;

    /// Speed-up available from parallelism: sum(durations) / wall clock.
    /// Meaningless (returns 0) when the wall clock was not measured.
    [[nodiscard]] double parallelEfficiency() const;

    [[nodiscard]] json::Value toJson() const;

    static RunStatistics compute(const std::vector<TestResult>& results,
                                 Milliseconds wallDuration = Milliseconds{0});
};

/// One invocation of the runner: the results plus the context they were
/// produced in.
struct TestRun {
    std::string runId;
    std::string label;  ///< free text, e.g. "nightly regression"
    TimePoint startedAt;
    TimePoint finishedAt;
    Milliseconds duration{0};

    int workers = 1;
    std::string filterDescription;  ///< "--suite smoke --tag api"
    std::string gitCommit;
    std::string hostname;

    /// Environment captured once per run rather than per result: it is the
    /// same for every test and duplicating it would bloat the database.
    json::Value environment = json::Value::object();

    std::vector<TestResult> results;

    /// Set when the run stopped early: Ctrl-C, SIGTERM, or fail-fast. Empty
    /// when the run completed on its own terms.
    ///
    /// This is not cosmetic. A run that was cut short has not verified the
    /// tests it never reached, so reporting it as a pass — which is what a
    /// zero exit code means to CI — would be a false statement about code
    /// nobody tested.
    std::string cancellationReason;

    [[nodiscard]] bool wasCancelled() const noexcept { return !cancellationReason.empty(); }

    [[nodiscard]] RunStatistics statistics() const;

    [[nodiscard]] json::Value toJson() const;

    /// Serialises without the per-test results, for run listings.
    [[nodiscard]] json::Value toSummaryJson() const;

    static TestRun fromJson(const json::Value& value);

    /// Process exit code convention: 0 clean, 1 test failures, 2 framework or
    /// configuration error, 4 cancelled before completion. CI depends on this
    /// distinction.
    [[nodiscard]] int exitCode() const;
};

}  // namespace testforge
